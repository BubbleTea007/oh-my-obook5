#include "settings.h"

#include "launcher.h"
#include "platform.h"
#include "workshop_ime.h"

#include "lvgl.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#define EINK_MODE_PATH "/sys/bus/spi/devices/spi1.0/eink_mode"
#define BACKLIGHT_PATH "/sys/class/backlight/lm3630a_leda/brightness"
#define BACKLIGHT_MAX_PATH "/sys/class/backlight/lm3630a_leda/max_brightness"
#define SETTINGS_DIR "/user/workshop"
#define BRIGHTNESS_FILE SETTINGS_DIR "/brightness"
#define WIFI_PAGE_SIZE 5

#define WPA_CLI "/usr/sbin/wpa_cli"

typedef enum {
    ACT_WIFI,
    ACT_REFRESH,
    ACT_BRIGHTNESS,
    ACT_ABOUT,
    ACT_RETURN_DESKTOP,
    ACT_WIFI_REFRESH,
    ACT_WIFI_SCAN,
    ACT_WIFI_DISCONNECT,
    ACT_BACK,
    ACT_CLOSE,
} settings_action_t;

typedef struct {
    char ssid[64];
    int rssi;
    int secure;
} scan_entry_t;

static const lv_color_t C_INK = LV_COLOR_MAKE(0x11, 0x11, 0x11);
static const lv_color_t C_WHITE = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t C_GRAY = LV_COLOR_MAKE(0x88, 0x88, 0x88);

static const lv_font_t *s_font_20;
static const lv_font_t *s_font_24;
static const lv_font_t *s_font_32;
static bool s_ui_ready;

static lv_obj_t *s_overlay;
static int s_page;
static char s_message[160];
static scan_entry_t s_scan[12];
static int s_scan_count;
static int s_wifi_page;
static char s_password_ssid[64];
static lv_obj_t *s_password_ta;
static lv_obj_t *s_password_message;

static bool s_busy;
static int s_pending_op;
static lv_timer_t *s_op_timer;
static int s_pending_connect;
static int s_confirm_index = -1;
static char s_pending_psk[80];

static lv_obj_t *s_press_obj;
static uint32_t s_press_at;
static bool s_press_inverted;
static lv_timer_t *s_press_timer;

enum { PAGE_MAIN, PAGE_WIFI, PAGE_WIFI_CONFIRM, PAGE_WIFI_PASSWORD,
       PAGE_REFRESH, PAGE_BRIGHTNESS, PAGE_ABOUT };

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* wpa_cli escapes non-ASCII SSID bytes as \xNN.  Decode them so the UI and
 * wpa_cli set_network get the original UTF-8 SSID. */
static void wpa_unescape(char *text)
{
    if (!text) return;

    char *src = text;
    char *dst = text;
    while (*src) {
        if (src[0] == '\\' && src[1] == 'x' &&
            hex_nibble(src[2]) >= 0 && hex_nibble(src[3]) >= 0) {
            const int hi = hex_nibble(src[2]);
            const int lo = hex_nibble(src[3]);
            *dst++ = (char)((hi << 4) | lo);
            src += 4;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool cmd_output(const char *command, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    FILE *pipe = popen(command, "r");
    if (!pipe) return false;
    size_t used = 0;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        size_t len = strlen(buffer);
        if (len > cap - used - 1) len = cap - used - 1;
        memcpy(out + used, buffer, len);
        used += len;
        if (used + 1 >= cap) break;
    }
    out[used] = '\0';
    pclose(pipe);
    return true;
}

static int read_int_file(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int value = -1;
    if (fscanf(file, "%d", &value) != 1) value = -1;
    fclose(file);
    return value;
}

static void write_int_file(const char *path, int value)
{
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "%d\n", value);
    fclose(file);
}

static void read_first_line(const char *path, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    FILE *file = fopen(path, "r");
    if (!file) return;
    if (fgets(out, cap, file)) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = '\0';
    }
    fclose(file);
}

static void shell_quote(const char *value, char *out, size_t cap)
{
    size_t used = 0;
    if (cap < 3) return;
    out[used++] = '\'';
    for (const char *p = value; *p && used + 5 < cap; ++p) {
        if (*p == '\'') {
            memcpy(out + used, "'\\''", 4);
            used += 4;
        } else {
            out[used++] = *p;
        }
    }
    out[used++] = '\'';
    out[used] = '\0';
}

static void wpa_ssid_arg(const char *ssid, char *out, size_t cap)
{
    char quoted[256];
    size_t used = 0;
    if (cap < 3) return;
    quoted[used++] = '"';
    for (const char *p = ssid; *p && used + 2 < sizeof(quoted); ++p) {
        if (*p == '"' || *p == '\\') quoted[used++] = '\\';
        quoted[used++] = *p;
    }
    quoted[used++] = '"';
    quoted[used] = '\0';
    shell_quote(quoted, out, cap);
}

