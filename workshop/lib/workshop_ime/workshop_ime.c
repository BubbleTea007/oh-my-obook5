#include "workshop_ime.h"

#include <stdbool.h>
#include <string.h>

#define MAX_BOUND_TEXTAREAS 24
#define SCAN_PERIOD_MS 400
#define CAND_PANEL_HEIGHT 52
#define FALLBACK_FONT_PATH "A:/user/workshop/fonts/LXGWWenKai-Regular.ttf"
#define FALLBACK_FONT_SIZE 22
#define KEYBOARD_FONT (&lv_font_montserrat_20)

static lv_obj_t *s_kb_text;
static lv_obj_t *s_kb_ascii;
static lv_obj_t *s_ime;
static lv_obj_t *s_cand;
static lv_obj_t *s_active;
static lv_obj_t *s_bound[MAX_BOUND_TEXTAREAS];
static lv_timer_t *s_scan_timer;
static lv_timer_t *s_cursor_guard_timer;
static lv_font_t *s_loaded_font;
static const lv_font_t *s_font;
static bool s_init_done;
static bool s_text_used;

static const lv_font_t *ime_font(void)
{
    if (s_font) return s_font;
    if (!s_loaded_font) {
        s_loaded_font = lv_tiny_ttf_create_file(FALLBACK_FONT_PATH,
                                                FALLBACK_FONT_SIZE);
    }
    return s_loaded_font ? s_loaded_font : &lv_font_montserrat_14;
}

