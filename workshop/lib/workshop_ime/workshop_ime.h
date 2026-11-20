#pragma once

#include "lvgl.h"

/*
 * Shared on-screen keyboard for Workshop and its LVGL apps.
 *
 * Call workshop_ime_init() once after lv_init(). Any lv_textarea on the
 * active screen, top layer or system layer is detected automatically and
 * pops the keyboard when focused or tapped.
 *
 * Password textareas (lv_textarea_set_password_mode(ta, true)) use a plain
 * ASCII keyboard; all other textareas use the pinyin IME.
 */

void workshop_ime_init(void);
void workshop_ime_set_font(const lv_font_t *font);

void workshop_ime_show(lv_obj_t *textarea);
void workshop_ime_hide(void);

/* Optional explicit helpers; auto-detection already binds plain textareas. */
void workshop_textarea_bind(lv_obj_t *textarea);
lv_obj_t *workshop_textarea_create(lv_obj_t *parent);