static bool wifi_status(char *ssid, size_t ssid_cap, char *ip, size_t ip_cap)
{
    char output[2048];
    if (ssid && ssid_cap) ssid[0] = '\0';
    if (ip && ip_cap) ip[0] = '\0';
    if (!cmd_output(WPA_CLI " -i wlan0 status 2>/dev/null", output, sizeof(output)))
        return false;

    bool completed = false;
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "ssid=", 5) == 0) {
            /* Decode in the large status buffer before copying into the
             * 64-byte SSID field; an escaped UTF-8 SSID can be 4x longer. */
            wpa_unescape(line + 5);
            snprintf(ssid, ssid_cap, "%s", line + 5);
        } else if (strncmp(line, "ip_address=", 11) == 0) {
            snprintf(ip, ip_cap, "%s", line + 11);
        } else if (strncmp(line, "wpa_state=COMPLETED", 19) == 0) {
            completed = true;
        }
    }
    return completed && ip && ip[0] != '\0';
}

static int saved_network_id(const char *ssid)
{
    char output[4096];
    if (!cmd_output(WPA_CLI " -i wlan0 list_networks 2>/dev/null", output,
                    sizeof(output)))
        return -1;

    char *save = NULL;
    bool header = true;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (header) {
            header = false;
            continue;
        }
        char *id = line;
        char *name = strchr(line, '\t');
        if (!name) continue;
        *name++ = '\0';
        char *rest = strchr(name, '\t');
        if (rest) *rest = '\0';
        wpa_unescape(name);
        if (strcmp(name, ssid) == 0) return atoi(id);
    }
    return -1;
}