static void style_keyboard(lv_obj_t *kb)
{
    if (!kb) return;

    lv_obj_set_style_bg_color(kb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 1, 0);
    lv_obj_set_style_border_color(kb, lv_color_black(), 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 3, 0);
    lv_obj_set_style_text_font(kb, KEYBOARD_FONT, 0);

    lv_obj_set_style_bg_color(kb, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, KEYBOARD_FONT, LV_PART_ITEMS);

    /* Pressed keys must look identical to normal keys: the character and
     * the persistent last-key highlight are rendered in the same frame. */
    lv_obj_set_style_bg_color(kb, lv_color_white(),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(kb, lv_color_black(),
                                  LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_black(),
                                LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_recolor_opa(kb, LV_OPA_TRANSP,
                                 LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_set_style_bg_color(kb, lv_color_black(),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER,
                            LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(kb, lv_color_black(),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_white(),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_recolor_opa(kb, LV_OPA_TRANSP,
                                 LV_PART_ITEMS | LV_STATE_CHECKED);

    /* A key that is both pressed and selected must stay black. Otherwise
     * the pressed white style overrides the checked style and the release
     * frame needs a second e-ink refresh. */
    lv_obj_set_style_bg_color(kb, lv_color_black(),
                              LV_PART_ITEMS | LV_STATE_PRESSED |
                                  LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER,
                            LV_PART_ITEMS | LV_STATE_PRESSED |
                                LV_STATE_CHECKED);
    lv_obj_set_style_border_color(kb, lv_color_black(),
                                  LV_PART_ITEMS | LV_STATE_PRESSED |
                                      LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_white(),
                                LV_PART_ITEMS | LV_STATE_PRESSED |
                                    LV_STATE_CHECKED);
    lv_obj_set_style_recolor_opa(kb, LV_OPA_TRANSP,
                                 LV_PART_ITEMS | LV_STATE_PRESSED |
                                     LV_STATE_CHECKED);

    /* The persistent checked key is the press feedback. No pressed style
     * means the press and release frames are identical. */
    lv_obj_remove_style(kb, NULL, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_remove_style(kb, NULL, LV_PART_ITEMS | LV_STATE_PRESSED |
                                    LV_STATE_CHECKED);
}

static void style_cand_panel(lv_obj_t *cand)
{
    if (!cand) return;

    lv_obj_set_style_bg_color(cand, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cand, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cand, 1, 0);
    lv_obj_set_style_border_color(cand, lv_color_black(), 0);
    lv_obj_set_style_radius(cand, 0, 0);
    lv_obj_set_style_pad_all(cand, 4, 0);
    lv_obj_set_style_pad_gap(cand, 2, 0);
    lv_obj_set_style_text_color(cand, lv_color_black(), 0);

    lv_obj_set_style_bg_color(cand, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(cand, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(cand, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(cand, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(cand, lv_color_black(), LV_PART_ITEMS);

    lv_obj_set_style_bg_color(cand, lv_color_white(),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(cand, lv_color_black(),
                                LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(cand, 0,
                                  LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_recolor_opa(cand, LV_OPA_TRANSP,
                                 LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_remove_style(cand, NULL, LV_PART_ITEMS | LV_STATE_PRESSED);
}

static void apply_font(void)
{
    const lv_font_t *font = ime_font();

    if (s_ime) lv_obj_set_style_text_font(s_ime, font, 0);
    if (s_cand) {
        lv_obj_set_style_text_font(s_cand, font, 0);
        lv_obj_set_style_text_font(s_cand, font, LV_PART_ITEMS);
    }
}

static void layout_keyboards(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) return;

    const int32_t width = lv_display_get_horizontal_resolution(disp);
    const int32_t height = lv_display_get_vertical_resolution(disp);
    int32_t kb_height = height * 38 / 100;

    if (kb_height < 180) kb_height = height / 2;
    if (kb_height > height - 120) kb_height = height - 120;

    if (s_kb_text) {
        lv_obj_set_size(s_kb_text, width, kb_height);
        lv_obj_align(s_kb_text, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    if (s_kb_ascii) {
        lv_obj_set_size(s_kb_ascii, width, kb_height);
        lv_obj_align(s_kb_ascii, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    if (s_cand) {
        lv_obj_set_size(s_cand, width, CAND_PANEL_HEIGHT);
        lv_obj_align_to(s_cand, s_kb_text, LV_ALIGN_OUT_TOP_MID, 0, 0);
    }
}

static void kb_event_cb(lv_event_t *e);

static void prepare_key_highlight(lv_obj_t *kb)
{
    if (!kb) return;
    lv_buttonmatrix_set_one_checked(kb, true);
    lv_buttonmatrix_clear_button_ctrl_all(kb,
                                          LV_BUTTONMATRIX_CTRL_CHECKED);
}

static void kb_value_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_current_target(e);
    if (!kb) return;

    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;

    lv_buttonmatrix_clear_button_ctrl_all(kb,
                                          LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_buttonmatrix_set_button_ctrl(kb, id, LV_BUTTONMATRIX_CTRL_CHECKED);
}

static void create_text_keyboard(void)
{
    if (s_kb_text) return;

    lv_obj_t *sys = lv_layer_sys();

    s_kb_text = lv_keyboard_create(sys);
    style_keyboard(s_kb_text);
    lv_obj_add_flag(s_kb_text, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb_text, kb_event_cb, LV_EVENT_ALL, NULL);

    s_ime = lv_ime_pinyin_create(sys);
    if (s_ime) {
        lv_ime_pinyin_set_keyboard(s_ime, s_kb_text);
        s_cand = lv_ime_pinyin_get_cand_panel(s_ime);
        style_cand_panel(s_cand);
        if (s_cand) lv_obj_add_flag(s_cand, LV_OBJ_FLAG_HIDDEN);
    }

    /* Registered after the keyboard and IME handlers so the highlight is
     * applied last, including after a keyboard mode change. */
    lv_obj_add_event_cb(s_kb_text, kb_value_cb, LV_EVENT_VALUE_CHANGED, NULL);
    prepare_key_highlight(s_kb_text);

    layout_keyboards();
}

static void destroy_text_keyboard(void)
{
    if (s_ime) {
        lv_obj_delete(s_ime);
    } else if (s_kb_text) {
        lv_obj_delete(s_kb_text);
    }

    s_ime = NULL;
    s_kb_text = NULL;
    s_cand = NULL;
}

static void ime_hide_now(void)
{
    if (s_kb_text) {
        lv_obj_add_flag(s_kb_text, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb_text, NULL);
    }
    if (s_kb_ascii) {
        lv_obj_add_flag(s_kb_ascii, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb_ascii, NULL);
    }
    if (s_cand) lv_obj_add_flag(s_cand, LV_OBJ_FLAG_HIDDEN);
    s_active = NULL;
}

static void show_for(lv_obj_t *ta)
{
    if (!ta || !lv_obj_is_valid(ta)) return;
    if (!s_init_done) workshop_ime_init();
    if (s_active == ta) return;

    const bool ascii = lv_textarea_get_password_mode(ta);

    /*
     * The pinyin IME keeps an in-progress composition (input buffer and
     * character count). Reusing that state for another textarea would make
     * candidate selection delete characters from the new field, so a fresh
     * IME keyboard is created whenever the target changes.
     */
    if (s_text_used && (ascii || s_active != ta)) {
        destroy_text_keyboard();
        s_text_used = false;
    }

    if (!ascii && !s_kb_text) create_text_keyboard();

    lv_obj_t *kb = ascii ? s_kb_ascii : s_kb_text;
    lv_obj_t *other = ascii ? s_kb_text : s_kb_ascii;

    if (!kb) return;

    apply_font();
    /* The textarea may have started its blink animation before the local
     * style was applied. Kill it explicitly; blinking would refresh the
     * whole e-ink panel forever. */
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_anim_delete(ta, NULL);

    s_active = ta;
    if (!ascii) s_text_used = true;

    if (other) {
        lv_obj_add_flag(other, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(other, NULL);
    }

    lv_keyboard_set_textarea(kb, ta);

    if (ascii) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        if (s_cand) lv_obj_add_flag(s_cand, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_ime) lv_ime_pinyin_set_mode(s_ime, LV_IME_PINYIN_MODE_K26);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        if (s_cand) lv_obj_add_flag(s_cand, LV_OBJ_FLAG_HIDDEN);
    }
    prepare_key_highlight(kb);

    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
    if (s_cand) lv_obj_move_foreground(s_cand);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

static void unbind_textarea(lv_obj_t *ta)
{
    for (int i = 0; i < MAX_BOUND_TEXTAREAS; ++i) {
        if (s_bound[i] == ta) {
            s_bound[i] = NULL;
            return;
        }
    }
}

static void ta_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target_obj(e);
    if (!ta) return;

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        show_for(ta);
    } else if (code == LV_EVENT_DEFOCUSED) {
        if (s_active == ta) ime_hide_now();
    } else if (code == LV_EVENT_DELETE) {
        if (s_active == ta) ime_hide_now();
        unbind_textarea(ta);
    }
}

static void kb_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        ime_hide_now();
    }
}

static void bind_textarea(lv_obj_t *ta)
{
    if (!ta || !lv_obj_check_type(ta, &lv_textarea_class)) return;

    for (int i = 0; i < MAX_BOUND_TEXTAREAS; ++i) {
        if (s_bound[i] == ta) return;
    }

    for (int i = 0; i < MAX_BOUND_TEXTAREAS; ++i) {
        if (s_bound[i] && lv_obj_is_valid(s_bound[i])) continue;

        s_bound[i] = ta;
        /* E-ink must not repaint a blinking cursor. The textarea
         * constructor starts a blink before this local style exists, so
         * kill it here as well. */
        lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
        lv_anim_delete(ta, NULL);
        lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, NULL);
        return;
    }
}

static void scan_object(lv_obj_t *obj)
{
    if (!obj) return;

    if (lv_obj_check_type(obj, &lv_textarea_class)) bind_textarea(obj);

    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        scan_object(lv_obj_get_child(obj, i));
    }
}

static void scan_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    scan_object(lv_screen_active());
    scan_object(lv_layer_bottom());
    scan_object(lv_layer_top());
    scan_object(lv_layer_sys());
}

static void display_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RESOLUTION_CHANGED) layout_keyboards();
}

static void cursor_guard_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_active || !lv_obj_is_valid(s_active)) return;

    lv_anim_delete(s_active, NULL);
    lv_obj_t *label = lv_textarea_get_label(s_active);
    if (label) lv_anim_delete(label, NULL);
    if (s_kb_text) lv_anim_delete(s_kb_text, NULL);
    if (s_kb_ascii) lv_anim_delete(s_kb_ascii, NULL);
    if (s_cand) lv_anim_delete(s_cand, NULL);
}

void workshop_ime_init(void)
{
    if (s_init_done) return;

    lv_obj_t *sys = lv_layer_sys();

    create_text_keyboard();

    s_kb_ascii = lv_keyboard_create(sys);
    style_keyboard(s_kb_ascii);
    lv_obj_add_flag(s_kb_ascii, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb_ascii, kb_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(s_kb_ascii, kb_value_cb, LV_EVENT_VALUE_CHANGED, NULL);
    prepare_key_highlight(s_kb_ascii);

    layout_keyboards();

    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_display_add_event_cb(disp, display_event_cb,
                                LV_EVENT_RESOLUTION_CHANGED, NULL);
    }

    s_scan_timer = lv_timer_create(scan_timer_cb, SCAN_PERIOD_MS, NULL);
    s_cursor_guard_timer = lv_timer_create(cursor_guard_timer_cb, 1000, NULL);
    s_init_done = true;
}

void workshop_ime_set_font(const lv_font_t *font)
{
    s_font = font;
    apply_font();
    layout_keyboards();
}

void workshop_ime_show(lv_obj_t *textarea)
{
    show_for(textarea);
}

void workshop_ime_hide(void)
{
    ime_hide_now();
}

void workshop_textarea_bind(lv_obj_t *textarea)
{
    if (!s_init_done) workshop_ime_init();
    bind_textarea(textarea);
}

lv_obj_t *workshop_textarea_create(lv_obj_t *parent)
{
    if (!s_init_done) workshop_ime_init();
    lv_obj_t *ta = lv_textarea_create(parent);
    bind_textarea(ta);
    return ta;
}
