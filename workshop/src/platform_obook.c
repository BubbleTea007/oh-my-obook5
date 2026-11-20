#include "platform.h"

#include "lvgl.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FB_PATH "/dev/fb0"
#define TOUCH_PATH "/dev/input/event1"
#define APP_ROOT "/user/workshop/app"
#define FONT_LIGHT "A:/user/workshop/fonts/LXGWWenKai-Regular.ttf"
#define FONT_BOLD "A:/user/workshop/fonts/LXGWWenKai-Medium.ttf"

#define PHYS_W 800
#define PHYS_H 480
#define EINK_WAIT_US 90000
#define MIN_FLUSH_INTERVAL_MS 200

/* OBOOK5 battery: SADC aux1, divided by 3 on the board. */
#define BATTERY_ADC_PATH "/dev/jz_adc_aux_1"
#define BATTERY_DIVIDER 3
#define BATTERY_CACHE_MS 60000U
#define CHARGER_GPIO_CMD "/usr/bin/cmd_gpio get_value PB27"

/* OBOOK5 WiFi: wlan0, link level from /proc/net/wireless. */
#define WIFI_CARRIER_PATH "/sys/class/net/wlan0/carrier"
#define WIFI_PROC_PATH "/proc/net/wireless"
#define WIFI_CACHE_MS 30000U

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
static workshop_platform_info_t s_info = {480, 800, 1};
static int s_quit;
static int s_touch_down;
static int s_last_touch_down;
static uint32_t s_ignore_touch_until;
static int s_raw_x;
static int s_raw_y;

static uint64_t timespec_diff_us(const struct timespec *start,
                                 const struct timespec *end)
{
    int64_t sec = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    int64_t nsec = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000LL;
    }
    return (uint64_t)sec * 1000000ULL + (uint64_t)nsec / 1000ULL;
}

static uint32_t s_flush_seq;
static uint32_t s_flush_count;
static uint32_t s_last_flush_end_ms;
static uint64_t s_flush_total_us;
static uint32_t s_flush_max_us;

static int s_battery_percent = -1;
static int s_battery_charging;
static int s_battery_valid;
static uint32_t s_battery_read_ms;

static int s_wifi_present;
static int s_wifi_connected;
static int s_wifi_rssi;
static int s_wifi_valid;
static uint32_t s_wifi_read_ms;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);

    const uint32_t now = platform_ticks_ms();
    if (s_last_flush_end_ms != 0) {
        const uint32_t elapsed = now - s_last_flush_end_ms;
        if (elapsed < MIN_FLUSH_INTERVAL_MS) {
            usleep((MIN_FLUSH_INTERVAL_MS - elapsed) * 1000U);
        }
    }

    const int lw = lv_display_get_horizontal_resolution(disp);
    const int lh = lv_display_get_vertical_resolution(disp);

    if (s_info.portrait && lw == 480 && lh == 800) {
        const int src_stride = lw * 2;
        uint16_t *dst = (uint16_t *)s_phys_buf;
        for (int y = 0; y < 800; y++) {
            const uint16_t *src = (const uint16_t *)(px_map + y * src_stride);
            const int X = PHYS_W - 1 - y;
            for (int x = 0; x < 480; x++) {
                const int Y = x;
                dst[Y * PHYS_W + X] = src[x];
            }
        }
    } else {
        memcpy(s_phys_buf, px_map, (size_t)lw * lh * 2);
    }

    int identical = 1;
    if (s_finfo.line_length == PHYS_W * 2) {
        identical = memcmp(s_fb_mem, s_phys_buf,
                           (size_t)PHYS_W * PHYS_H * 2) == 0;
    } else {
        for (int y = 0; y < PHYS_H; y++) {
            if (memcmp(s_fb_mem + (size_t)y * s_finfo.line_length,
                       s_phys_buf + (size_t)y * PHYS_W * 2,
                       PHYS_W * 2) != 0) {
                identical = 0;
                break;
            }
        }
    }

    if (identical) {
        s_flush_seq++;
        fprintf(stderr, "flush #%u skipped (no pixel change)\n", s_flush_seq);
        lv_display_flush_ready(disp);
        return;
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

    struct timespec ioctl_started;
    clock_gettime(CLOCK_MONOTONIC, &ioctl_started);
    ioctl(s_fb_fd, FBIOPAN_DISPLAY, &v);
    struct timespec ioctl_done;
    clock_gettime(CLOCK_MONOTONIC, &ioctl_done);

    usleep(EINK_WAIT_US);

    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &finished);

    const uint64_t ioctl_us = timespec_diff_us(&ioctl_started, &ioctl_done);
    const uint64_t total_us = timespec_diff_us(&started, &finished);

    s_flush_seq++;
    s_flush_count++;
    s_flush_total_us += total_us;
    if (total_us > s_flush_max_us) s_flush_max_us = (uint32_t)total_us;
    s_last_flush_end_ms = platform_ticks_ms();

    fprintf(stderr,
            "flush #%u area=%d,%d %dx%d ioctl=%lluus total=%lluus avg=%lluus max=%uus\n",
            s_flush_seq, area ? area->x1 : 0, area ? area->y1 : 0,
            area ? lv_area_get_width(area) : lw,
            area ? lv_area_get_height(area) : lh,
            (unsigned long long)ioctl_us, (unsigned long long)total_us,
            (unsigned long long)(s_flush_total_us / s_flush_count),
            s_flush_max_us);

    lv_display_flush_ready(disp);
}