static void wifi_scan(void)
{
    s_scan_count = 0;
    s_wifi_page = 0;
    s_message[0] = '\0';
    (void)system(WPA_CLI " -i wlan0 scan >/dev/null 2>&1");
    usleep(2000000);

    static char output[16384];
    if (!cmd_output(WPA_CLI " -i wlan0 scan_results 2>/dev/null", output,
                    sizeof(output))) {
        snprintf(s_message, sizeof(s_message), "扫描失败");
        return;
    }

    char *save = NULL;
    bool header = true;
    for (char *line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (header) {
            header = false;
            if (strncmp(line, "bssid", 5) == 0) continue;
        }
        char *fields[5] = {line, NULL, NULL, NULL, NULL};
        char *p = line;
        for (int i = 0; i < 4; ++i) {
            p = strchr(p, '\t');
            if (!p) break;
            *p++ = '\0';
            fields[i + 1] = p;
        }
        if (!fields[4] || fields[4][0] == '\0') continue;
        char *ssid_end = strchr(fields[4], '\n');
        if (ssid_end) *ssid_end = '\0';
        wpa_unescape(fields[4]);
        if (fields[4][0] == '\0') continue;

        bool duplicate = false;
        for (int i = 0; i < s_scan_count; ++i) {
            if (strcmp(s_scan[i].ssid, fields[4]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate || s_scan_count >= (int)(sizeof(s_scan) / sizeof(s_scan[0])))
            continue;

        scan_entry_t *entry = &s_scan[s_scan_count++];
        snprintf(entry->ssid, sizeof(entry->ssid), "%s", fields[4]);
        entry->rssi = fields[2] ? atoi(fields[2]) : 0;
        entry->secure = fields[3] &&
                        (strstr(fields[3], "WPA") || strstr(fields[3], "RSN") ||
                         strstr(fields[3], "WEP"));
    }

    for (int i = 0; i < s_scan_count; ++i) {
        for (int j = i + 1; j < s_scan_count; ++j) {
            if (s_scan[j].rssi > s_scan[i].rssi) {
                scan_entry_t tmp = s_scan[i];
                s_scan[i] = s_scan[j];
                s_scan[j] = tmp;
            }
        }
    }

    if (s_scan_count == 0) snprintf(s_message, sizeof(s_message), "未发现网络");
}

static void wifi_connect(int index)
{
    if (index < 0 || index >= s_scan_count) return;
    scan_entry_t *entry = &s_scan[index];
    int id = saved_network_id(entry->ssid);
    char command[512];

    if (id < 0) {
        if (entry->secure) {
            snprintf(s_message, sizeof(s_message),
                     "加密网络：请先在 CrossMux 中连接");
            return;
        }

        char output[128];
        if (!cmd_output(WPA_CLI " -i wlan0 add_network 2>/dev/null", output,
                        sizeof(output))) {
            snprintf(s_message, sizeof(s_message), "无法添加网络");
            return;
        }
        id = atoi(output);
        if (id < 0) {
            snprintf(s_message, sizeof(s_message), "无法添加网络");
            return;
        }

        char ssid_arg[512];
        wpa_ssid_arg(entry->ssid, ssid_arg, sizeof(ssid_arg));
        snprintf(command, sizeof(command),
                 WPA_CLI " -i wlan0 set_network %d ssid %s", id, ssid_arg);
        (void)system(command);
        snprintf(command, sizeof(command),
                 WPA_CLI " -i wlan0 set_network %d key_mgmt NONE", id);
        (void)system(command);
        snprintf(command, sizeof(command),
                 WPA_CLI " -i wlan0 enable_network %d", id);
        (void)system(command);
    }

    snprintf(command, sizeof(command), WPA_CLI " -i wlan0 select_network %d", id);
    (void)system(command);
    (void)system(WPA_CLI " -i wlan0 save_config >/dev/null 2>&1");
    snprintf(s_message, sizeof(s_message), "正在连接 %s ...", entry->ssid);
    usleep(2000000);
}

static bool wifi_connect_psk(const char *ssid, const char *psk)
{
    if (!ssid || !ssid[0]) return false;
    if (!psk || strlen(psk) < 8) {
        snprintf(s_message, sizeof(s_message), "密码至少需要 8 位");
        return false;
    }

    int id = saved_network_id(ssid);
    char command[768];

    if (id < 0) {
        char output[128];
        if (!cmd_output(WPA_CLI " -i wlan0 add_network 2>/dev/null", output,
                        sizeof(output))) {
            snprintf(s_message, sizeof(s_message), "无法添加网络");
            return false;
        }
        id = atoi(output);
        if (id < 0) {
            snprintf(s_message, sizeof(s_message), "无法添加网络");
            return false;
        }
    }

    char ssid_arg[512];
    char psk_arg[512];
    wpa_ssid_arg(ssid, ssid_arg, sizeof(ssid_arg));
    wpa_ssid_arg(psk, psk_arg, sizeof(psk_arg));

    snprintf(command, sizeof(command),
             WPA_CLI " -i wlan0 set_network %d ssid %s", id, ssid_arg);
    (void)system(command);
    snprintf(command, sizeof(command),
             WPA_CLI " -i wlan0 set_network %d psk %s", id, psk_arg);
    (void)system(command);
    snprintf(command, sizeof(command),
             WPA_CLI " -i wlan0 set_network %d key_mgmt WPA-PSK", id);
    (void)system(command);
    snprintf(command, sizeof(command),
             WPA_CLI " -i wlan0 enable_network %d", id);
    (void)system(command);
    snprintf(command, sizeof(command),
             WPA_CLI " -i wlan0 select_network %d", id);
    (void)system(command);
    (void)system(WPA_CLI " -i wlan0 save_config >/dev/null 2>&1");

    snprintf(s_message, sizeof(s_message), "正在连接 %s ...", ssid);
    usleep(2000000);
    return true;
}

static int brightness_max(void)
{
    int value = read_int_file(BACKLIGHT_MAX_PATH);
    return value > 0 ? value : 255;
}

static int brightness_percent(void)
{
    const int max = brightness_max();
    int current = read_int_file(BACKLIGHT_PATH);
    if (current < 0) current = 0;
    if (current > max) current = max;
    return current * 100 / max;
}

static void brightness_set_percent(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const int max = brightness_max();
    const int value = (max * percent + 50) / 100;
    write_int_file(BACKLIGHT_PATH, value);
    write_int_file(BRIGHTNESS_FILE, value);
}

static void refresh_mode_set(int mode)
{
    if (mode < 0 || mode > 3) return;
    char command[128];
    snprintf(command, sizeof(command),
             "/opt/script/change_refresh_mode.sh %d >/dev/null 2>&1", mode);
    if (access("/opt/script/change_refresh_mode.sh", X_OK) == 0) {
        (void)system(command);
    } else {
        write_int_file(EINK_MODE_PATH, mode);
    }
}

static const char *refresh_mode_name(int mode)
{
    switch (mode) {
        case 0: return "快速刷新";
        case 1: return "普通刷新";
        case 2: return "高清刷新";
        case 3: return "单色模式";
        default: return "未知";
    }
}

static void press_feedback_timer_cb(lv_timer_t *timer);

static void style_init(void)
{
    s_font_20 = lv_tiny_ttf_create_file(platform_font_light(), 20);
    s_font_24 = lv_tiny_ttf_create_file(platform_font_light(), 24);
    s_font_32 = lv_tiny_ttf_create_file(platform_font_bold(), 32);
    if (!s_font_20) s_font_20 = &lv_font_montserrat_20;
    if (!s_font_24) s_font_24 = &lv_font_montserrat_24;
    if (!s_font_32) s_font_32 = &lv_font_montserrat_32;
    workshop_ime_set_font(s_font_20);
    if (!s_press_timer) {
        s_press_timer = lv_timer_create(press_feedback_timer_cb, 50, NULL);
    }
    s_ui_ready = true;
}

static void base_obj(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, C_WHITE, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text ? text : "");
    return label;
}

static void set_widget_inverted(lv_obj_t *obj, bool inverted)
{
    if (!obj) return;

    lv_obj_set_style_bg_color(obj, inverted ? C_INK : C_WHITE, 0);
    lv_obj_set_style_border_color(obj, inverted ? C_WHITE : C_INK, 0);

    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (child && lv_obj_check_type(child, &lv_label_class)) {
            const lv_color_t color = inverted
                                         ? C_WHITE
                                         : (i == 1 ? C_GRAY : C_INK);
            lv_obj_set_style_text_color(child, color, 0);
        }
    }
}

