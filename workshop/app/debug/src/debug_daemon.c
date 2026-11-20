// Persistent workshop debug shell.
//
// The Debug app's UI owns its listeners only while it is in the foreground.
// This small daemon keeps the same 8099 shell (and 8098 HTTP status endpoint)
// alive in the background, so launching another workshop app does not cut the
// developer connection.  It is started by /user/workshop/bin/workshop.sh.
//
// Build:
//   ./build_daemon.sh

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HTTP_PORT 8098
#define SHELL_PORT 8099
#define PID_FILE "/tmp/workshop_debug.pid"
#define DEBUG_LOG "/user/workshop/app/debug/logs/debug.log"

static volatile sig_atomic_t s_stop;

static void on_term(int sig)
{
    (void)sig;
    s_stop = 1;
}

static uint32_t ticks_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static int read_int_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int value = -1;
    if (fscanf(f, "%d", &value) != 1) value = -1;
    fclose(f);
    return value;
}

static int get_gateway_addr(struct in_addr *out)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return -1;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        unsigned int dest, gw, flags;
        if (sscanf(line, "%31s %x %x %x", iface, &dest, &gw, &flags) == 4 &&
            strcmp(iface, "wlan0") == 0 && dest == 0) {
            out->s_addr = gw;
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int wifi_connected(void)
{
    return read_int_file("/sys/class/net/wlan0/carrier") == 1;
}

static void wifi_keepalive(void)
{
    struct in_addr gw;
    if (wifi_connected() && get_gateway_addr(&gw) == 0) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons(9);
            to.sin_addr = gw;
            char byte = 0;
            (void)sendto(fd, &byte, 1, 0, (struct sockaddr *)&to, sizeof(to));
            close(fd);
        }
    } else {
        (void)system("/usr/sbin/wpa_cli -i wlan0 reconnect >/dev/null 2>&1 &");
    }
}

static int open_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static void send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, data, len, 0);
        if (n <= 0) return;
        data += n;
        len -= (size_t)n;
    }
}

static void send_response(int fd, const char *type, const char *body)
{
    char header[256];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     type, strlen(body));
    send_all(fd, header, (size_t)n);
    send_all(fd, body, strlen(body));
}

static void send_log(int fd)
{
    FILE *f = fopen(DEBUG_LOG, "r");
    if (!f) {
        send_response(fd, "text/plain; charset=utf-8", "(no log yet)\n");
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        send_response(fd, "text/plain; charset=utf-8", "(log unavailable)\n");
        return;
    }
    long size = ftell(f);
    long start = size > 16384 ? size - 16384 : 0;
    fseek(f, start, SEEK_SET);
    char *buf = malloc(16385);
    if (!buf) {
        fclose(f);
        send_response(fd, "text/plain; charset=utf-8", "(out of memory)\n");
        return;
    }
    size_t got = fread(buf, 1, 16384, f);
    buf[got] = '\0';
    fclose(f);
    send_response(fd, "text/plain; charset=utf-8", buf);
    free(buf);
}

static void handle_http(int fd)
{
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[1024];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = '\0';

    char method[8], path[512];
    if (sscanf(req, "%7s %511s", method, path) != 2 || strcmp(method, "GET") != 0) {
        send_response(fd, "text/plain; charset=utf-8", "bad request\n");
        return;
    }
    char *query = strchr(path, '?');
    if (query) *query = '\0';

    if (!strcmp(path, "/ping")) {
        send_response(fd, "text/plain; charset=utf-8", "pong\n");
        return;
    }
    if (!strcmp(path, "/log")) {
        send_log(fd);
        return;
    }
    if (!strcmp(path, "/status")) {
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"running\":1,\"mode\":\"daemon\",\"http_port\":%d,"
                 "\"shell_port\":%d,\"pid\":%d}\n",
                 HTTP_PORT, SHELL_PORT, (int)getpid());
        send_response(fd, "application/json", body);
        return;
    }

    send_response(fd, "text/html; charset=utf-8",
                  "<!doctype html><meta charset=\"utf-8\">"
                  "<title>workshop debug daemon</title>"
                  "<h1>workshop debug daemon</h1>"
                  "<pre>shell : nc &lt;ip&gt; 8099\nhttp  : /status /log /ping</pre>\n");
}

static void handle_shell(int client)
{
    pid_t pid = fork();
    if (pid == 0) {
        dup2(client, STDIN_FILENO);
        dup2(client, STDOUT_FILENO);
        dup2(client, STDERR_FILENO);
        if (client > STDERR_FILENO) close(client);
        signal(SIGCHLD, SIG_DFL);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        setsid();
        execl("/bin/sh", "sh", "-i", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) close(client);
}

static int already_running(void)
{
    FILE *f = fopen(PID_FILE, "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0;
}

static int write_pid(void)
{
    FILE *f = fopen(PID_FILE, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    return 0;
}

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);
    setsid();
    pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);

    chdir("/");
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

int main(int argc, char **argv)
{
    int wait_mode = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--wait")) wait_mode = 1;
    }

    if (already_running()) return 0;

    int http_fd = -1;
    int shell_fd = -1;
    for (int attempt = 0; attempt < (wait_mode ? 300 : 1); ++attempt) {
        if (attempt) sleep(2);
        if (already_running()) return 0;
        http_fd = open_listener(HTTP_PORT);
        shell_fd = open_listener(SHELL_PORT);
        if (http_fd >= 0 && shell_fd >= 0) break;
        if (http_fd >= 0) close(http_fd);
        if (shell_fd >= 0) close(shell_fd);
        http_fd = shell_fd = -1;
    }
    if (http_fd < 0 || shell_fd < 0) return 0;

    daemonize();
    write_pid();

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    uint32_t last_keepalive = 0;
    while (!s_stop) {
        uint32_t now = ticks_ms();
        if ((uint32_t)(now - last_keepalive) >= 20000) {
            last_keepalive = now;
            wifi_keepalive();
        }

        struct pollfd fds[2];
        int count = 0;
        fds[count++] = (struct pollfd){http_fd, POLLIN, 0};
        fds[count++] = (struct pollfd){shell_fd, POLLIN, 0};
        int ready = poll(fds, count, 1000);
        if (ready <= 0) continue;

        if (fds[0].revents & POLLIN) {
            for (int i = 0; i < 8; ++i) {
                int client = accept(http_fd, NULL, NULL);
                if (client < 0) break;
                handle_http(client);
                close(client);
            }
        }
        if (fds[1].revents & POLLIN) {
            for (int i = 0; i < 8; ++i) {
                int client = accept(shell_fd, NULL, NULL);
                if (client < 0) break;
                handle_shell(client);
            }
        }
    }

    close(http_fd);
    close(shell_fd);
    unlink(PID_FILE);
    return 0;
}
