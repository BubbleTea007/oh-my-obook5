#pragma once

// OBOOK5 WiFiClass backed by the stock wpa_supplicant.
//
// The original simulator header faked scan results and connection state.  The
// device runs wpa_supplicant on wlan0 (config /home/wpa_supplicant.conf) and a
// Qtopia helper, so the Arduino-style API is implemented by driving wpa_cli
// and reading the kernel's network state.  CrossMux keeps its own credential
// store; wpa_cli only performs the join.

#include "IPAddress.h"
#include "NetworkClient.h"
#include "WString.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#define WIFI_SCAN_RUNNING -1
#define WIFI_SCAN_FAILED -2

using arduino_event_id_t = int;
using arduino_event_info_t = int;
inline constexpr arduino_event_id_t ARDUINO_EVENT_WIFI_STA_START = 0;
inline constexpr arduino_event_id_t ARDUINO_EVENT_WIFI_AP_START = 1;

using WiFiClient = NetworkClient;

enum wl_status_t {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_DISCONNECTED = 6
};

enum wifi_mode_t {
  WIFI_OFF = 0,
  WIFI_STA = 1,
  WIFI_AP = 2,
  WIFI_AP_STA = 3,
  WIFI_MODE_NULL = 0
};
enum wifi_auth_mode_t {
  WIFI_AUTH_OPEN = 0,
  WIFI_AUTH_WEP = 1,
  WIFI_AUTH_WPA_PSK = 2,
  WIFI_AUTH_WPA2_PSK = 3,
  WIFI_AUTH_WPA_WPA2_PSK = 4
};
enum wifi_scan_method_t { WIFI_FAST_SCAN = 0, WIFI_ALL_CHANNEL_SCAN };
enum wifi_sort_method_t {
  WIFI_CONNECT_AP_BY_SIGNAL = 0,
  WIFI_CONNECT_AP_BY_SECURITY
};

#define WIFI_MODE_STA WIFI_STA
#define WIFI_MODE_AP WIFI_AP

class WiFiClass {
  struct Network {
    String ssid;
    int32_t rssi = 0;
    wifi_auth_mode_t auth = WIFI_AUTH_OPEN;
  };

  wifi_mode_t currentMode = WIFI_OFF;
  wl_status_t currentStatus = WL_DISCONNECTED;
  String currentSsid;
  String currentIp;
  String currentBssidText;
  int currentFreq = 0;
  mutable int currentRssi = 0;
  std::vector<Network> networks;

  bool scanInProgress = false;
  bool dhcpStarted = false;
  bool forceStatusRefresh = true;
  bool connecting = false;
  std::chrono::steady_clock::time_point connectStarted;
  std::chrono::steady_clock::time_point scanStarted;
  std::chrono::steady_clock::time_point lastScanPoll;
  std::chrono::steady_clock::time_point lastStatusPoll;

  static constexpr const char *kWpaCli = "/usr/sbin/wpa_cli";
  static constexpr int kStatusPollMs = 500;
  static constexpr int kScanSettleMs = 3500;