static void press_feedback_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_press_obj || s_press_inverted) return;

    if (platform_ticks_ms() - s_press_at >= 200U) {
        set_widget_inverted(s_press_obj, true);
        s_press_inverted = true;
    }
}

static void press_begin_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    if (!obj) return;

    if (s_press_obj && s_press_obj != obj && s_press_inverted) {
        set_widget_inverted(s_press_obj, false);
    }

    s_press_obj = obj;
    s_press_at = platform_ticks_ms();
    s_press_inverted = false;
}

static void press_end_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    if (!obj) return;

    if (s_press_obj == obj) {
        if (s_press_inverted) set_widget_inverted(obj, false);
        s_press_obj = NULL;
        s_press_inverted = false;
    }
}

static void press_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    if (s_press_obj == obj) {
        s_press_obj = NULL;
        s_press_inverted = false;
    }
}

static void attach_press_feedback(lv_obj_t *obj)
{
    if (!obj) return;

    lv_obj_add_event_cb(obj, press_begin_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, press_end_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, press_end_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(obj, press_delete_cb, LV_EVENT_DELETE, NULL);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int width,
                             int height, lv_event_cb_t cb, void *data)
{
    lv_obj_t *button = lv_obj_create(parent);
    base_obj(button);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, C_WHITE, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, C_INK, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(button, text, s_font_24, C_INK);
    attach_press_feedback(button);
    if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, data);
    return button;
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *title,
                          const char *value, lv_event_cb_t cb, void *data)
{
    lv_obj_t *row = lv_obj_create(parent);
    base_obj(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 56);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, C_INK, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    make_label(row, title, s_font_24, C_INK);
    if (value && value[0]) make_label(row, value, s_font_20, C_GRAY);
    attach_press_feedback(row);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, data);
    return row;
}

static lv_obj_t *make_header(lv_obj_t *parent, const char *title)
{
    const workshop_platform_info_t *info = platform_info();
    const int height = info->portrait ? 80 : 72;

    lv_obj_t *header = lv_obj_create(parent);
    base_obj(header);
    lv_obj_set_size(header, LV_PCT(100), height);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, C_INK, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(header, 16, 0);
    lv_obj_set_style_pad_right(header, 16, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_label(header, title, s_font_32, C_INK);
    make_button(header, s_page == PAGE_MAIN ? "关闭" : "返回", 96, 44,
                s_page == PAGE_MAIN ? NULL : NULL, NULL);
    return header;
}

static void settings_close(void);
static void settings_render(void);

enum {
    OP_NONE,
    OP_SCAN,
    OP_CONNECT,
    OP_CONNECT_PSK,
};

static void op_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_op_timer = NULL;

    const int op = s_pending_op;
    s_pending_op = OP_NONE;

    switch (op) {
        case OP_SCAN:
            wifi_scan();
            s_wifi_page = 0;
            break;
        case OP_CONNECT:
            wifi_connect(s_pending_connect);
            break;
        case OP_CONNECT_PSK:
            (void)wifi_connect_psk(s_password_ssid, s_pending_psk);
            memset(s_pending_psk, 0, sizeof(s_pending_psk));
            break;
        default:
            break;
    }

    platform_discard_touch();
    s_busy = false;
    settings_render();
}

static void schedule_op(int op)
{
    if (s_busy || s_op_timer) return;

    s_busy = true;
    s_pending_op = op;
    s_op_timer = lv_timer_create(op_timer_cb, 100, NULL);
    if (s_op_timer) {
        lv_timer_set_repeat_count(s_op_timer, 1);
    } else {
        s_pending_op = OP_NONE;
        s_busy = false;
    }
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    if (s_page == PAGE_MAIN) {
        settings_close();
    } else if (s_page == PAGE_WIFI_PASSWORD ||
               s_page == PAGE_WIFI_CONFIRM) {
        s_page = PAGE_WIFI;
        settings_render();
    } else {
        s_page = PAGE_MAIN;
        settings_render();
    }
}

