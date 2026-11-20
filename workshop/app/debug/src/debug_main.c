#include "lvgl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FB_PATH "/dev/fb0"
#define TOUCH_PATH "/dev/input/event1"
#define PHYS_W 800
#define PHYS_H 480
#define EINK_WAIT_US 90000
#define HTTP_PORT 8098
#define SHELL_PORT 8099
#define DEBUG_LOG "/user/workshop/app/debug/logs/debug.log"
#define FONT_LIGHT "A:/user/workshop/fonts/LXGWWenKai-Regular.ttf"
#define FONT_BOLD "A:/user/workshop/fonts/LXGWWenKai-Medium.ttf"
#define DEBUGD_PID "/tmp/workshop_debug.pid"

static int s_fb_fd = -1;
static int s_touch_fd = -1;
static unsigned char *s_fb_mem;
static size_t s_fb_size;
static unsigned char *s_saved_fb;
static unsigned char *s_draw_buf;
static unsigned char *s_phys_buf;
static struct fb_var_screeninfo s_vinfo;
static struct fb_fix_screeninfo s_finfo;
static int s_saved_yoffset;

static lv_display_t *s_disp;
static int s_quit;
static int s_touch_down;
static int s_last_touch_down;
static int s_raw_x;
static int s_raw_y;

static int s_http_fd = -1;
static int s_http_port;
static int s_shell_fd = -1;
static int s_shell_port;
static uint32_t s_last_keepalive;

static lv_obj_t *s_info_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_toggle_btn;
static lv_timer_t *s_refresh_timer;
static char s_last_info[2048];

static const lv_color_t C_INK = LV_COLOR_MAKE(0x11, 0x11, 0x11);
static const lv_color_t C_WHITE = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t C_GRAY = LV_COLOR_MAKE(0x99, 0x99, 0x99);

static const lv_font_t *s_font_20;
static const lv_font_t *s_font_24;
static const lv_font_t *s_font_32;

static lv_style_t s_style_btn;
static lv_style_t s_style_btn_pressed;

typedef struct {
    char ip[32];
    char gateway[32];
    char mac[32];
    int http_port;
    int shell_port;
    int running;
    int wifi_connected;
    int wifi_rssi;
} debug_info_t;

static int daemon_pid(void)
{
    FILE *f = fopen(DEBUGD_PID, "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0 ? pid : 0;
}

static int daemon_running(void) { return daemon_pid() > 0; }

static void daemon_stop(void)
{
    int pid = daemon_pid();
    if (pid > 0) kill(pid, SIGTERM);
    unlink(DEBUGD_PID);
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

static void get_wlan_ip(char *out, size_t len)
{
    snprintf(out, len, "--");
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return;

    for (struct ifaddrs *it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(it->ifa_name, "wlan0") != 0) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)it->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, out, len);
        break;
    }
    freeifaddrs(list);
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

static void get_gateway(char *out, size_t len)
{
    snprintf(out, len, "--");
    struct in_addr gw;
    if (get_gateway_addr(&gw) == 0) {
        inet_ntop(AF_INET, &gw, out, len);
    }
}

static void get_mac(char *out, size_t len)
{
    snprintf(out, len, "--");
    FILE *f = fopen("/sys/class/net/wlan0/address", "r");
    if (!f) return;
    if (fgets(out, len, f)) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = '\0';
    }
    fclose(f);
}

static int wifi_rssi(void)
{
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) return 0;

    char line[256];
    int level = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "wlan0:", 6) != 0) continue;
        char *colon = strchr(p, ':');
        unsigned int state;
        int quality;
        if (colon && sscanf(colon + 1, " %x %d. %d.", &state, &quality, &level) == 3) {
            break;
        }
    }
    fclose(f);
    return level;
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

