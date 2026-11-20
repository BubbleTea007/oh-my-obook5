#include "platform.h"

#include "lvgl.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef WORKSHOP_SIM_APPS
#define WORKSHOP_SIM_APPS "./app"
#endif
#ifndef WORKSHOP_SIM_FONT_LIGHT
#define WORKSHOP_SIM_FONT_LIGHT "./fonts/LxgwWenKai/fonts/TTF/LXGWWenKai-Regular.ttf"
#endif
#ifndef WORKSHOP_SIM_FONT_BOLD
#define WORKSHOP_SIM_FONT_BOLD "./fonts/LxgwWenKai/fonts/TTF/LXGWWenKai-Medium.ttf"
#endif

#define MAX_WIDTH 800
#define MAX_HEIGHT 800
#define MAX_BUF_BYTES (MAX_WIDTH * MAX_HEIGHT * 2)
#define EINK_REFRESH_MS 150

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static lv_display_t *s_display;
static unsigned char *s_draw_buf;

static workshop_platform_info_t s_info = {480, 800, 1};

static int s_quit;
static int s_orientation_changed;
static int s_pending_portrait = 1;
static int s_error_requested;
static int s_eink_busy;

static int s_mouse_x;
static int s_mouse_y;
static int s_mouse_pressed;
static int s_screenshot_saved;

static void recreate_texture(void)
{
    if (s_texture) SDL_DestroyTexture(s_texture);
    s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  s_info.width, s_info.height);
    if (!s_texture) {
        fprintf(stderr, "workshop: SDL_CreateTexture failed: %s\n", SDL_GetError());
        return;
    }
    SDL_RenderSetLogicalSize(s_renderer, s_info.width, s_info.height);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int w = lv_area_get_width(area);
    const int h = lv_area_get_height(area);
    SDL_Rect dst = {area->x1, area->y1, w, h};

    SDL_UpdateTexture(s_texture, &dst, px_map, w * 2);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);

    const char *screenshot_path = getenv("WORKSHOP_SCREENSHOT");
    if (!s_screenshot_saved && screenshot_path && *screenshot_path) {
        SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(
            px_map, w, h, 16, w * 2, 0xF800, 0x07E0, 0x001F, 0);
        if (surface) {
            SDL_SaveBMP(surface, screenshot_path);
            SDL_FreeSurface(surface);
        }
        s_screenshot_saved = 1;
    }

    /* The e-ink refresh is blocking. Do not accept input during it. */
    s_eink_busy = 1;
    SDL_Delay(EINK_REFRESH_MS);
    s_eink_busy = 0;

    lv_display_flush_ready(disp);
}

static void input_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_mouse_x;
    data->point.y = s_mouse_y;
    data->state = (s_mouse_pressed && !s_eink_busy)
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
}

