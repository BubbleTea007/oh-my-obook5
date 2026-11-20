# OBOOK5 HAL overlay

These files replace the simulator's SDL-backed `HalDisplay`, `HalGPIO` and
`main` while keeping the CrossMux UI and reader core unchanged.

- `HalDisplay.cpp`: 800x480 1-bit framebuffer -> `/dev/fb0`, `FBIOPAN_DISPLAY`
- `HalGPIO.cpp`: `/dev/input/event1` touch -> CrossMux touch API
- `main.cpp`: setup/loop, signal handling, 1 ms frame yield

The overlay intentionally implements the generic Linux path. OBOOK5-specific
e-ink timing, if needed, belongs in `linux/fbdev.cpp` or a small separate hook,
not in CrossMux code.

Build integration is not wired yet. It needs a PlatformIO/CMake target that:

1. compiles upstream CrossMux sources
2. compiles simulator shims except `HalDisplay.cpp`, `HalGPIO.cpp`, `simulator_main.cpp`
3. compiles this overlay
4. defines `FREEINK_DEVICE_X4PRO=1`, `SIMULATOR`, `CROSSPOINT_EMULATED=1`
5. uses `/user` as the storage root