static void input_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    struct input_event ev;

    /*
     * A foreground app's last press/release is still queued when control
     * returns here.  Without a short guard, that same release lands on the
     * launcher tile under the finger and relaunches the app immediately,
     * which looks like Workshop and the app fighting each other.
     */
    if (platform_ticks_ms() < s_ignore_touch_until) {
        while (read(s_touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        }
        s_touch_down = 0;
        s_last_touch_down = 0;
        data->point.x = s_raw_x > 0 ? s_raw_x : 0;
        data->point.y = s_raw_y > 0 ? s_raw_y : 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    while (read(s_touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) s_raw_x = ev.value;
            else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) s_raw_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            int down = ev.value != 0;
            s_touch_down = down;
            /*
             * Stop draining as soon as the press state changes. If a fast
             * tap queued both press and release while the e-ink refresh was
             * blocking us, this keeps the release for the next read instead
             * of collapsing the whole tap into a single "released" sample.
             */
            if (down != s_last_touch_down) break;
        }
    }

    /*
     * Report the panel-native 800x480 coordinates here. LVGL applies the
     * display rotation itself (ROTATION_270 maps raw to the 480x800 logical
     * frame). Do not pre-rotate, or the point gets transformed twice.
     */
    int x = s_raw_x;
    int y = s_raw_y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= PHYS_W) x = PHYS_W - 1;
    if (y >= PHYS_H) y = PHYS_H - 1;

    if (s_touch_down && !s_last_touch_down) {
        int lx = s_info.portrait ? y : x;
        int ly = s_info.portrait ? (PHYS_W - 1 - x) : y;
        fprintf(stderr, "touch raw=(%d,%d) logical=(%d,%d)\n",
                x, y, lx, ly);
    }
    s_last_touch_down = s_touch_down;

    data->point.x = x;
    data->point.y = y;
    data->state = s_touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int platform_init(void)
{
    s_fb_fd = open(FB_PATH, O_RDWR);
    if (s_fb_fd < 0) {
        perror("open fb0");
        return 1;
    }
    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &s_vinfo) < 0 ||
        ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &s_finfo) < 0) {
        perror("fb ioctl");
        return 1;
    }

    s_fb_size = s_finfo.smem_len;
    s_fb_mem = mmap(NULL, s_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, s_fb_fd, 0);
    if (s_fb_mem == MAP_FAILED) {
        perror("mmap fb");
        return 1;
    }

    s_saved_fb = malloc(s_fb_size);
    s_saved_yoffset = s_vinfo.yoffset;
    if (s_saved_fb) memcpy(s_saved_fb, s_fb_mem, s_fb_size);

    s_touch_fd = open(TOUCH_PATH, O_RDONLY | O_NONBLOCK);
    if (s_touch_fd < 0) {
        perror("open event1");
        return 1;
    }
    ioctl(s_touch_fd, EVIOCGRAB, 1);

    lv_init();
    lv_tick_set_cb(platform_ticks_ms);

    s_disp = lv_display_create(PHYS_W, PHYS_H);
    if (!s_disp) return 1;
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_270);

    s_draw_buf = malloc(480 * 800 * 2);
    s_phys_buf = malloc(PHYS_W * PHYS_H * 2);
    if (!s_draw_buf || !s_phys_buf) {
        fprintf(stderr, "workshop: out of memory for display buffers\n");
        return 1;
    }

    lv_display_set_buffers(s_disp, s_draw_buf, NULL, 480 * 800 * 2,
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, input_read_cb);

    return 0;
}

