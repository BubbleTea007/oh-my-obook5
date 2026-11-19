#!/usr/bin/env python3
"""
Fake the OBOOK5 online-update endpoint and serve the custom OTA package.

This script combines the OTA network setup and the package checks:

  - verifies custom_workshop.img against custom_workshop.img.md5
  - copies the package into a temporary serve directory
  - reads the package version from the image
  - adds a local IP alias for the official OTA server
  - answers *.obook.com.cn DNS queries for the target device
  - ARP-poisons only the target device's gateway entry
  - serves /readerapi.aspx and the package on ports 8085 and 80
  - restores ARP and removes the alias when stopped

The device still downloads, verifies, installs and reboots by itself. No root
shell on the device is needed.

Usage:
  python3 package_ota.py
  sudo python3 jailbreak_ota.py --target <设备IP>

If --iface/--gateway are omitted they are read from the macOS default route.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import official

ROOT = Path(__file__).resolve().parent
DEFAULT_PACKAGE = ROOT / "custom_workshop.img"
VERSION_FILE = ROOT / "custom_workshop.version"

DEFAULT_SERVE_DIR = Path("/private/tmp/obook_capture/serve")
DEFAULT_LOG = Path("/private/tmp/obook_capture/jailbreak_ota.log")
DEFAULT_STOP = Path("/private/tmp/obook_capture/stop")

sys.path.insert(0, str(ROOT / "vendor"))
try:
    from scapy.all import (  # noqa: E402
        ARP, DNS, DNSRR, Ether, IP, UDP, AsyncSniffer,
        get_if_hwaddr, sendp, srp1,
    )
except ImportError:
    ARP = DNS = DNSRR = Ether = IP = UDP = AsyncSniffer = None
    get_if_hwaddr = sendp = srp1 = None

LOG_LOCK = threading.Lock()
LOG_PATH: Path | None = None
STOP = threading.Event()


def log(message: str) -> None:
    line = f"{time.strftime('%H:%M:%S')} {message}"
    print(line, flush=True)
    if LOG_PATH:
        with LOG_LOCK:
            with LOG_PATH.open("a", encoding="utf-8") as f:
                f.write(line + "\n")


def md5(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def expected_md5(path: Path) -> str:
    text = path.read_text(encoding="utf-8", errors="replace").strip()
    match = re.search(r"\b([0-9a-fA-F]{32})\b", text)
    if not match:
        raise SystemExit(f"no MD5 digest in {path}")
    return match.group(1).lower()


def package_version(image: Path) -> str:
    try:
        with tarfile.open(image, "r:gz") as archive:
            member = archive.extractfile("image/opt/Qtopia/home/version")
            if member is not None:
                value = member.read().decode("utf-8", "replace").strip()
                if value:
                    return value
    except (tarfile.TarError, KeyError, OSError):
        pass

    if VERSION_FILE.is_file():
        value = VERSION_FILE.read_text(encoding="utf-8", errors="replace").strip()
        if value:
            return value

    raise SystemExit(
        "cannot determine package version; run python3 package_ota.py again"
    )


def trigger_version(fallback: str) -> str:
    if VERSION_FILE.is_file():
        value = VERSION_FILE.read_text(encoding="utf-8", errors="replace").strip()
        if value:
            return value
    return fallback


def default_route() -> tuple[str | None, str | None]:
    try:
        result = subprocess.run(
            ["route", "-n", "get", "default"],
            check=False, capture_output=True, text=True,
        )
    except FileNotFoundError:
        return None, None

    if result.returncode != 0:
        return None, None

    gateway = None
    iface = None
    for line in result.stdout.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        value = value.strip()
        if key.strip() == "gateway":
            gateway = value
        elif key.strip() == "interface":
            iface = value
    return iface, gateway


def prepare_serve_dir(package: Path, md5_path: Path,
                      serve_dir: Path) -> None:
    serve_dir.mkdir(parents=True, exist_ok=True)
    image = serve_dir / "official.img"
    package_md5 = serve_dir / "official.img.md5"
    log(f"copying package -> {image}")
    shutil.copyfile(package, image)
    shutil.copyfile(md5_path, package_md5)


def resolve_mac(ip: str, iface: str, timeout: float = 3.0) -> str | None:
    answer = srp1(
        Ether(dst="ff:ff:ff:ff:ff:ff") / ARP(pdst=ip),
        iface=iface, timeout=timeout, verbose=False,
    )
    return answer.hwsrc if answer else None


def make_handler(serve_dir: Path, package_name: str,
                 md5_name: str, version: str):
    class Handler(BaseHTTPRequestHandler):
        server_version = "Microsoft-IIS/8.5"
        sys_version = ""
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            log(f"HTTP {self.client_address[0]} {fmt % args}")

        def _send(self, code, body, ctype, extra=None):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "private")
            if extra:
                for key, value in extra.items():
                    self.send_header(key, value)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            log(f"GET {self.path} Host={self.headers.get('Host')} "
                f"Device={self.headers.get('Device')} "
                f"action={self.headers.get('action')}")

            if path.lower().endswith("readerapi.aspx"):
                xml = (
                    '<?xml version="1.0" encoding="UTF-8"?>'
                    "<Response><GetReaderPackage>"
                    "<Item><title>主要更新 (注意：更新完设备自动重启需要几分钟，请耐心等待！)</title>"
                    "<content>1.优化待机图显示效果。</content></Item>"
                    f"<Version>{version}</Version>"
                    "<GetPackageUrl>http://e.obook.com.cn/szdownload/1.0epd86s2_pro192.img</GetPackageUrl>"
                    "<Md5>http://e.obook.com.cn/szdownload/1.0epd86s2_pro192.img.md5</Md5>"
                    "</GetReaderPackage></Response>"
                )
                body = gzip.compress(xml.encode("utf-8"), mtime=0)
                self.send_response(200)
                self.send_header("Cache-Control", "private")
                self.send_header("Content-Type", "application/xml; charset=utf-8")
                self.send_header("Content-Encoding", "gzip")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Set-Cookie",
                                 "ASP.NET_SessionId=fakefakefakefakefake; "
                                 "path=/; HttpOnly; SameSite=Lax")
                self.send_header("TimeStamp", time.strftime("%Y/%m/%d %H:%M:%S"))
                self.send_header("result-code", "0")
                self.send_header("X-AspNet-Version", "4.0.30319")
                self.send_header("X-Powered-By", "ASP.NET")
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(body)
                return

            if path.endswith(".img.md5") or path == f"/{md5_name}":
                try:
                    data = (serve_dir / md5_name).read_bytes()
                    self._send(200, data, "text/plain")
                except FileNotFoundError:
                    self._send(404, "not found", "text/plain")
                return

            if path.endswith(".img") or path == f"/{package_name}":
                fpath = serve_dir / package_name
                try:
                    size = fpath.stat().st_size
                    self.send_response(200)
                    self.send_header("Content-Type", "application/zip")
                    self.send_header("Content-Length", str(size))
                    self.send_header("Cache-Control", "private")
                    self.end_headers()
                    if self.command != "HEAD":
                        with fpath.open("rb") as f:
                            shutil.copyfileobj(f, self.wfile, 1024 * 256)
                    log(f"SERVED {package_name} {size} bytes")
                except FileNotFoundError:
                    self._send(404, "not found", "text/plain")
                return

            self._send(404, "not found", "text/plain")

    return Handler


def start_http_servers(serve_dir: Path, package_name: str,
                       md5_name: str, version: str, port: int):
    handler = make_handler(serve_dir, package_name, md5_name, version)
    servers = []
    for listen_port in sorted({port, 80}):
        try:
            httpd = ThreadingHTTPServer(("0.0.0.0", listen_port), handler)
            httpd.daemon_threads = True
            threading.Thread(target=httpd.serve_forever, daemon=True).start()
            servers.append(httpd)
            log(f"[*] HTTP server on 0.0.0.0:{listen_port}")
        except Exception as exc:
            log(f"[!] HTTP server on port {listen_port} failed: {exc}")
    return servers


def run_server(args, iface: str, gateway: str, version: str) -> int:
    if AsyncSniffer is None:
        log("[!] scapy is not installed; run: pip3 install -r requirements.txt")
        return 1
    try:
        my_mac = get_if_hwaddr(iface)
    except Exception as exc:
        log(f"[!] cannot read MAC for {iface}: {exc}")
        return 1
    target_mac = args.target_mac or resolve_mac(args.target, iface)
    if not target_mac:
        for _ in range(3):
            time.sleep(1)
            target_mac = resolve_mac(args.target, iface)
            if target_mac:
                break
    gateway_mac = resolve_mac(gateway, iface)
    if not target_mac or not gateway_mac:
        log(f"[!] cannot resolve MACs target={target_mac} gateway={gateway_mac}")
        return 1

    log(f"[*] iface={iface} my={my_mac} "
        f"target={args.target}/{target_mac} gw={gateway}/{gateway_mac}")
    log(f"[*] alias={args.alias} port={args.port} version={version}")

    subprocess.run(
        ["/sbin/ifconfig", iface, "alias", args.alias, "netmask", "255.255.255.255"],
        check=False,
    )
    log(f"[*] added alias {args.alias} on {iface}")

    stop_threads = threading.Event()

    poison = ARP(op=2, pdst=args.target, hwdst=target_mac,
                 psrc=gateway, hwsrc=my_mac)

    def poison_loop():
        while not stop_threads.is_set():
            sendp(Ether(dst=target_mac) / poison, iface=iface, verbose=False)
            stop_threads.wait(1.0)

    threading.Thread(target=poison_loop, daemon=True).start()

    def handle_dns(pkt):
        try:
            if not pkt.haslayer(DNS) or pkt[DNS].qr != 0 or pkt[DNS].qd is None:
                return
            if not pkt.haslayer(UDP):
                return
            qname = pkt[DNS].qd.qname.decode(errors="replace").rstrip(".")
            if not qname.lower().endswith("obook.com.cn"):
                return
            qtype = pkt[DNS].qd.qtype
            if qtype == 1:
                answer = DNSRR(rrname=pkt[DNS].qd.qname, type="A",
                               ttl=60, rdata=args.alias)
            else:
                answer = None
            response = (
                IP(src=gateway, dst=args.target)
                / UDP(sport=53, dport=pkt[UDP].sport)
                / DNS(id=pkt[DNS].id, qr=1, aa=1, qd=pkt[DNS].qd,
                      an=answer, ancount=1 if answer else 0)
            )
            sendp(Ether(src=my_mac, dst=target_mac) / response,
                  iface=iface, verbose=False)
            log(f"DNS {qname} type={qtype} -> "
                f"{args.alias if answer else 'NODATA'}")
        except Exception as exc:
            log(f"DNS error: {exc}")

    dns_sniffer = AsyncSniffer(
        iface=iface,
        filter=f"udp port 53 and src host {args.target}",
        prn=handle_dns,
        store=False,
    )
    dns_sniffer.start()
    log("[*] DNS responder started")

    traffic_count = 0

    def handle_traffic(pkt):
        nonlocal traffic_count
        try:
            if not pkt.haslayer(IP) or pkt[IP].src != args.target:
                return
            ip = pkt[IP]
            if pkt.haslayer(TCP):
                tcp = pkt[TCP]
                if tcp.flags & 0x02:
                    traffic_count += 1
                    if traffic_count <= 60:
                        log(f"TRAFFIC TCP SYN {ip.src}:{tcp.sport} -> "
                            f"{ip.dst}:{tcp.dport}")
            elif pkt.haslayer(UDP) and pkt[UDP].dport != 53:
                traffic_count += 1
                if traffic_count <= 60:
                    log(f"TRAFFIC UDP {ip.src}:{pkt[UDP].sport} -> "
                        f"{ip.dst}:{pkt[UDP].dport}")
        except Exception:
            pass

    traffic_sniffer = AsyncSniffer(
        iface=iface,
        filter=f"src host {args.target}",
        prn=handle_traffic,
        store=False,
    )
    traffic_sniffer.start()
    log("[*] traffic logger started")

    package_name = "official.img"
    md5_name = "official.img.md5"
    servers = start_http_servers(
        args.serve_dir, package_name, md5_name, version, args.port)

    try:
        start = time.time()
        while time.time() - start < args.duration:
            if STOP.is_set() or args.stop_file.exists():
                log("[*] stop requested")
                break
            time.sleep(0.5)
    finally:
        stop_threads.set()
        time.sleep(0.5)
        for sniffer in (dns_sniffer, traffic_sniffer):
            try:
                sniffer.stop()
            except Exception:
                pass
        for httpd in servers:
            try:
                httpd.shutdown()
                httpd.server_close()
            except Exception:
                pass

        # Restore the target's view of the gateway.
        for _ in range(5):
            try:
                sendp(
                    Ether(dst=target_mac) / ARP(
                        op=2, pdst=args.target, hwdst=target_mac,
                        psrc=gateway, hwsrc=gateway_mac,
                    ), iface=iface, verbose=False,
                )
                sendp(
                    Ether(dst=gateway_mac) / ARP(
                        op=2, pdst=gateway, hwdst=gateway_mac,
                        psrc=args.target, hwsrc=target_mac,
                    ), iface=iface, verbose=False,
                )
            except Exception:
                pass
            time.sleep(0.2)
        subprocess.run(["/sbin/ifconfig", iface, "-alias", args.alias],
                       check=False)
        log("[*] restored ARP, removed alias, exiting")

    return 0


def install_signal_handlers() -> None:
    try:
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, lambda *_: STOP.set())
        signal.signal(signal.SIGINT, lambda *_: STOP.set())
    except Exception:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Serve the custom workshop OTA package to one device")
    parser.add_argument("--target", required=True,
                        help="device IP on the same LAN")
    parser.add_argument("--target-mac", default=None,
                        help="device MAC (optional; resolved with ARP)")
    parser.add_argument("--iface", default=None,
                        help="Mac interface (default: from the default route)")
    parser.add_argument("--gateway", default=None,
                        help="LAN gateway (default: from the default route)")
    parser.add_argument("--package", type=Path, default=None,
                        help="package to serve (default: custom_workshop.img)")
    parser.add_argument("--md5-file", type=Path, default=None,
                        help="package MD5 file (default: <package>.md5)")
    parser.add_argument("--official", action="store_true",
                        help="restore official V1.8.1 (downloads if needed)")
    parser.add_argument("--serve-dir", type=Path, default=DEFAULT_SERVE_DIR)
    parser.add_argument("--version", default=None,
                        help="API version (default: package version; "
                             "--official uses V9.9.9)")
    parser.add_argument("--duration", type=int, default=900,
                        help="serve for N seconds (default: 900)")
    parser.add_argument("--alias", default="112.124.108.210",
                        help="fake OTA server IP (default: 112.124.108.210)")
    parser.add_argument("--port", type=int, default=8085,
                        help="API port (default: 8085; port 80 is also bound)")
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--stop-file", type=Path, default=DEFAULT_STOP)
    parser.add_argument("--dry-run", action="store_true",
                        help="validate the package and print the plan")
    args = parser.parse_args()

    if args.official:
        if args.package:
            package = args.package
            md5_path = args.md5_file or Path(str(package) + ".md5")
        else:
            package, md5_path = official.ensure_official_image()
        api_version = args.version or "V99.0.0"
    else:
        package = args.package or DEFAULT_PACKAGE
        md5_path = args.md5_file or Path(str(package) + ".md5")
        api_version = args.version

    if not package.is_file():
        raise SystemExit(f"missing package: {package}")
    if not md5_path.is_file():
        raise SystemExit(f"missing checksum: {md5_path}")

    actual = md5(package)
    expected = expected_md5(md5_path)
    if actual != expected:
        raise SystemExit(
            f"package MD5 mismatch:\n  image: {actual}\n  .md5:  {expected}"
        )

    packaged_version = package_version(package)
    if api_version is None:
        api_version = trigger_version(packaged_version)
    if args.version and not args.official and VERSION_FILE.is_file():
        packaged = VERSION_FILE.read_text(encoding="utf-8").strip()
        if packaged and packaged != args.version:
            log(f"[!] warning: --version {args.version} != package {packaged}")

    iface, gateway = default_route()
    iface = args.iface or iface
    gateway = args.gateway or gateway
    if not iface:
        raise SystemExit("cannot detect interface; pass --iface en0")
    if not gateway:
        raise SystemExit("cannot detect gateway; pass --gateway <网关IP>")

    log(f"package: {package} ({package.stat().st_size} bytes)")
    log(f"package version: {packaged_version}")
    log(f"API version:     {api_version}")
    log(f"target:  {args.target}")
    log(f"route:   {iface} gateway {gateway}")
    log(f"serve:   {args.serve_dir}")

    if args.dry_run:
        log("dry run; no network changes")
        return 0

    if os.geteuid() != 0:
        log("[!] root is required for IP alias, ARP/DNS and port 80.")
        log("    re-run with sudo:")
        log("    sudo " + shlex.join(
            [sys.executable, str(Path(__file__).resolve())] + sys.argv[1:]))
        return 1

    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.stop_file.parent.mkdir(parents=True, exist_ok=True)
    args.stop_file.unlink(missing_ok=True)

    global LOG_PATH
    LOG_PATH = args.log
    LOG_PATH.write_text("", encoding="utf-8")

    prepare_serve_dir(package, md5_path, args.serve_dir)
    install_signal_handlers()

    log("starting OTA server; on the device choose 设置 -> 检查更新")
    log("stop with Ctrl-C or: touch " + str(args.stop_file))
    return run_server(args, iface, gateway, api_version)


if __name__ == "__main__":
    raise SystemExit(main())