static void fill_info(debug_info_t *info)
{
    memset(info, 0, sizeof(*info));
    get_wlan_ip(info->ip, sizeof(info->ip));
    get_gateway(info->gateway, sizeof(info->gateway));
    get_mac(info->mac, sizeof(info->mac));
    const int background_pid = daemon_pid();
    info->http_port = s_http_fd >= 0 ? s_http_port : (background_pid ? HTTP_PORT : 0);
    info->shell_port = s_shell_fd >= 0 ? s_shell_port : (background_pid ? SHELL_PORT : 0);
    info->running = s_http_fd >= 0 || s_shell_fd >= 0 || background_pid > 0;
    info->wifi_connected = wifi_connected();
    info->wifi_rssi = wifi_rssi();
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

static void handle_client(int fd)
{
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

    debug_info_t info;
    fill_info(&info);

    if (!strcmp(path, "/status")) {
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"running\":%d,\"ip\":\"%s\",\"http_port\":%d,"
                 "\"shell_port\":%d,\"gateway\":\"%s\",\"mac\":\"%s\","
                 "\"wifi\":%d,\"rssi\":%d}\n",
                 info.running, info.ip, info.http_port, info.shell_port,
                 info.gateway, info.mac, info.wifi_connected, info.wifi_rssi);
        send_response(fd, "application/json", body);
        return;
    }

    char body[2048];
    snprintf(body, sizeof(body),
             "<!doctype html><html><head><meta charset=\"utf-8\">"
             "<title>workshop debug</title>"
             "<style>body{font-family:monospace;margin:24px;line-height:1.7}"
             "a{color:#111}</style></head><body>"
             "<h1>workshop debug</h1><pre>"
             "server : %s\n"
             "ip     : %s\n"
             "http   : http://%s:%d/\n"
             "shell  : nc %s %d\n"
             "gateway: %s\n"
             "mac    : %s\n"
             "wifi   : %s (%d dBm)\n"
             "</pre><p><a href=\"/status\">/status</a> &nbsp; "
             "<a href=\"/log\">/log</a> &nbsp; <a href=\"/ping\">/ping</a></p>"
             "</body></html>\n",
             info.running ? "running" : "stopped",
             info.ip, info.ip, info.http_port, info.ip, info.shell_port,
             info.gateway, info.mac,
             info.wifi_connected ? "connected" : "disconnected", info.wifi_rssi);
    send_response(fd, "text/html; charset=utf-8", body);
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

static void handle_shell_client(int client)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (s_fb_fd >= 0) close(s_fb_fd);
        if (s_touch_fd >= 0) close(s_touch_fd);
        if (s_http_fd >= 0) close(s_http_fd);
        if (s_shell_fd >= 0) close(s_shell_fd);

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

static void server_poll(void)
{
    if (s_http_fd < 0 && s_shell_fd < 0) return;

    uint32_t now = ticks_ms();
    if ((uint32_t)(now - s_last_keepalive) >= 20000) {
        s_last_keepalive = now;
        wifi_keepalive();
    }

    for (int i = 0; i < 4 && s_http_fd >= 0; i++) {
        int client = accept(s_http_fd, NULL, NULL);
        if (client < 0) break;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle_client(client);
        close(client);
    }

    for (int i = 0; i < 4 && s_shell_fd >= 0; i++) {
        int client = accept(s_shell_fd, NULL, NULL);
        if (client < 0) break;
        handle_shell_client(client);
    }
}

static int server_start(void)
{
    if (daemon_running()) return 0;
    if (s_http_fd >= 0 && s_shell_fd >= 0) return 0;

    if (s_http_fd < 0) {
        s_http_fd = open_listener(HTTP_PORT);
        if (s_http_fd < 0) return -1;
        s_http_port = HTTP_PORT;
    }
    if (s_shell_fd < 0) {
        s_shell_fd = open_listener(SHELL_PORT);
        if (s_shell_fd < 0) {
            close(s_http_fd);
            s_http_fd = -1;
            s_http_port = 0;
            return -1;
        }
        s_shell_port = SHELL_PORT;
    }

    s_last_keepalive = 0;
    fprintf(stderr, "debug servers: http=%d shell=%d\n", s_http_port, s_shell_port);
    return 0;
}

static void server_stop(void)
{
    if (s_http_fd >= 0) {
        close(s_http_fd);
        s_http_fd = -1;
        s_http_port = 0;
    }
    if (s_shell_fd >= 0) {
        close(s_shell_fd);
        s_shell_fd = -1;
        s_shell_port = 0;
    }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    const int lw = lv_display_get_horizontal_resolution(disp);
    const int lh = lv_display_get_vertical_resolution(disp);

    if (lw == 480 && lh == 800) {
        const int src_stride = lw * 2;
        uint16_t *dst = (uint16_t *)s_phys_buf;
        for (int y = 0; y < 800; y++) {
            const uint16_t *src = (const uint16_t *)(px_map + y * src_stride);
            const int X = PHYS_W - 1 - y;
            for (int x = 0; x < 480; x++) {
                dst[x * PHYS_W + X] = src[x];
            }
        }
    } else {
        memcpy(s_phys_buf, px_map, (size_t)lw * lh * 2);
    }

    if (s_finfo.line_length == PHYS_W * 2) {
        memcpy(s_fb_mem, s_phys_buf, (size_t)PHYS_W * PHYS_H * 2);
    } else {
        for (int y = 0; y < PHYS_H; y++) {
            memcpy(s_fb_mem + (size_t)y * s_finfo.line_length,
                   s_phys_buf + (size_t)y * PHYS_W * 2,
                   PHYS_W * 2);
        }
    }

    struct fb_var_screeninfo v = s_vinfo;
    v.xoffset = 0;
    v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW;
    ioctl(s_fb_fd, FBIOPAN_DISPLAY, &v);
    usleep(EINK_WAIT_US);
    lv_display_flush_ready(disp);
}

static void input_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    struct input_event ev;
    while (read(s_touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) s_raw_x = ev.value;
            else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) s_raw_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            int down = ev.value != 0;
            s_touch_down = down;
            if (down != s_last_touch_down) break;
        }
    }

    int x = s_raw_x;
    int y = s_raw_y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= PHYS_W) x = PHYS_W - 1;
    if (y >= PHYS_H) y = PHYS_H - 1;

    s_last_touch_down = s_touch_down;
    data->point.x = x;
    data->point.y = y;
    data->state = s_touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static int platform_init(void)
{
    s_fb_fd = open(FB_PATH, O_RDWR);
    if (s_fb_fd < 0) {
        perror("open fb0");
        return -1;
    }
    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &s_vinfo) < 0 ||
        ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &s_finfo) < 0) {
        perror("fb ioctl");
        return -1;
    }

    s_fb_size = s_finfo.smem_len;
    s_fb_mem = mmap(NULL, s_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, s_fb_fd, 0);
    if (s_fb_mem == MAP_FAILED) {
        perror("mmap fb");
        return -1;
    }

    s_saved_fb = malloc(s_fb_size);
    s_saved_yoffset = s_vinfo.yoffset;
    if (s_saved_fb) memcpy(s_saved_fb, s_fb_mem, s_fb_size);

    s_touch_fd = open(TOUCH_PATH, O_RDONLY | O_NONBLOCK);
    if (s_touch_fd < 0) {
        perror("open event1");
        return -1;
    }
    ioctl(s_touch_fd, EVIOCGRAB, 1);

    lv_init();
    lv_tick_set_cb(ticks_ms);

    s_disp = lv_display_create(PHYS_W, PHYS_H);
    if (!s_disp) return -1;
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_270);

    s_draw_buf = malloc(480 * 800 * 2);
    s_phys_buf = malloc(PHYS_W * PHYS_H * 2);
    if (!s_draw_buf || !s_phys_buf) return -1;

    lv_display_set_buffers(s_disp, s_draw_buf, NULL, 480 * 800 * 2,
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, input_read_cb);
    return 0;
}