void platform_deinit(void)
{
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
        int rc = ioctl(s_fb_fd, FBIOPAN_DISPLAY, &v);
        fprintf(stderr, "restore desktop: yoffset=%d pan=%d\n",
                s_saved_yoffset, rc);
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

const workshop_platform_info_t *platform_info(void)
{
    return &s_info;
}

const char *platform_apps_dir(void)
{
    return APP_ROOT;
}

const char *platform_font_light(void)
{
    return FONT_LIGHT;
}

const char *platform_font_bold(void)
{
    return FONT_BOLD;
}

static int read_battery_mv(void)
{
    int fd = open(BATTERY_ADC_PATH, O_RDONLY);
    if (fd < 0) return -1;

    int32_t raw = -1;
    ssize_t n = read(fd, &raw, sizeof(raw));
    close(fd);

    if (n != (ssize_t)sizeof(raw) || raw <= 0) return -1;
    return (int)raw;
}

static int battery_percent_from_mv(int mv)
{
    static const int volts[] = {
        3300, 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200
    };
    static const int percents[] = {
        0, 5, 10, 20, 35, 52, 70, 82, 92, 100
    };
    const int count = (int)(sizeof(volts) / sizeof(volts[0]));

    if (mv <= volts[0]) return 0;
    if (mv >= volts[count - 1]) return 100;

    for (int i = 1; i < count; i++) {
        if (mv <= volts[i]) {
            int span = volts[i] - volts[i - 1];
            int delta = percents[i] - percents[i - 1];
            return percents[i - 1] + (mv - volts[i - 1]) * delta / span;
        }
    }
    return 100;
}

static int read_charging(void)
{
    /*
     * PB27 is the charger-detect pin used by the stock
     * /opt/script/read_usb_state.sh: 1 = charging, 0 = discharging.
     * The pin is owned by the kernel, so it cannot be read via
     * /sys/class/gpio; use the vendor helper that talks to /dev/gpio.
     */
    FILE *p = popen(CHARGER_GPIO_CMD, "r");
    if (!p) return 0;

    char value[32] = {0};
    int charging = 0;
    if (fgets(value, sizeof(value), p)) {
        charging = (value[0] == '1');
    }
    pclose(p);
    return charging;
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

static void refresh_wifi_cache(void)
{
    int present = 0;
    int connected = 0;
    int rssi = 0;

    /*
     * IFF_UP: the WiFi interface is enabled. If it is administratively
     * down (WiFi switched off), hide the icon entirely.
     */
    FILE *flags_file = fopen("/sys/class/net/wlan0/flags", "r");
    if (flags_file) {
        unsigned int flags = 0;
        if (fscanf(flags_file, "%x", &flags) == 1) {
            present = (flags & 0x1) != 0;
        }
        fclose(flags_file);
    } else if (access("/sys/class/net/wlan0", F_OK) == 0) {
        present = 1;
    }

    if (present && read_int_file(WIFI_CARRIER_PATH) == 1) {
        FILE *f = fopen(WIFI_PROC_PATH, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (strncmp(p, "wlan0:", 6) != 0) continue;

                char *colon = strchr(p, ':');
                unsigned int state = 0;
                int quality = 0;
                int level = 0;
                if (colon &&
                    sscanf(colon + 1, " %x %d. %d.",
                           &state, &quality, &level) == 3) {
                    if (level < 0 && level > -120) {
                        connected = 1;
                        rssi = level;
                    }
                }
                break;
            }
            fclose(f);
        }
    }

    s_wifi_present = present;
    s_wifi_connected = connected;
    s_wifi_rssi = rssi;
    s_wifi_read_ms = platform_ticks_ms();
    s_wifi_valid = 1;

    fprintf(stderr, "wifi: present=%d connected=%d rssi=%d\n",
            present, connected, rssi);
}

static void refresh_battery_cache(void)
{
    int adc_mv = read_battery_mv();
    int charging = read_charging();
    if (adc_mv > 0) {
        int battery_mv = adc_mv * BATTERY_DIVIDER;
        s_battery_percent = battery_percent_from_mv(battery_mv);
        fprintf(stderr, "battery: adc=%dmV battery=%dmV %d%% charging=%d\n",
                adc_mv, battery_mv, s_battery_percent, charging);
    } else {
        s_battery_percent = -1;
        fprintf(stderr, "battery: cannot read %s\n", BATTERY_ADC_PATH);
    }

    s_battery_charging = charging;
    s_battery_read_ms = platform_ticks_ms();
    s_battery_valid = 1;
}

void platform_get_status(workshop_status_t *status)
{
    if (!status) return;

    memset(status, 0, sizeof(*status));
    status->battery_percent = -1;

    time_t now = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now)) {
        status->hour = tm_now.tm_hour;
        status->minute = tm_now.tm_min;
    }

    uint32_t now_ms = platform_ticks_ms();
    if (!s_battery_valid ||
        (uint32_t)(now_ms - s_battery_read_ms) >= BATTERY_CACHE_MS) {
        refresh_battery_cache();
    }
    if (!s_wifi_valid ||
        (uint32_t)(now_ms - s_wifi_read_ms) >= WIFI_CACHE_MS) {
        refresh_wifi_cache();
    }

    status->battery_percent = s_battery_percent;
    status->charging = s_battery_charging;
    status->wifi_present = s_wifi_present;
    status->wifi_connected = s_wifi_connected;
    status->wifi_rssi = s_wifi_rssi;
}

