#include "launcher.h"

#include "icons.h"
#include "manifest.h"
#include "settings.h"
#include "platform.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define APPS_PER_PAGE 6
#define MOCK_LAUNCH_MS 600
#define COLOR_INK 0x111111

static workshop_app_t s_apps[WORKSHOP_MAX_APPS];
static int s_app_count;
static int s_page;

static const lv_font_t *s_font_20;
static const lv_font_t *s_font_24;
static const lv_font_t *s_font_bold_24;
static const lv_font_t *s_font_32;
static const lv_font_t *s_font_status;

static lv_style_t s_style_tile;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_pressed;
static lv_style_t s_style_btn_disabled;
static lv_style_t s_style_btn_dark;
static lv_style_t s_style_btn_dark_pressed;

static lv_coord_t s_col_dsc[4];
static lv_coord_t s_row_dsc[4];

static int s_launching;
static lv_timer_t *s_launch_timer;
static workshop_app_t *s_pending_app;
static lv_obj_t *s_first_tile;

static lv_obj_t *s_status_wifi_box;
static lv_obj_t *s_status_wifi_arc3;
static lv_obj_t *s_status_wifi_arc2;
static lv_obj_t *s_status_wifi_arc1;
static lv_obj_t *s_status_wifi_dot;
static lv_obj_t *s_status_battery;
static lv_obj_t *s_status_percent_label;
static lv_obj_t *s_status_time_label;
static lv_timer_t *s_status_timer;
static lv_obj_t *s_pressed_tile;
static uint32_t s_tile_press_at;
static int s_tile_press_shown;
static lv_timer_t *s_tile_press_timer;
static const char *s_status_battery_key;
static int s_status_wifi_bars = -2;
static int s_status_hour = -1;
static int s_status_minute = -1;
static int s_status_percent = -2;
static int s_status_charging = -1;

static const lv_color_t C_INK = LV_COLOR_MAKE(0x11, 0x11, 0x11);
static const lv_color_t C_WHITE = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t C_GRAY = LV_COLOR_MAKE(0x99, 0x99, 0x99);

static void build_ui(void);
static void show_desktop_exit(void);
static void update_status(int force);

static void build_ui_async(void *unused)
{
    (void)unused;
    build_ui();
}

static void show_desktop_exit_async(void *unused)
{
    (void)unused;
    show_desktop_exit();
}

static void style_init(void)
{
    lv_style_init(&s_style_tile);
    lv_style_set_bg_opa(&s_style_tile, LV_OPA_COVER);
    lv_style_set_bg_color(&s_style_tile, C_WHITE);
    lv_style_set_border_width(&s_style_tile, 1);
    lv_style_set_border_color(&s_style_tile, C_INK);
    lv_style_set_border_opa(&s_style_tile, LV_OPA_COVER);
    lv_style_set_radius(&s_style_tile, 0);
    lv_style_set_shadow_width(&s_style_tile, 0);
    lv_style_set_pad_all(&s_style_tile, 12);
    lv_style_set_text_color(&s_style_tile, C_INK);

    lv_style_init(&s_style_btn);
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_bg_color(&s_style_btn, C_WHITE);
    lv_style_set_border_width(&s_style_btn, 2);
    lv_style_set_border_color(&s_style_btn, C_INK);
    lv_style_set_border_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn, 0);
    lv_style_set_shadow_width(&s_style_btn, 0);
    lv_style_set_pad_all(&s_style_btn, 0);
    lv_style_set_text_color(&s_style_btn, C_INK);

    lv_style_init(&s_style_btn_pressed);
    lv_style_set_bg_opa(&s_style_btn_pressed, LV_OPA_COVER);
    lv_style_set_bg_color(&s_style_btn_pressed, C_INK);
    lv_style_set_text_color(&s_style_btn_pressed, C_WHITE);

    lv_style_init(&s_style_btn_disabled);
    lv_style_set_border_color(&s_style_btn_disabled, lv_color_hex(0xAAAAAA));
    lv_style_set_text_color(&s_style_btn_disabled, lv_color_hex(0xAAAAAA));

    lv_style_init(&s_style_btn_dark);
    lv_style_set_bg_opa(&s_style_btn_dark, LV_OPA_COVER);
    lv_style_set_bg_color(&s_style_btn_dark, C_INK);
    lv_style_set_border_width(&s_style_btn_dark, 2);
    lv_style_set_border_color(&s_style_btn_dark, C_INK);
    lv_style_set_border_opa(&s_style_btn_dark, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_dark, 0);
    lv_style_set_shadow_width(&s_style_btn_dark, 0);
    lv_style_set_pad_all(&s_style_btn_dark, 0);
    lv_style_set_text_color(&s_style_btn_dark, C_WHITE);

    lv_style_init(&s_style_btn_dark_pressed);
    lv_style_set_bg_opa(&s_style_btn_dark_pressed, LV_OPA_COVER);
    lv_style_set_bg_color(&s_style_btn_dark_pressed, C_WHITE);
    lv_style_set_text_color(&s_style_btn_dark_pressed, C_INK);
}

