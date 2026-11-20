# Host shims (Arduino / ESP-IDF / FreeRTOS)

This directory is a snapshot of the CrossPoint simulator's host shim layer,
vendored into the OBOOK5 port so the device build does **not** compile or link
anything from `crossmux/simulator/`.

- Source: https://github.com/0x1abin/crosspoint-simulator
- Commit: `e8a64964ea36982c3f8caca02a8d64162a06a58c`
- License: MIT (see `LICENSE`)

Changes from the snapshot:

- `HalDisplay.cpp`, `HalGPIO.cpp` and `simulator_main.cpp` are not copied;
  `crossmux/port/obook5/` provides OBOOK5 display, touch and `main()`.
- Deliberate local patches are kept small and tracked in git, so the shim layer
  can be re-synced from the simulator when needed.

The simulator submodule remains as the reference implementation and source of
future shim updates.