static void update_mouse_position(int x, int y)
{
    int ww = 1;
    int wh = 1;
    SDL_GetWindowSize(s_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;

    s_mouse_x = x * s_info.width / ww;
    s_mouse_y = y * s_info.height / wh;

    if (s_mouse_x < 0) s_mouse_x = 0;
    if (s_mouse_y < 0) s_mouse_y = 0;
    if (s_mouse_x >= s_info.width) s_mouse_x = s_info.width - 1;
    if (s_mouse_y >= s_info.height) s_mouse_y = s_info.height - 1;
}

int platform_init(void)
{
    if (getenv("WORKSHOP_LANDSCAPE")) {
        s_info.portrait = 0;
        s_info.width = 800;
        s_info.height = 480;
        s_pending_portrait = 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "workshop: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");

    s_window = SDL_CreateWindow("workshop",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                s_info.width, s_info.height,
                                SDL_WINDOW_SHOWN);
    if (!s_window) {
        fprintf(stderr, "workshop: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowResizable(s_window, SDL_FALSE);
    SDL_ShowCursor(SDL_DISABLE);

    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
    if (!s_renderer) {
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_renderer) {
        fprintf(stderr, "workshop: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    lv_init();
    lv_tick_set_cb(platform_ticks_ms);

    s_display = lv_display_create(s_info.width, s_info.height);
    if (!s_display) {
        fprintf(stderr, "workshop: lv_display_create failed\n");
        return 1;
    }

    s_draw_buf = malloc(MAX_BUF_BYTES);
    if (!s_draw_buf) {
        fprintf(stderr, "workshop: framebuffer allocation failed\n");
        return 1;
    }

    lv_display_set_buffers(s_display, s_draw_buf, NULL, MAX_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_display, flush_cb);

    recreate_texture();
    if (!s_texture) return 1;

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, input_read_cb);

    return 0;
}

void platform_deinit(void)
{
    free(s_draw_buf);
    s_draw_buf = NULL;

    if (s_texture) SDL_DestroyTexture(s_texture);
    if (s_renderer) SDL_DestroyRenderer(s_renderer);
    if (s_window) SDL_DestroyWindow(s_window);
    s_texture = NULL;
    s_renderer = NULL;
    s_window = NULL;

    lv_deinit();
    SDL_Quit();
}

const workshop_platform_info_t *platform_info(void)
{
    return &s_info;
}

const char *platform_apps_dir(void)
{
    return WORKSHOP_SIM_APPS;
}

const char *platform_font_light(void)
{
    return WORKSHOP_SIM_FONT_LIGHT;
}

const char *platform_font_bold(void)
{
    return WORKSHOP_SIM_FONT_BOLD;
}

void platform_get_status(workshop_status_t *status)
{
    if (!status) return;

    time_t now = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now)) {
        status->hour = tm_now.tm_hour;
        status->minute = tm_now.tm_min;
    } else {
        status->hour = 0;
        status->minute = 0;
    }

    /* Simulator shows plausible fixed values so the header can be tested. */
    const char *battery = getenv("WORKSHOP_SIM_BATTERY");
    status->battery_percent = battery ? atoi(battery) : 86;
    status->charging = getenv("WORKSHOP_SIM_CHARGING") ? 1 : 0;
    status->wifi_present = getenv("WORKSHOP_SIM_WIFI_OFF") ? 0 : 1;
    status->wifi_connected =
        getenv("WORKSHOP_SIM_WIFI_DISCONNECTED") ? 0 : status->wifi_present;
    const char *rssi = getenv("WORKSHOP_SIM_WIFI_RSSI");
    status->wifi_rssi = rssi ? atoi(rssi) : -55;
}

void platform_poll_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            s_quit = 1;
            break;

        case SDL_KEYDOWN:
            if (ev.key.repeat) break;
            if (ev.key.keysym.sym == SDLK_ESCAPE) {
                s_quit = 1;
            } else if (ev.key.keysym.sym == SDLK_r) {
                s_pending_portrait = !s_info.portrait;
                s_orientation_changed = 1;
            } else if (ev.key.keysym.sym == SDLK_F1) {
                s_error_requested = 1;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                update_mouse_position(ev.button.x, ev.button.y);
                s_mouse_pressed = 1;
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                update_mouse_position(ev.button.x, ev.button.y);
                s_mouse_pressed = 0;
            }
            break;

        case SDL_MOUSEMOTION:
            update_mouse_position(ev.motion.x, ev.motion.y);
            break;

        default:
            break;
        }
    }
}

int platform_should_quit(void)
{
    return s_quit;
}

int platform_take_orientation_changed(void)
{
    if (!s_orientation_changed) return 0;
    s_orientation_changed = 0;
    platform_set_orientation(s_pending_portrait);
    return 1;
}

int platform_take_error_request(void)
{
    if (!s_error_requested) return 0;
    s_error_requested = 0;
    return 1;
}

void platform_request_exit(void)
{
    s_quit = 1;
}

int platform_launch(const char *app_dir, const char *exec_path,
                    char *err, size_t errlen)
{
    (void)app_dir;
    (void)exec_path;
    if (err && errlen) snprintf(err, errlen, "simulator does not exec apps");
    return -1;
}

void platform_set_orientation(int portrait)
{
    s_info.portrait = portrait ? 1 : 0;
    s_info.width = portrait ? 480 : 800;
    s_info.height = portrait ? 800 : 480;

    SDL_SetWindowSize(s_window, s_info.width, s_info.height);
    SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    recreate_texture();

    lv_display_set_resolution(s_display, s_info.width, s_info.height);
    lv_obj_invalidate(lv_screen_active());
}

void platform_discard_touch(void)
{
    s_mouse_pressed = 0;
}

void platform_sleep_ms(uint32_t ms)
{
    SDL_Delay(ms);
}

uint32_t platform_ticks_ms(void)
{
    return SDL_GetTicks();
}
