#include "launcher.h"
#include "platform.h"
#include "workshop_ime.h"

#include "lvgl.h"

int main(void)
{
    if (platform_init() != 0) return 1;

    workshop_ime_init();
    launcher_init();

    while (!platform_should_quit()) {
        platform_poll_events();

        if (platform_take_orientation_changed()) {
            launcher_rebuild();
        }
        if (platform_take_error_request()) {
            launcher_show_error_demo();
        }

        uint32_t next = lv_timer_handler();
        if (next < 5) next = 5;
        if (next > 20) next = 20;
        platform_sleep_ms(next);
    }

    platform_deinit();
    return 0;
}
