#ifndef WORKSHOP_ICONS_H
#define WORKSHOP_ICONS_H

#include "lvgl.h"

typedef struct {
    const char *name;
    const lv_image_dsc_t *img32;
    const lv_image_dsc_t *img64;
    const lv_image_dsc_t *img72;
} workshop_icon_t;

const workshop_icon_t *workshop_icon_find(const char *name);

#endif