static void make_screen_base(lv_obj_t *obj, lv_color_t bg)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, bg, 0);
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

static lv_obj_t *make_icon(lv_obj_t *parent, const char *key, int size)
{
    const workshop_icon_t *entry = workshop_icon_find(key);
    if (!entry) entry = workshop_icon_find("app_default");

    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_style_all(img);
    const lv_image_dsc_t *src = entry->img64;
    if (size <= 32 && entry->img32) src = entry->img32;
    else if (size >= 72 && entry->img72) src = entry->img72;
    lv_image_set_src(img, src);
    lv_obj_set_size(img, size, size);
    lv_image_set_scale(img, (uint32_t)(256 * size / src->header.w));
    lv_obj_set_style_image_recolor(img, C_INK, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

static int app_icon_src(const workshop_app_t *app, char *src, size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/icon.png", app->dir);
    if (access(path, R_OK) == 0) {
        snprintf(src, len, "A:%s", path);
        return 0;
    }
    return -1;
}

static lv_obj_t *make_app_icon(lv_obj_t *parent, const workshop_app_t *app,
                               int size)
{
    char src[640];
    lv_image_header_t header;

    if (app_icon_src(app, src, sizeof(src)) != 0)
        return make_icon(parent, "app_default", size);
    if (lv_image_decoder_get_info(src, &header) != LV_RESULT_OK)
        return make_icon(parent, "app_default", size);

    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_style_all(img);
    lv_image_set_src(img, src);
    lv_obj_set_size(img, size, size);
    if (header.w > 0)
        lv_image_set_scale(img, (uint32_t)(256 * size / header.w));
    // App icons must be RGBA with a transparent background.  Recoloring an
    // opaque RGB icon with full opacity would turn its white background into
    // a solid black square.  Press feedback still recolors the icon through
    // set_tile_inverted(), which is correct for RGBA glyphs.
    return img;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int width, int height, int dark,
                             lv_event_cb_t cb, void *user_data, int disabled)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(btn, dark ? &s_style_btn_dark : &s_style_btn, 0);
    lv_obj_add_style(btn, dark ? &s_style_btn_dark_pressed : &s_style_btn_pressed,
                     LV_STATE_PRESSED);
    lv_obj_add_style(btn, &s_style_btn_disabled, LV_STATE_DISABLED);
    lv_obj_set_size(btn, width, height);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_remove_style_all(label);
    lv_obj_set_style_text_font(label, s_font_bold_24, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    if (disabled) lv_obj_add_state(btn, LV_STATE_DISABLED);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

static void set_tile_inverted(lv_obj_t *tile, int inverted)
{
    lv_obj_set_style_bg_color(tile, inverted ? C_INK : C_WHITE, 0);
    lv_obj_set_style_text_color(tile, inverted ? C_WHITE : C_INK, 0);

    lv_obj_t *icon = lv_obj_get_child(tile, 0);
    lv_obj_t *name = lv_obj_get_child(tile, 1);
    if (icon) {
        lv_obj_set_style_image_recolor(icon, inverted ? C_WHITE : C_INK, 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    }
    if (name) lv_obj_set_style_text_color(name, inverted ? C_WHITE : C_INK, 0);
}

static void close_modal_cb(lv_event_t *e)
{
    lv_obj_t *overlay = lv_event_get_user_data(e);
    if (overlay) lv_obj_delete(overlay);
    s_launching = 0;
    s_pending_app = NULL;
    lv_async_call(build_ui_async, NULL);
}

static lv_obj_t *open_modal(void)
{
    lv_obj_clean(lv_layer_top());
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    make_screen_base(overlay, C_WHITE);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return overlay;
}

static void show_app_placeholder(workshop_app_t *app)
{
    lv_obj_t *overlay = open_modal();
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const workshop_platform_info_t *info = platform_info();
    const int head_h = info->portrait ? 80 : 72;
    const int foot_h = info->portrait ? 88 : 72;

    lv_obj_t *head = lv_obj_create(overlay);
    make_screen_base(head, C_WHITE);
    lv_obj_set_size(head, LV_PCT(100), head_h);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_border_color(head, C_INK, 0);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(head, info->portrait ? 20 : 24, 0);
    lv_obj_set_style_pad_right(head, info->portrait ? 20 : 24, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_obj_create(head);
    make_screen_base(title, C_WHITE);
    lv_obj_set_size(title, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title, 12, 0);

    make_app_icon(title, app, 44);
    make_label(title, app->name, s_font_bold_24, C_INK);

    make_label(head, "自制 App", s_font_20, C_INK);

    lv_obj_t *body = lv_obj_create(overlay);
    make_screen_base(body, C_WHITE);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(body, 24, 0);
    lv_obj_set_style_pad_row(body, 12, 0);

    make_app_icon(body, app, 96);
    make_label(body, app->name, s_font_bold_24, C_INK);

    lv_obj_t *text = make_label(body,
        "真实运行时，会切换到该 App 的全屏画面；退出后回到 workshop。",
        s_font_24, C_INK);
    lv_obj_set_width(text, LV_PCT(90));
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *foot = lv_obj_create(overlay);
    make_screen_base(foot, C_WHITE);
    lv_obj_set_size(foot, LV_PCT(100), foot_h);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, C_INK, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_button(foot, "返回 workshop", info->portrait ? 200 : 220, 44, 0,
                close_modal_cb, overlay, 0);
}

static void show_error_dialog(const char *title, const char *message,
                              const char *detail)
{
    const workshop_platform_info_t *info = platform_info();
    lv_obj_t *overlay = open_modal();

    lv_obj_t *dialog = lv_obj_create(overlay);
    make_screen_base(dialog, C_WHITE);
    lv_obj_set_width(dialog, info->portrait ? 432 : 520);
    lv_obj_set_height(dialog, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(dialog, 2, 0);
    lv_obj_set_style_border_color(dialog, C_INK, 0);
    lv_obj_set_style_pad_all(dialog, 24, 0);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dialog, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(dialog, 12, 0);

    make_label(dialog, title, s_font_32, C_INK);
    make_label(dialog, message, s_font_24, C_INK);

    lv_obj_t *detail_box = lv_obj_create(dialog);
    make_screen_base(detail_box, C_WHITE);
    lv_obj_set_width(detail_box, LV_PCT(100));
    lv_obj_set_height(detail_box, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(detail_box, 1, 0);
    lv_obj_set_style_border_color(detail_box, C_INK, 0);
    lv_obj_set_style_pad_all(detail_box, 12, 0);

    lv_obj_t *detail_label = make_label(detail_box, detail, s_font_20, C_INK);
    lv_obj_set_width(detail_label, LV_PCT(100));
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *actions = lv_obj_create(dialog);
    make_screen_base(actions, C_WHITE);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_button(actions, "返回", 120, 44, 0, close_modal_cb, overlay, 0);
}

static void return_desktop_confirm_cb(lv_event_t *e)
{
    lv_obj_t *overlay = lv_event_get_user_data(e);
    if (overlay) lv_obj_delete(overlay);
#ifdef WORKSHOP_DEVICE
    platform_request_exit();
#else
    lv_async_call(show_desktop_exit_async, NULL);
#endif
}

static void show_return_desktop_dialog(void)
{
    const workshop_platform_info_t *info = platform_info();
    lv_obj_t *overlay = open_modal();

    lv_obj_t *dialog = lv_obj_create(overlay);
    make_screen_base(dialog, C_WHITE);
    lv_obj_set_width(dialog, info->portrait ? 432 : 520);
    lv_obj_set_height(dialog, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(dialog, 2, 0);
    lv_obj_set_style_border_color(dialog, C_INK, 0);
    lv_obj_set_style_pad_all(dialog, 24, 0);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dialog, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(dialog, 12, 0);

    make_label(dialog, "返回桌面？", s_font_32, C_INK);
    make_label(dialog, "退出 workshop，返回系统桌面。", s_font_24, C_INK);

    lv_obj_t *actions = lv_obj_create(dialog);
    make_screen_base(actions, C_WHITE);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 16, 0);

    make_button(actions, "取消", 120, 44, 0, close_modal_cb, overlay, 0);

    /* Confirm: drop the dialog, then show the simulated desktop. */
    lv_obj_t *confirm = make_button(actions, "返回桌面", 160, 44, 1, NULL, NULL, 0);
    lv_obj_add_event_cb(confirm, return_desktop_confirm_cb, LV_EVENT_CLICKED, overlay);
}

void launcher_show_return_desktop(void) { show_return_desktop_dialog(); }

static void desktop_exit_cb(lv_event_t *e)
{
    lv_obj_t *overlay = lv_event_get_user_data(e);
    if (overlay) lv_obj_delete(overlay);
    s_launching = 0;
    lv_async_call(build_ui_async, NULL);
}

static void show_desktop_exit(void)
{
    lv_obj_t *overlay = open_modal();
    lv_obj_set_style_pad_row(overlay, 16, 0);

    make_label(overlay, "已返回桌面", s_font_32, C_INK);

    lv_obj_t *text = make_label(overlay,
        "这是本地模拟。真机上会恢复进入前的系统桌面。",
        s_font_24, C_INK);
    lv_obj_set_width(text, LV_PCT(80));
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);

    make_button(overlay, "重新打开 workshop", 240, 44, 0,
                desktop_exit_cb, overlay, 0);
}

static void launch_timer_cb(lv_timer_t *timer)
{
    lv_timer_delete(timer);
    s_launch_timer = NULL;
    show_app_placeholder(s_pending_app);
}

static void start_mock_launch(workshop_app_t *app)
{
    s_pending_app = app;
    s_launch_timer = lv_timer_create(launch_timer_cb, MOCK_LAUNCH_MS, NULL);
    if (s_launch_timer) lv_timer_set_repeat_count(s_launch_timer, 1);
}

static void tile_press_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_pressed_tile || s_tile_press_shown) return;

    if (platform_ticks_ms() - s_tile_press_at >= 200U) {
        set_tile_inverted(s_pressed_tile, 1);
        s_tile_press_shown = 1;
    }
}

static void tile_release_press(void)
{
    if (s_pressed_tile && s_tile_press_shown) {
        set_tile_inverted(s_pressed_tile, 0);
    }
    s_pressed_tile = NULL;
    s_tile_press_shown = 0;
}

static void tile_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *tile = lv_event_get_target(e);
    workshop_app_t *app = lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        if (s_launching) return;

        if (s_pressed_tile && s_pressed_tile != tile &&
            s_tile_press_shown) {
            set_tile_inverted(s_pressed_tile, 0);
        }

        s_pressed_tile = tile;
        s_tile_press_at = platform_ticks_ms();
        s_tile_press_shown = 0;

        if (!s_tile_press_timer) {
            s_tile_press_timer = lv_timer_create(tile_press_timer_cb, 50,
                                                 NULL);
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        if (!s_launching) tile_release_press();
    } else if (code == LV_EVENT_DELETE) {
        if (s_pressed_tile == tile) {
            s_pressed_tile = NULL;
            s_tile_press_shown = 0;
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (s_launching) return;
        tile_release_press();
        s_launching = 1;
        if (app->special == 1) {
            show_return_desktop_dialog();
        } else if (app->special == 2) {
            settings_open();
        } else {
#ifdef WORKSHOP_DEVICE
            char err[512] = {0};
            char message[160];
            snprintf(message, sizeof(message), "无法启动 %s。", app->name);
            int rc = platform_launch(app->dir, app->exec, err, sizeof(err));
            s_launching = 0;
            if (rc != 0) {
                show_error_dialog("启动失败", message, err);
            } else {
                lv_async_call(build_ui_async, NULL);
            }
#else
            start_mock_launch(app);
#endif
        }
    }
}

static void prev_page_cb(lv_event_t *e)
{
    (void)e;
    if (s_launching) return;
    if (s_page > 0) {
        s_page--;
        lv_async_call(build_ui_async, NULL);
    }
}

static void next_page_cb(lv_event_t *e)
{
    (void)e;
    if (s_launching) return;
    int pages = (s_app_count + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
    if (pages < 1) pages = 1;
    if (s_page + 1 < pages) {
        s_page++;
        lv_async_call(build_ui_async, NULL);
    }
}

static const char *battery_icon_key(int percent, int charging)
{
    if (charging) return "battery_charging";
    if (percent < 0) return "battery_empty";
    if (percent >= 80) return "battery_full";
    if (percent >= 50) return "battery_medium";
    if (percent >= 20) return "battery_low";
    return "battery_empty";
}

static int wifi_signal_bars(int present, int connected, int rssi)
{
    if (!present) return -1;       /* WiFi off: hide the icon */
    if (!connected) return 0;      /* On but not associated: all gray */
    if (rssi >= -65) return 3;
    if (rssi >= -80) return 2;
    return 1;
}

static void update_status(int force)
{
    if (!s_status_wifi_box || !s_status_battery ||
        !s_status_percent_label || !s_status_time_label) {
        return;
    }

    workshop_status_t status;
    platform_get_status(&status);

    int wifi_bars = wifi_signal_bars(
        status.wifi_present, status.wifi_connected, status.wifi_rssi);
    const char *battery_key =
        battery_icon_key(status.battery_percent, status.charging);

    if (!force &&
        status.hour == s_status_hour &&
        status.minute == s_status_minute &&
        status.battery_percent == s_status_percent &&
        status.charging == s_status_charging &&
        wifi_bars == s_status_wifi_bars) {
        return;
    }

    if (force || wifi_bars != s_status_wifi_bars) {
        if (wifi_bars < 0) {
            lv_obj_add_flag(s_status_wifi_box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_status_wifi_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_image_recolor(
                s_status_wifi_arc3,
                wifi_bars >= 3 ? C_INK : C_GRAY, 0);
            lv_obj_set_style_image_recolor(
                s_status_wifi_arc2,
                wifi_bars >= 2 ? C_INK : C_GRAY, 0);
            lv_obj_set_style_image_recolor(
                s_status_wifi_arc1,
                wifi_bars >= 1 ? C_INK : C_GRAY, 0);
            lv_obj_set_style_image_recolor(
                s_status_wifi_dot,
                wifi_bars >= 1 ? C_INK : C_GRAY, 0);
        }
        s_status_wifi_bars = wifi_bars;
    }

    if (force || !s_status_battery_key ||
        strcmp(battery_key, s_status_battery_key)) {
        const workshop_icon_t *icon = workshop_icon_find(battery_key);
        if (icon && icon->img32) {
            lv_image_set_src(s_status_battery, icon->img32);
        }
        s_status_battery_key = battery_key;
    }

    char percent_text[16];
    if (status.battery_percent >= 0) {
        snprintf(percent_text, sizeof(percent_text), "%d%%",
                 status.battery_percent);
    } else {
        snprintf(percent_text, sizeof(percent_text), "--");
    }

    char time_text[16];
    snprintf(time_text, sizeof(time_text), "%02d:%02d",
             status.hour, status.minute);

    lv_label_set_text(s_status_percent_label, percent_text);
    lv_label_set_text(s_status_time_label, time_text);

    s_status_hour = status.hour;
    s_status_minute = status.minute;
    s_status_percent = status.battery_percent;
    s_status_charging = status.charging;
}

static void status_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Settings, dialogs and the keyboard overlay cover the launcher status
     * bar. Updating it in the background would only cause an e-ink refresh. */
    if (lv_obj_get_child_count(lv_layer_top()) > 0) return;

    update_status(0);
}

static void build_ui(void)
{
    const workshop_platform_info_t *info = platform_info();
    const int portrait = info->portrait;
    const int header_h = portrait ? 96 : 88;
    const int footer_h = portrait ? 88 : 72;
    const int pad_x = portrait ? 20 : 24;
    const int pages = s_app_count > 0
                          ? (s_app_count + APPS_PER_PAGE - 1) / APPS_PER_PAGE
                          : 1;

    if (s_page < 0) s_page = 0;
    if (s_page >= pages) s_page = pages - 1;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    make_screen_base(screen, C_WHITE);
    lv_obj_set_size(screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *header = lv_obj_create(screen);
    make_screen_base(header, C_WHITE);
    lv_obj_set_size(header, LV_PCT(100), header_h);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, C_INK, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(header, pad_x, 0);
    lv_obj_set_style_pad_right(header, pad_x, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_label(header, "workshop", s_font_32, C_INK);

    lv_obj_t *status = lv_obj_create(header);
    make_screen_base(status, C_WHITE);
    lv_obj_set_size(status, 250, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, 10, 0);

    s_status_wifi_box = lv_obj_create(status);
    make_screen_base(s_status_wifi_box, C_WHITE);
    lv_obj_set_size(s_status_wifi_box, 32, 32);

    s_status_wifi_arc3 = make_icon(s_status_wifi_box, "wifi_arc3", 32);
    lv_obj_set_pos(s_status_wifi_arc3, 0, 0);
    s_status_wifi_arc2 = make_icon(s_status_wifi_box, "wifi_arc2", 32);
    lv_obj_set_pos(s_status_wifi_arc2, 0, 0);
    s_status_wifi_arc1 = make_icon(s_status_wifi_box, "wifi_arc1", 32);
    lv_obj_set_pos(s_status_wifi_arc1, 0, 0);
    s_status_wifi_dot = make_icon(s_status_wifi_box, "wifi_dot", 32);
    lv_obj_set_pos(s_status_wifi_dot, 0, 0);

    s_status_battery = make_icon(status, "battery_full", 32);
    s_status_percent_label =
        make_label(status, "", s_font_status, C_INK);
    s_status_time_label =
        make_label(status, "", s_font_status, C_INK);
    update_status(1);

    lv_obj_t *content = lv_obj_create(screen);
    make_screen_base(content, C_WHITE);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, portrait ? 20 : 24, 0);

    lv_obj_t *grid = lv_obj_create(content);
    make_screen_base(grid, C_WHITE);
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);

    const int cols = portrait ? 2 : 3;
    const int rows = portrait ? 3 : 2;
    for (int i = 0; i < cols; i++) s_col_dsc[i] = LV_GRID_FR(1);
    s_col_dsc[cols] = LV_GRID_TEMPLATE_LAST;
    for (int i = 0; i < rows; i++) s_row_dsc[i] = LV_GRID_FR(1);
    s_row_dsc[rows] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(grid, s_col_dsc, s_row_dsc);

    const int start = s_page * APPS_PER_PAGE;
    int shown = s_app_count - start;
    if (shown < 0) shown = 0;
    if (shown > APPS_PER_PAGE) shown = APPS_PER_PAGE;

    s_first_tile = NULL;
    for (int i = 0; i < shown; i++) {
        workshop_app_t *app = &s_apps[start + i];
        lv_obj_t *tile = lv_obj_create(grid);
        lv_obj_remove_style_all(tile);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(tile, &s_style_tile, 0);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tile, 10, 0);

        const int col = i % cols;
        const int row = i / cols;
        lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, col, 1,
                             LV_GRID_ALIGN_STRETCH, row, 1);
        if (i == 0) s_first_tile = tile;

        make_app_icon(tile, app, portrait ? 72 : 64);

        lv_obj_t *name = make_label(tile, app->name, s_font_bold_24, C_INK);
        lv_obj_set_width(name, LV_PCT(100));
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);

        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_ALL, app);
    }

    if (s_app_count == 0) {
        lv_obj_t *empty = make_label(content, "没有 App", s_font_32, C_INK);
        lv_obj_center(empty);
    }

    lv_obj_t *footer = lv_obj_create(screen);
    make_screen_base(footer, C_WHITE);
    lv_obj_set_size(footer, LV_PCT(100), footer_h);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_border_color(footer, C_INK, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_left(footer, pad_x, 0);
    lv_obj_set_style_pad_right(footer, pad_x, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(footer, 12, 0);

    make_button(footer, "上一页", portrait ? 96 : 120, 44, 0,
                prev_page_cb, NULL, s_page == 0);

    char page_buf[32];
    snprintf(page_buf, sizeof(page_buf), "%d / %d", s_page + 1, pages);
    lv_obj_t *page_label = make_label(footer, page_buf, s_font_20, C_INK);
    lv_obj_set_style_text_align(page_label, LV_TEXT_ALIGN_CENTER, 0);

    make_button(footer, "下一页", portrait ? 96 : 120, 44, 0,
                next_page_cb, NULL, s_page >= pages - 1);
}

void launcher_init(void)
{
    const int loaded = manifest_load(platform_apps_dir(), s_apps, WORKSHOP_MAX_APPS);
    int kept = 0;
    for (int i = 0; i < loaded; ++i) {
        if (strcmp(s_apps[i].id, "return_desktop") == 0 ||
            strcmp(s_apps[i].id, "settings") == 0)
            continue;
        if (kept != i) s_apps[kept] = s_apps[i];
        ++kept;
    }
    s_app_count = kept;
    if (s_app_count < WORKSHOP_MAX_APPS) {
        workshop_app_t *app = &s_apps[s_app_count];
        memset(app, 0, sizeof(*app));
        snprintf(app->id, sizeof(app->id), "settings");
        snprintf(app->name, sizeof(app->name), "设置");
        snprintf(app->dir, sizeof(app->dir), "%s/settings", platform_apps_dir());
        app->special = 2;
        ++s_app_count;
    }
    s_page = 0;

    s_font_20 = lv_tiny_ttf_create_file(platform_font_light(), 20);
    s_font_24 = lv_tiny_ttf_create_file(platform_font_light(), 24);
    s_font_bold_24 = lv_tiny_ttf_create_file(platform_font_bold(), 24);
    s_font_32 = lv_tiny_ttf_create_file(platform_font_bold(), 32);
    s_font_status = lv_tiny_ttf_create_file(platform_font_bold(), 28);

    if (!s_font_20) s_font_20 = &lv_font_montserrat_20;
    if (!s_font_24) s_font_24 = &lv_font_montserrat_24;
    if (!s_font_bold_24) s_font_bold_24 = &lv_font_montserrat_24;
    if (!s_font_32) s_font_32 = &lv_font_montserrat_32;
    if (!s_font_status) s_font_status = &lv_font_montserrat_28;

    style_init();
    settings_init();
    build_ui();
    if (!s_status_timer) {
        s_status_timer = lv_timer_create(status_timer_cb, 60000, NULL);
    }

    if (getenv("WORKSHOP_DEMO_PAGE2")) {
        s_page = 1;
        build_ui();
    }
    if (getenv("WORKSHOP_DEMO_ERROR")) {
        launcher_show_error_demo();
    }
    if (getenv("WORKSHOP_DEMO_APP")) {
        s_launching = 1;
        show_app_placeholder(&s_apps[0]);
    }
    if (getenv("WORKSHOP_DEMO_RETURN")) {
        s_launching = 1;
        show_return_desktop_dialog();
    }
    if (getenv("WORKSHOP_DEMO_LAUNCH") && s_first_tile) {
        s_launching = 1;
        set_tile_inverted(s_first_tile, 1);
        start_mock_launch(&s_apps[0]);
    }
}

void launcher_rebuild(void)
{
    if (s_launch_timer) {
        lv_timer_delete(s_launch_timer);
        s_launch_timer = NULL;
    }
    s_pending_app = NULL;
    s_launching = 0;
    lv_obj_clean(lv_layer_top());
    build_ui();
}

void launcher_show_error_demo(void)
{
    if (s_launching) return;
    show_error_dialog(
        "启动失败",
        "无法启动 Hello。",
        "启动脚本返回 127\n找不到程序：\n/user/workshop/app/hello/bin/hello\n日志：/user/workshop/app/hello/logs/hello.log");
}
