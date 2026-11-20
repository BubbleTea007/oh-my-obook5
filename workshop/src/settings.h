#pragma once

/* Built-in Workshop settings UI.  Called from the launcher; no separate app
 * binary, and it shares the launcher's framebuffer/touch session. */
void settings_init(void);
void settings_open(void);
