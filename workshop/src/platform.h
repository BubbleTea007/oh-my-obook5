#ifndef WORKSHOP_PLATFORM_H
#define WORKSHOP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#define WORKSHOP_DEVICE_ROOT "/user/workshop"
#define WORKSHOP_DEVICE_APPS_DIR "/user/workshop/app"

typedef struct {
    int width;
    int height;
    int portrait;
} workshop_platform_info_t;

typedef struct {
    int hour;              /* 0-23 */
    int minute;            /* 0-59 */
    int battery_percent;   /* 0-100, or -1 when unknown */
    int charging;          /* 0/1 */
    int wifi_present;      /* 0/1: wlan0 exists and is administratively up */
    int wifi_connected;    /* 0/1 */
    int wifi_rssi;         /* dBm, e.g. -65; 0 when unknown */
} workshop_status_t;

int platform_init(void);
void platform_deinit(void);

const workshop_platform_info_t *platform_info(void);

const char *platform_apps_dir(void);
const char *platform_font_light(void);
const char *platform_font_bold(void);

/* Current wall clock and battery state for the header. */
void platform_get_status(workshop_status_t *status);

void platform_poll_events(void);
int platform_should_quit(void);
int platform_take_orientation_changed(void);
int platform_take_error_request(void);
void platform_request_exit(void);

void platform_set_orientation(int portrait);
void platform_sleep_ms(uint32_t ms);
uint32_t platform_ticks_ms(void);

/*
 * Launch a real app and wait for it. The platform releases and re-acquires
 * touch for the child. Returns 0 on success; otherwise fills err and returns -1.
 */
int platform_launch(const char *app_dir, const char *exec_path,
                    char *err, size_t errlen);

#endif