static void platform_deinit(void)
{
    server_stop();

    if (s_touch_fd >= 0) {
        ioctl(s_touch_fd, EVIOCGRAB, 0);
        close(s_touch_fd);
        s_touch_fd = -1;
    }

    if (s_fb_mem && s_saved_fb) {
        memcpy(s_fb_mem, s_saved_fb, s_fb_size);
        struct fb_var_screeninfo v = s_vinfo;
        v.yoffset = s_saved_yoffset;
        v.activate = FB_ACTIVATE_NOW;
        ioctl(s_fb_fd, FBIOPAN_DISPLAY, &v);
        usleep(700000);
    }

    free(s_saved_fb);
    free(s_draw_buf);
    free(s_phys_buf);
    s_saved_fb = NULL;
    s_draw_buf = NULL;
    s_phys_buf = NULL;

    if (s_fb_mem && s_fb_mem != MAP_FAILED) munmap(s_fb_mem, s_fb_size);
    s_fb_mem = NULL;
    if (s_fb_fd >= 0) close(s_fb_fd);
    s_fb_fd = -1;

    lv_deinit();
}

static void make_screen_base(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, C_WHITE, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_text_color(obj, C_INK, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int width, int height, lv_event_cb_t cb,
                             void *user_data)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(btn, &s_style_btn, 0);
    lv_obj_add_style(btn, &s_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, width, height);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_remove_style_all(label);
    lv_obj_set_style_text_font(label, s_font_24, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

static void refresh_info(void)
{
    if (!s_info_label) return;

    debug_info_t info;
    fill_info(&info);

    char text[2048];
    if (info.running) {
        snprintf(text, sizeof(text),
                 "调试服务：运行中\n\n"
                 "IP：%s\n"
                 "Shell：%d\n"
                 "  nc %s %d\n"
                 "HTTP：%d\n"
                 "  http://%s:%d/\n"
                 "网关：%s\n"
                 "MAC：%s\n"
                 "WiFi：%s\n"
                 "保活：开启（20 秒）\n\n"
                 "HTTP 接口：/status  /log  /ping",
                 info.ip, info.shell_port, info.ip, info.shell_port,
                 info.http_port, info.ip, info.http_port,
                 info.gateway, info.mac,
                 info.wifi_connected ? "已连接" : "未连接");
    } else {
        snprintf(text, sizeof(text),
                 "调试服务：未启动\n\n"
                 "IP：%s\n"
                 "Shell 端口：%d\n"
                 "HTTP 端口：%d\n"
                 "网关：%s\n"
                 "MAC：%s\n"
                 "WiFi：%s\n\n"
                 "点“启动服务”开始监听。",
                 info.ip, SHELL_PORT, HTTP_PORT,
                 info.gateway, info.mac,
                 info.wifi_connected ? "已连接" : "未连接");
    }

    if (strcmp(text, s_last_info) != 0) {
        snprintf(s_last_info, sizeof(s_last_info), "%s", text);
        lv_label_set_text(s_info_label, text);
    }

    if (s_status_label) {
        lv_label_set_text(s_status_label,
                          info.running ? "Shell 8099 / HTTP 8098"
                                       : "服务已停止");
    }
    if (s_toggle_btn) {
        lv_obj_t *label = lv_obj_get_child(s_toggle_btn, 0);
        if (label) lv_label_set_text(label,
                                     info.running ? "停止服务" : "启动服务");
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_info();
}

static void toggle_cb(lv_event_t *e)
{
    (void)e;
    if (s_http_fd >= 0 || s_shell_fd >= 0) {
        server_stop();
    } else if (daemon_running()) {
        daemon_stop();
    } else {
        server_start();
    }
    refresh_info();
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    refresh_info();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    s_quit = 1;
}

static void ui_create(void)
{
    s_font_20 = lv_tiny_ttf_create_file(FONT_LIGHT, 20);
    s_font_24 = lv_tiny_ttf_create_file(FONT_BOLD, 24);
    s_font_32 = lv_tiny_ttf_create_file(FONT_BOLD, 32);
    if (!s_font_20) s_font_20 = &lv_font_montserrat_20;
    if (!s_font_24) s_font_24 = &lv_font_montserrat_24;
    if (!s_font_32) s_font_32 = &lv_font_montserrat_32;

    lv_style_init(&s_style_btn);
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_bg_color(&s_style_btn, C_WHITE);
    lv_style_set_border_width(&s_style_btn, 2);
    lv_style_set_border_color(&s_style_btn, C_INK);
    lv_style_set_radius(&s_style_btn, 0);
    lv_style_set_shadow_width(&s_style_btn, 0);
    lv_style_set_pad_all(&s_style_btn, 0);
    lv_style_set_text_color(&s_style_btn, C_INK);

    lv_style_init(&s_style_btn_pressed);
    lv_style_set_bg_opa(&s_style_btn_pressed, LV_OPA_COVER);
    lv_style_set_bg_color(&s_style_btn_pressed, C_INK);
    lv_style_set_text_color(&s_style_btn_pressed, C_WHITE);

    lv_obj_t *screen = lv_screen_active();
    make_screen_base(screen);
    lv_obj_set_size(screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = lv_obj_create(screen);
    make_screen_base(header);
    lv_obj_set_size(header, LV_PCT(100), 80);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, C_INK, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(header, 20, 0);
    lv_obj_set_style_pad_right(header, 20, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_label(header, "调试模式", s_font_32, C_INK);
    s_status_label = make_label(header, "HTTP 服务已停止", s_font_20, C_GRAY);

    lv_obj_t *content = lv_obj_create(screen);
    make_screen_base(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, 24, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 16, 0);

    s_info_label = make_label(content, "", s_font_20, C_INK);
    lv_obj_set_width(s_info_label, LV_PCT(100));
    lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *actions = lv_obj_create(content);
    make_screen_base(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 12, 0);

    s_toggle_btn = make_button(actions, "停止服务", 120, 52, toggle_cb, NULL);
    make_button(actions, "刷新", 80, 52, refresh_cb, NULL);
    make_button(actions, "返回 workshop", 170, 52, back_cb, NULL);
}

int main(void)
{
    if (platform_init() != 0) return 1;

    ui_create();
    if (server_start() != 0) {
        fprintf(stderr, "cannot listen on ports %d/%d: %s\n",
                HTTP_PORT, SHELL_PORT, strerror(errno));
    }
    refresh_info();

    s_refresh_timer = lv_timer_create(refresh_timer_cb, 3000, NULL);
    signal(SIGCHLD, SIG_IGN);

    while (!s_quit) {
        server_poll();
        uint32_t next = lv_timer_handler();
        if (next < 10) next = 10;
        if (next > 30) next = 30;
        usleep(next * 1000U);
    }

    if (s_refresh_timer) lv_timer_delete(s_refresh_timer);
    server_stop();
    platform_deinit();
    return 0;
}