  static std::string run(const std::string &command) {
    std::string output;
    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe) return output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    pclose(pipe);
    return output;
  }

  static std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                              value.back() == ' ' || value.back() == '\t'))
      value.pop_back();
    size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t'))
      ++start;
    return value.substr(start);
  }

  static std::string shellQuote(const std::string &value) {
    std::string out = "'";
    for (char c : value) {
      if (c == '\'')
        out += "'\\''";
      else
        out += c;
    }
    out += "'";
    return out;
  }

  static std::string escapeWpa(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
      if (c == '\\' || c == '"') out += '\\';
      out += c;
    }
    return out;
  }

  static std::string unescapeWpa(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\\' && i + 3 < value.size() && value[i + 1] == 'x') {
        auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        const int hi = hex(value[i + 2]);
        const int lo = hex(value[i + 3]);
        if (hi >= 0 && lo >= 0) {
          out += static_cast<char>((hi << 4) | lo);
          i += 3;
          continue;
        }
      }
      out += value[i];
    }
    return out;
  }

  static int parseInt(const std::string &value) {
    return static_cast<int>(strtol(value.c_str(), nullptr, 10));
  }

  static int channelFromFreq(const int freq) {
    if (freq == 2484) return 14;
    if (freq >= 2412 && freq <= 2472) return (freq - 2407) / 5;
    if (freq >= 5000 && freq <= 5900) return (freq - 5000) / 5;
    return 0;
  }

  void refreshStatus() {
    const auto now = std::chrono::steady_clock::now();
    if (!forceStatusRefresh &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                              lastStatusPoll)
                .count() < kStatusPollMs)
      return;
    lastStatusPoll = now;
    forceStatusRefresh = false;
    if (connecting &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                              connectStarted)
                .count() > 30000)
      connecting = false;

    const std::string out =
        run(std::string(kWpaCli) + " -i wlan0 status 2>/dev/null");
    std::string state;
    std::string ssid;
    std::string ip;
    std::string bssid;
    std::string freq;

    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = line.substr(0, eq);
      const std::string value = trim(line.substr(eq + 1));
      if (key == "wpa_state") state = value;
      else if (key == "ssid") ssid = unescapeWpa(value);
      else if (key == "ip_address") ip = value;
      else if (key == "bssid") bssid = value;
      else if (key == "freq") freq = value;
    }

    currentSsid = String(ssid.c_str());
    currentIp = String(ip.c_str());
    currentBssidText = String(bssid.c_str());
    currentFreq = parseInt(freq);

    if (state == "COMPLETED") {
      currentStatus = ip.empty() ? WL_IDLE_STATUS : WL_CONNECTED;
      currentMode = WIFI_STA;
      connecting = false;
      if (!dhcpStarted) {
        dhcpStarted = true;
        // Always take a fresh lease after a join.  The stock helper can leave
        // an old address/route cached across associations, which looks
        // connected but has no gateway path.
        (void)system("/sbin/udhcpc -i wlan0 -n -q -t 3 -T 2 "
                     ">/dev/null 2>&1 &");
      }
    } else if (state.find("DISCONNECTED") != std::string::npos ||
               state.empty()) {
      currentStatus = WL_DISCONNECTED;
      dhcpStarted = false;
    } else {
      currentStatus = WL_IDLE_STATUS;
      currentMode = WIFI_STA;
    }
  }

  int parseScanResults() {
    const std::string out =
        run(std::string(kWpaCli) + " -i wlan0 scan_results 2>/dev/null");
    if (out.find("FAIL") == 0) return -1;

    std::vector<Network> parsed;
    std::istringstream stream(out);
    std::string line;
    bool header = true;
    while (std::getline(stream, line)) {
      if (header) {
        header = false;
        if (line.rfind("bssid", 0) == 0) continue;
      }
      if (line.empty()) continue;

      std::vector<std::string> fields;
      size_t start = 0;
      while (true) {
        const size_t tab = line.find('\t', start);
        fields.push_back(line.substr(
            start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
      }
      if (fields.size() < 5) continue;

      Network network;
      network.ssid = String(unescapeWpa(fields[4]).c_str());
      if (network.ssid.isEmpty()) continue;
      network.rssi = parseInt(fields[2]);
      const std::string &flags = fields[3];
      network.auth = (flags.find("WPA") != std::string::npos ||
                      flags.find("RSN") != std::string::npos ||
                      flags.find("WEP") != std::string::npos)
                         ? WIFI_AUTH_WPA2_PSK
                         : WIFI_AUTH_OPEN;

      bool merged = false;
      for (auto &existing : parsed) {
        if (existing.ssid == network.ssid) {
          if (network.rssi > existing.rssi) existing = network;
          merged = true;
          break;
        }
      }
      if (!merged) parsed.push_back(std::move(network));
    }

    std::sort(parsed.begin(), parsed.end(),
              [](const Network &a, const Network &b) {
                return a.rssi > b.rssi;
              });
    networks = std::move(parsed);
    return static_cast<int>(networks.size());
  }

  void refreshRssi() {
    if (currentStatus != WL_CONNECTED) {
      currentRssi = 0;
      return;
    }
    const std::string out =
        run(std::string(kWpaCli) + " -i wlan0 signal_poll 2>/dev/null");
    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
      if (line.rfind("RSSI=", 0) == 0)
        currentRssi = parseInt(line.substr(5));
    }
  }

public:
  template <typename Callback>
  void onEvent(Callback &&, arduino_event_id_t) {}

  wl_status_t begin(const char *ssid = nullptr, const char *pass = nullptr) {
    refreshStatus();
    if (ssid && *ssid && currentStatus == WL_CONNECTED && currentSsid == ssid)
      return WL_CONNECTED;
    currentMode = WIFI_STA;
    currentSsid = ssid ? ssid : "";
    currentStatus = WL_IDLE_STATUS;
    forceStatusRefresh = true;
    dhcpStarted = false;
    connecting = true;
    connectStarted = std::chrono::steady_clock::now();

    const std::string add = run(std::string(kWpaCli) + " -i wlan0 add_network");
    int networkId = -1;
    {
      std::istringstream stream(add);
      std::string line;
      while (std::getline(stream, line)) {
        const std::string value = trim(line);
        if (!value.empty() && value[0] >= '0' && value[0] <= '9')
          networkId = parseInt(value);
      }
    }
    if (networkId < 0) {
      currentStatus = WL_CONNECT_FAILED;
      return currentStatus;
    }

    const std::string id = std::to_string(networkId);
    const std::string ssidArg =
        "\"" + escapeWpa(currentSsid.s) + "\"";
    run(std::string(kWpaCli) + " -i wlan0 set_network " + id + " ssid " +
        shellQuote(ssidArg));

    if (pass && *pass) {
      const std::string pskArg = "\"" + escapeWpa(pass) + "\"";
      run(std::string(kWpaCli) + " -i wlan0 set_network " + id + " psk " +
          shellQuote(pskArg));
    } else {
      run(std::string(kWpaCli) + " -i wlan0 set_network " + id +
          " key_mgmt NONE");
    }

    run(std::string(kWpaCli) + " -i wlan0 enable_network " + id);
    run(std::string(kWpaCli) + " -i wlan0 select_network " + id);
    run(std::string(kWpaCli) + " -i wlan0 save_config");
    return currentStatus;
  }

  wl_status_t status() {
    refreshStatus();
    return currentStatus;
  }

  IPAddress localIP() {
    refreshStatus();
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(currentIp.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
      return IPAddress(static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                       static_cast<uint8_t>(c), static_cast<uint8_t>(d));
    return IPAddress();
  }

  void persistent(bool) {}
  void disconnect(bool wifioff = false, bool eraseap = false) {
    (void)wifioff;
    (void)eraseap;
    // CrossMux's ESP32 UI disconnects before scanning because the ESP32 radio
    // cannot scan reliably while associated.  Linux can, and the OBOOK5's
    // system connection is shared with Qtopia, so only abort an in-flight
    // CrossMux join.  A plain disconnect() from the scan screen is a no-op.
    if (!connecting) return;
    connecting = false;
    run(std::string(kWpaCli) + " -i wlan0 disconnect");
    currentStatus = WL_DISCONNECTED;
    dhcpStarted = false;
    forceStatusRefresh = true;
  }

  bool mode(int mode) {
    if (mode == WIFI_OFF) {
      // The OS owns wlan0.  Leaving the shared connection alive is what makes
      // the app and the system agree on network state; only abort a join that
      // CrossMux itself started.
      if (connecting) {
        connecting = false;
        run(std::string(kWpaCli) + " -i wlan0 disconnect");
      }
      currentMode = currentStatus == WL_CONNECTED ? WIFI_STA : WIFI_OFF;
      return true;
    }
    currentMode = static_cast<wifi_mode_t>(mode);
    if (mode == WIFI_STA) {
      (void)system("/sbin/ifconfig wlan0 up >/dev/null 2>&1");
      return true;
    }
    return false;
  }

  bool softAP(const char *ssid, const char *pass = NULL, int channel = 1,
              int hidden = 0, int max_connection = 4) {
    (void)ssid;
    (void)pass;
    (void)channel;
    (void)hidden;
    (void)max_connection;
    // OBOOK5's stock hotspot is owned by Qtopia's wifidevicecontroller; do not
    // pretend an AP came up when hostapd is not running.
    return false;
  }

  bool softAPdisconnect(bool wifioff = false) {
    (void)wifioff;
    return true;
  }
  IPAddress softAPIP() { return IPAddress(192, 168, 4, 1); }

  String macAddress() {
    std::string text;
    FILE *f = fopen("/sys/class/net/wlan0/address", "r");
    if (f) {
      char buffer[32] = {0};
      if (fgets(buffer, sizeof(buffer), f)) text = trim(buffer);
      fclose(f);
    }
    return String(text.c_str());
  }
  uint8_t *macAddress(uint8_t *mac) {
    if (!mac) return mac;
    const std::string text = macAddress().s;
    unsigned values[6] = {0};
    if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1],
               &values[2], &values[3], &values[4], &values[5]) == 6) {
      for (int i = 0; i < 6; ++i)
        mac[i] = static_cast<uint8_t>(values[i]);
    }
    return mac;
  }

  void scanDelete() {
    networks.clear();
    scanInProgress = false;
  }

  int scanNetworks(bool async = false, bool show_hidden = false,
                   bool passive = false, uint32_t max_ms_per_chan = 300,
                   uint8_t channel = 0) {
    (void)show_hidden;
    (void)passive;
    (void)max_ms_per_chan;
    (void)channel;

    networks.clear();
    scanStarted = std::chrono::steady_clock::now();
    lastScanPoll = scanStarted;
    const std::string out = run(std::string(kWpaCli) + " -i wlan0 scan");
    if (out.find("FAIL") == 0 && out.find("FAIL-BUSY") == std::string::npos) {
      scanInProgress = false;
      return WIFI_SCAN_FAILED;
    }

    if (!async) {
      (void)system("sleep 4");
      const int count = parseScanResults();
      scanInProgress = false;
      return count < 0 ? WIFI_SCAN_FAILED : count;
    }

    scanInProgress = true;
    return WIFI_SCAN_RUNNING;
  }

  int scanComplete() {
    if (!scanInProgress) return static_cast<int>(networks.size());
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - scanStarted)
            .count() < kScanSettleMs)
      return WIFI_SCAN_RUNNING;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                              lastScanPoll)
            .count() < 400)
      return WIFI_SCAN_RUNNING;
    lastScanPoll = now;

    const int count = parseScanResults();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - scanStarted)
            .count();
    if (count <= 1 && elapsed < 8000) {
      // The AP's scan cache is still filling; one line is just the current
      // BSSID.  Keep the UI in its scanning state rather than showing a
      // single-network list.
      return WIFI_SCAN_RUNNING;
    }
    if (count < 0) {
      if (elapsed > 10000) {
        scanInProgress = false;
        return WIFI_SCAN_FAILED;
      }
      return WIFI_SCAN_RUNNING;
    }
    scanInProgress = false;
    return count;
  }

  String SSID() {
    refreshStatus();
    return currentSsid;
  }
  String SSID(int i) {
    return i >= 0 && i < static_cast<int>(networks.size())
               ? networks[i].ssid
               : String();
  }
  int RSSI() {
    refreshRssi();
    return currentRssi;
  }
  int RSSI(int i) {
    return i >= 0 && i < static_cast<int>(networks.size())
               ? networks[i].rssi
               : 0;
  }
  int encryptionType(int i) {
    return i >= 0 && i < static_cast<int>(networks.size())
               ? networks[i].auth
               : WIFI_AUTH_OPEN;
  }
  void setHostname(const char *) {}
  wifi_mode_t getMode() {
    refreshStatus();
    return currentMode;
  }
  void setSleep(bool) {}
  void setAutoReconnect(bool) {}
  void setScanMethod(wifi_scan_method_t) {}
  void setSortMethod(wifi_sort_method_t) {}
  uint8_t *BSSID(uint8_t *bssid = nullptr) {
    static uint8_t value[6] = {0};
    unsigned v[6] = {0};
    refreshStatus();
    if (sscanf(currentBssidText.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1],
               &v[2], &v[3], &v[4], &v[5]) == 6) {
      for (int i = 0; i < 6; ++i) value[i] = static_cast<uint8_t>(v[i]);
    }
    if (bssid) {
      memcpy(bssid, value, sizeof(value));
      return bssid;
    }
    return value;
  }
  int32_t channel() {
    refreshStatus();
    return channelFromFreq(currentFreq);
  }
  String getHostname() { return String("obook5"); }
  int softAPgetStationNum() { return 0; }
};

extern WiFiClass WiFi;