static void action_cb(lv_event_t *e)
{
    const settings_action_t action =
        (settings_action_t)(intptr_t)lv_event_get_user_data(e);
    switch (action) {
        case ACT_WIFI:
            s_page = PAGE_WIFI;
            s_wifi_page = 0;
            s_confirm_index = -1;
            s_message[0] = '\0';
            settings_render();
            break;
        case ACT_REFRESH:
            s_page = PAGE_REFRESH;
            settings_render();
            break;
        case ACT_BRIGHTNESS:
            s_page = PAGE_BRIGHTNESS;
            settings_render();
            break;
        case ACT_ABOUT:
            s_page = PAGE_ABOUT;
            settings_render();
            break;
        case ACT_RETURN_DESKTOP:
            s_overlay = NULL;
            launcher_show_return_desktop();
            break;
        case ACT_WIFI_REFRESH:
            s_message[0] = '\0';
            settings_render();
            break;
        case ACT_WIFI_SCAN:
            if (s_busy) break;
            s_scan_count = 0;
            s_wifi_page = 0;
            snprintf(s_message, sizeof(s_message), "正在扫描...");
            settings_render();
            schedule_op(OP_SCAN);
            break;
        case ACT_WIFI_DISCONNECT:
            (void)system(WPA_CLI " -i wlan0 disconnect >/dev/null 2>&1");
            snprintf(s_message, sizeof(s_message), "已断开");
            settings_render();
            break;
        case ACT_BACK:
            back_cb(NULL);
            break;
        case ACT_CLOSE:
            settings_close();
            break;
    }
}

static void wifi_open_password(int index)
{
    if (index < 0 || index >= s_scan_count) return;

    snprintf(s_password_ssid, sizeof(s_password_ssid), "%s",
             s_scan[index].ssid);
    s_message[0] = '\0';
    s_password_ta = NULL;
    s_password_message = NULL;
    s_page = PAGE_WIFI_PASSWORD;
    settings_render();
}

static void password_message(const char *text)
{
    snprintf(s_message, sizeof(s_message), "%s", text ? text : "");
    if (s_password_message) lv_label_set_text(s_password_message, s_message);
}

static void password_connect_cb(lv_event_t *e)
{
    (void)e;
    if (!s_password_ta || s_busy) return;

    snprintf(s_pending_psk, sizeof(s_pending_psk), "%s",
             lv_textarea_get_text(s_password_ta));
    if (strlen(s_pending_psk) < 8) {
        password_message("密码至少需要 8 位");
        memset(s_pending_psk, 0, sizeof(s_pending_psk));
        return;
    }

    workshop_ime_hide();
    s_password_ta = NULL;
    s_password_message = NULL;
    s_page = PAGE_WIFI;
    snprintf(s_message, sizeof(s_message), "正在连接 %s ...",
             s_password_ssid);
    settings_render();
    schedule_op(OP_CONNECT_PSK);
}

static void password_cancel_cb(lv_event_t *e)
{
    (void)e;
    workshop_ime_hide();
    s_password_ta = NULL;
    s_password_message = NULL;
    s_page = PAGE_WIFI;
    settings_render();
}

static void confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    s_page = PAGE_WIFI;
    settings_render();
}

static void confirm_connect_cb(lv_event_t *e)
{
    (void)e;
    if (s_busy || s_confirm_index < 0 ||
        s_confirm_index >= s_scan_count) return;

    s_pending_connect = s_confirm_index;
    snprintf(s_message, sizeof(s_message), "正在连接 %s ...",
             s_scan[s_confirm_index].ssid);
    s_page = PAGE_WIFI;
    settings_render();
    schedule_op(OP_CONNECT);
}

static void scan_cb(lv_event_t *e)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= s_scan_count || s_busy) return;

    if (s_scan[index].secure &&
        saved_network_id(s_scan[index].ssid) < 0) {
        wifi_open_password(index);
        return;
    }

    s_confirm_index = index;
    s_page = PAGE_WIFI_CONFIRM;
    settings_render();
}

static void mode_cb(lv_event_t *e)
{
    const int mode = (int)(intptr_t)lv_event_get_user_data(e);
    refresh_mode_set(mode);
    settings_render();
}

static void brightness_cb(lv_event_t *e)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int percent = brightness_percent() + delta;
    brightness_set_percent(percent);
    settings_render();
}

static int wifi_page_count(void)
{
    return (s_scan_count + WIFI_PAGE_SIZE - 1) / WIFI_PAGE_SIZE;
}

static void wifi_prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_page > 0) {
        s_wifi_page--;
        settings_render();
    }
}

static void wifi_next_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_page + 1 < wifi_page_count()) {
        s_wifi_page++;
        settings_render();
    }
}