void platform_poll_events(void)
{
}

int platform_should_quit(void)
{
    return s_quit;
}

int platform_take_orientation_changed(void)
{
    return 0;
}

int platform_take_error_request(void)
{
    return 0;
}

void platform_request_exit(void)
{
    s_quit = 1;
}

void platform_set_orientation(int portrait)
{
    s_info.portrait = portrait ? 1 : 0;
    s_info.width = portrait ? 480 : 800;
    s_info.height = portrait ? 800 : 480;

    if (s_disp) {
        lv_display_set_rotation(s_disp, portrait ? LV_DISPLAY_ROTATION_270
                                                 : LV_DISPLAY_ROTATION_0);
        lv_obj_invalidate(lv_screen_active());
    }
}

void platform_discard_touch(void)
{
    if (s_touch_fd >= 0) {
        struct input_event ev;
        while (read(s_touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        }
    }
    s_touch_down = 0;
    s_last_touch_down = 0;
    s_ignore_touch_until = platform_ticks_ms() + 300;
}

void platform_sleep_ms(uint32_t ms)
{
    usleep(ms * 1000U);
}

uint32_t platform_ticks_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static void signal_named_processes(const char *name, int sig)
{
    DIR *proc = opendir("/proc");
    if (!proc) return;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char comm[64] = {0};
        if (fgets(comm, sizeof(comm), f)) {
            char *nl = strchr(comm, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
        if (strcmp(comm, name) == 0) {
            pid_t pid = (pid_t)atoi(entry->d_name);
            fprintf(stderr, "workshop: signaling stale %s pid=%d sig=%d\n",
                    name, pid, sig);
            kill(pid, sig);
        }
    }
    closedir(proc);
}

static void kill_stale_app(const char *name)
{
    signal_named_processes(name, SIGTERM);
    usleep(300000);
    signal_named_processes(name, SIGKILL);
}

int platform_launch(const char *app_dir, const char *exec_path,
                    char *err, size_t errlen)
{
    char path[512];
    char log_dir[512];
    char log_path[512];

    if (exec_path[0] == '/') snprintf(path, sizeof(path), "%s", exec_path);
    else snprintf(path, sizeof(path), "%s/%s", app_dir, exec_path);

    if (access(path, X_OK) != 0) {
        snprintf(err, errlen, "找不到可执行文件或没有执行权限：\n%s", path);
        return -1;
    }

    const char *base = strrchr(app_dir, '/');
    base = base ? base + 1 : app_dir;
    snprintf(log_dir, sizeof(log_dir), "%s/logs", app_dir);
    mkdir(log_dir, 0755);
    snprintf(log_path, sizeof(log_path), "%s/%s.log", log_dir, base);

    kill_stale_app(base);

    if (s_touch_fd >= 0) ioctl(s_touch_fd, EVIOCGRAB, 0);

    fprintf(stderr, "workshop: launch %s\n", path);
    pid_t pid = fork();
    if (pid == 0) {
        if (chdir(app_dir) != 0) _exit(126);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execl(path, path, (char *)NULL);
        dprintf(STDERR_FILENO, "exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (pid < 0) {
        if (s_touch_fd >= 0) ioctl(s_touch_fd, EVIOCGRAB, 1);
        snprintf(err, errlen, "fork 失败：%s", strerror(errno));
        return -1;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (s_touch_fd >= 0) {
        ioctl(s_touch_fd, EVIOCGRAB, 1);
        struct input_event ev;
        while (read(s_touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        }
    }
    s_touch_down = 0;
    s_last_touch_down = 0;
    s_ignore_touch_until = platform_ticks_ms() + 600;

    fprintf(stderr, "workshop: %s exited exited=%d code=%d signal=%d\n",
            base, WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            WIFSIGNALED(status) ? WTERMSIG(status) : 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 127) {
            snprintf(err, errlen,
                     "找不到可执行文件或启动失败\n%s\n日志：%s", path, log_path);
        } else {
            snprintf(err, errlen,
                     "App 退出码 %d\n日志：%s", code, log_path);
        }
    } else if (WIFSIGNALED(status)) {
        snprintf(err, errlen,
                 "App 被信号 %d 终止\n日志：%s", WTERMSIG(status), log_path);
    } else {
        snprintf(err, errlen, "App 异常退出\n日志：%s", log_path);
    }
    return -1;
}