static void render_main(lv_obj_t *body)
{
    char ssid[64] = {0};
    char ip[32] = {0};
    const bool connected = wifi_status(ssid, sizeof(ssid), ip, sizeof(ip));
    char wifi_value[128];
    if (connected)
        snprintf(wifi_value, sizeof(wifi_value), "%s  %s", ssid, ip);
    else
        snprintf(wifi_value, sizeof(wifi_value), "未连接");

    char refresh_value[32];
    const int mode = read_int_file(EINK_MODE_PATH);
    snprintf(refresh_value, sizeof(refresh_value), "模式 %d", mode >= 0 ? mode : 0);

    char brightness_value[32];
    snprintf(brightness_value, sizeof(brightness_value), "%d%%", brightness_percent());

    make_row(body, "WiFi", wifi_value, action_cb,
             (void *)(intptr_t)ACT_WIFI);
    make_row(body, "刷新模式", refresh_value, action_cb,
             (void *)(intptr_t)ACT_REFRESH);
    make_row(body, "亮度", brightness_value, action_cb,
             (void *)(intptr_t)ACT_BRIGHTNESS);
    make_row(body, "关于", "", action_cb, (void *)(intptr_t)ACT_ABOUT);
    make_row(body, "返回桌面", "", action_cb,
             (void *)(intptr_t)ACT_RETURN_DESKTOP);
}

static void render_wifi(lv_obj_t *body)
{
    char ssid[64] = {0};
    char ip[32] = {0};
    char text[192];
    const bool connected = wifi_status(ssid, sizeof(ssid), ip, sizeof(ip));
    if (connected)
        snprintf(text, sizeof(text), "当前：%s\nIP：%s", ssid, ip);
    else
        snprintf(text, sizeof(text), "当前：未连接");

    lv_obj_t *info = make_label(body, text, s_font_20, C_INK);
    lv_obj_set_width(info, LV_PCT(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(info, 12, 0);

    make_row(body, "刷新状态", "", action_cb,
             (void *)(intptr_t)ACT_WIFI_REFRESH);
    make_row(body, "扫描网络", "", action_cb,
             (void *)(intptr_t)ACT_WIFI_SCAN);
    make_row(body, "断开连接", "", action_cb,
             (void *)(intptr_t)ACT_WIFI_DISCONNECT);

    if (s_message[0]) {
        lv_obj_t *message = make_label(body, s_message, s_font_20, C_GRAY);
        lv_obj_set_width(message, LV_PCT(100));
        lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_pad_all(message, 12, 0);
    }

    const int pages = wifi_page_count();
    if (pages > 1) {
        if (s_wifi_page < 0) s_wifi_page = 0;
        if (s_wifi_page >= pages) s_wifi_page = pages - 1;

        int first = s_wifi_page * WIFI_PAGE_SIZE;
        int last = first + WIFI_PAGE_SIZE;
        if (last > s_scan_count) last = s_scan_count;

        for (int i = first; i < last; ++i) {
            char value[48];
            snprintf(value, sizeof(value), "%d dBm%s", s_scan[i].rssi,
                     s_scan[i].secure ? "  加密" : "");
            make_row(body, s_scan[i].ssid, value, scan_cb,
                     (void *)(intptr_t)i);
        }

        lv_obj_t *pager = lv_obj_create(body);
        base_obj(pager);
        lv_obj_set_size(pager, LV_PCT(100), 52);
        lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *prev = make_button(pager, "上一页", 110, 44,
                                     wifi_prev_cb, NULL);
        if (s_wifi_page == 0) {
            lv_obj_add_state(prev, LV_STATE_DISABLED);
            lv_obj_set_style_border_color(prev, C_GRAY, 0);
            lv_obj_t *label = lv_obj_get_child(prev, 0);
            if (label) lv_obj_set_style_text_color(label, C_GRAY, 0);
        }

        char page_text[24];
        snprintf(page_text, sizeof(page_text), "%d / %d",
                 s_wifi_page + 1, pages);
        make_label(pager, page_text, s_font_20, C_INK);

        lv_obj_t *next = make_button(pager, "下一页", 110, 44,
                                     wifi_next_cb, NULL);
        if (s_wifi_page + 1 >= pages) {
            lv_obj_add_state(next, LV_STATE_DISABLED);
            lv_obj_set_style_border_color(next, C_GRAY, 0);
            lv_obj_t *label = lv_obj_get_child(next, 0);
            if (label) lv_obj_set_style_text_color(label, C_GRAY, 0);
        }
    } else {
        for (int i = 0; i < s_scan_count; ++i) {
            char value[48];
            snprintf(value, sizeof(value), "%d dBm%s", s_scan[i].rssi,
                     s_scan[i].secure ? "  加密" : "");
            make_row(body, s_scan[i].ssid, value, scan_cb,
                     (void *)(intptr_t)i);
        }
    }
}

static void render_wifi_confirm(lv_obj_t *body)
{
    if (s_confirm_index < 0 || s_confirm_index >= s_scan_count) {
        s_page = PAGE_WIFI;
        return;
    }

    const scan_entry_t *entry = &s_scan[s_confirm_index];
    const int saved = saved_network_id(entry->ssid);

    char text[256];
    snprintf(text, sizeof(text),
             "网络：%s\n安全：%s\n状态：%s",
             entry->ssid,
             entry->secure ? "加密" : "开放",
             saved >= 0 ? "已保存" : "未保存");
    lv_obj_t *info = make_label(body, text, s_font_24, C_INK);
    lv_obj_set_width(info, LV_PCT(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(info, 16, 0);

    char value[48];
    snprintf(value, sizeof(value), "%d dBm", entry->rssi);
    make_row(body, "信号", value, NULL, NULL);

    lv_obj_t *buttons = lv_obj_create(body);
    base_obj(buttons);
    lv_obj_set_size(buttons, LV_PCT(100), 60);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_button(buttons, "取消", 130, 52, confirm_cancel_cb, NULL);
    make_button(buttons, "连接", 130, 52, confirm_connect_cb, NULL);

    lv_obj_t *note = make_label(body,
                                "切换网络会短暂断开当前连接。",
                                s_font_20, C_GRAY);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(note, 12, 0);
}

static void render_password(lv_obj_t *body)
{
    char text[128];
    snprintf(text, sizeof(text), "网络：%s", s_password_ssid);
    lv_obj_t *info = make_label(body, text, s_font_24, C_INK);
    lv_obj_set_width(info, LV_PCT(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(info, 12, 0);

    s_password_ta = workshop_textarea_create(body);
    lv_obj_set_width(s_password_ta, LV_PCT(100));
    lv_obj_set_height(s_password_ta, 56);
    lv_textarea_set_one_line(s_password_ta, true);
    lv_textarea_set_password_mode(s_password_ta, true);
    /* Mask the character immediately; the default 1.5s reveal animation
     * would cause another full e-ink refresh after every keystroke. */
    lv_textarea_set_password_show_time(s_password_ta, 0);
    lv_textarea_set_max_length(s_password_ta, 63);
    lv_textarea_set_placeholder_text(s_password_ta, "输入 WiFi 密码");
    lv_obj_set_style_text_font(s_password_ta, s_font_20, 0);
    lv_obj_set_style_text_color(s_password_ta, C_INK, 0);
    lv_obj_set_style_text_color(s_password_ta, C_GRAY,
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_password_ta, C_WHITE, 0);
    lv_obj_set_style_bg_opa(s_password_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_password_ta, 2, 0);
    lv_obj_set_style_border_color(s_password_ta, C_INK, 0);
    lv_obj_set_style_radius(s_password_ta, 0, 0);
    lv_obj_set_style_pad_all(s_password_ta, 8, 0);
    lv_obj_set_style_bg_color(s_password_ta, C_INK, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(s_password_ta, LV_OPA_COVER, LV_PART_CURSOR);

    s_password_message = make_label(body, s_message, s_font_20, C_GRAY);
    lv_obj_set_width(s_password_message, LV_PCT(100));
    lv_label_set_long_mode(s_password_message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(s_password_message, 4, 0);

    lv_obj_t *buttons = lv_obj_create(body);
    base_obj(buttons);
    lv_obj_set_size(buttons, LV_PCT(100), 60);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_button(buttons, "取消", 130, 52, password_cancel_cb, NULL);
    make_button(buttons, "连接", 130, 52, password_connect_cb, NULL);

    lv_obj_t *note = make_label(body,
                                "密码使用英文键盘；点击输入框可重新弹出键盘。",
                                s_font_20, C_GRAY);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(note, 8, 0);

    lv_obj_add_state(s_password_ta, LV_STATE_FOCUSED);
    workshop_ime_show(s_password_ta);
}

static void render_refresh(lv_obj_t *body)
{
    const int current = read_int_file(EINK_MODE_PATH);
    const int shown = current >= 0 ? current : 0;
    char text[128];
    snprintf(text, sizeof(text), "当前：%d（%s）", shown,
             refresh_mode_name(shown));
    lv_obj_t *info = make_label(body, text, s_font_20, C_INK);
    lv_obj_set_style_pad_all(info, 12, 0);

    char label[64];
    for (int mode = 0; mode < 4; ++mode) {
        snprintf(label, sizeof(label), "%d：%s", mode,
                 refresh_mode_name(mode));
        make_row(body, label, current == mode ? "当前" : "", mode_cb,
                 (void *)(intptr_t)mode);
    }

    lv_obj_t *note = make_label(body,
                                "0 为 Workshop 直接刷新；其他模式由系统墨水屏驱动控制，切换后立即生效。",
                                s_font_20, C_GRAY);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(note, 12, 0);
}

static void render_brightness(lv_obj_t *body)
{
    char text[64];
    snprintf(text, sizeof(text), "当前亮度：%d%%", brightness_percent());
    lv_obj_t *info = make_label(body, text, s_font_20, C_INK);
    lv_obj_set_style_pad_all(info, 12, 0);

    make_row(body, "亮度 -10%", "", brightness_cb, (void *)(intptr_t)-10);
    make_row(body, "亮度 +10%", "", brightness_cb, (void *)(intptr_t)10);
    make_row(body, "关闭背光", "", brightness_cb, (void *)(intptr_t)-1000);

    lv_obj_t *note = make_label(body, "亮度写入 lm3630a 背光，范围 0–100%。",
                                s_font_20, C_GRAY);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(note, 12, 0);
}

static void render_about(lv_obj_t *body)
{
    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    char version[64] = {0};
    read_first_line("/home/version", version, sizeof(version));
    if (!version[0])
        read_first_line("/opt/image/opt/Qtopia/home/version", version,
                        sizeof(version));

    char mac[32] = {0};
    read_first_line("/sys/class/net/wlan0/address", mac, sizeof(mac));

    char ssid[64] = {0};
    char ip[32] = {0};
    wifi_status(ssid, sizeof(ssid), ip, sizeof(ip));

    workshop_status_t status;
    platform_get_status(&status);

    struct statvfs vfs;
    memset(&vfs, 0, sizeof(vfs));
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (statvfs("/user", &vfs) == 0) {
        total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    }

    char text[1400];
    snprintf(text, sizeof(text),
             "版本：%s\n"
             "设备：%s\n"
             "内核：%s %s\n"
             "WiFi：%s\n"
             "IP：%s\n"
             "MAC：%s\n"
             "电量：%d%%%s\n"
             "存储：%.1f / %.1f GB 可用",
             version[0] ? version : "未知", uts.machine[0] ? uts.machine : "--",
             uts.release[0] ? uts.release : "--",
             uts.version[0] ? uts.version : "--",
             ssid[0] ? ssid : "未连接", ip[0] ? ip : "--",
             mac[0] ? mac : "--", status.battery_percent,
             status.charging ? "（充电中）" : "",
             (double)free_bytes / (1024.0 * 1024.0 * 1024.0),
             (double)total / (1024.0 * 1024.0 * 1024.0));

    lv_obj_t *info = make_label(body, text, s_font_20, C_INK);
    lv_obj_set_width(info, LV_PCT(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(info, 16, 0);
}

static void settings_render(void)
{
    if (!s_overlay) return;
    if (!s_ui_ready) style_init();
    if (s_page != PAGE_WIFI_PASSWORD) workshop_ime_hide();
    s_password_ta = NULL;
    s_password_message = NULL;
    lv_obj_clean(s_overlay);

    const char *title = "设置";
    if (s_page == PAGE_WIFI) title = "WiFi";
    if (s_page == PAGE_WIFI_CONFIRM) title = "连接网络";
    if (s_page == PAGE_WIFI_PASSWORD) title = "WiFi 密码";
    if (s_page == PAGE_REFRESH) title = "刷新模式";
    if (s_page == PAGE_BRIGHTNESS) title = "亮度";
    if (s_page == PAGE_ABOUT) title = "关于";

    make_header(s_overlay, title);
    lv_obj_t *back = lv_obj_get_child(s_overlay, 0);
    if (back) {
        lv_obj_t *button = lv_obj_get_child(back, 1);
        if (button) lv_obj_add_event_cb(button, back_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *body = lv_obj_create(s_overlay);
    base_obj(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_style_bg_color(body, C_WHITE, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    switch (s_page) {
        case PAGE_MAIN: render_main(body); break;
        case PAGE_WIFI: render_wifi(body); break;
        case PAGE_WIFI_CONFIRM: render_wifi_confirm(body); break;
        case PAGE_WIFI_PASSWORD: render_password(body); break;
        case PAGE_REFRESH: render_refresh(body); break;
        case PAGE_BRIGHTNESS: render_brightness(body); break;
        case PAGE_ABOUT: render_about(body); break;
    }
}

static void settings_close(void)
{
    workshop_ime_hide();
    if (s_overlay) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
    launcher_rebuild();
}

void settings_open(void)
{
    if (!s_ui_ready) style_init();
    workshop_ime_hide();
    lv_obj_clean(lv_layer_top());
    s_overlay = lv_obj_create(lv_layer_top());
    base_obj(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    s_page = PAGE_MAIN;
    s_message[0] = '\0';
    s_scan_count = 0;
    settings_render();
}

void settings_init(void)
{
    const int saved = read_int_file(BRIGHTNESS_FILE);
    if (saved >= 0) write_int_file(BACKLIGHT_PATH, saved);
}
