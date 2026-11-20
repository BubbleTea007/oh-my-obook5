#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <GfxRenderer.h>

#include "HalDisplay.h"
#include "HalGPIO.h"
#include "SimulatorLifecycle.h"

extern void setup();
extern void loop();
extern volatile sig_atomic_t obook5_quit_requested;

// Defined by upstream src/main.cpp.
extern GfxRenderer renderer;

namespace {
void request_quit(int) { obook5_quit_requested = 1; }
}  // namespace

int main(int argc, char** argv) {
  // The simulator HALs use CROSSPOINT_SIM_SD as their storage root.  On the
  // OBOOK5 that is the user partition; books live in /user/毕昇读库 and the
  // reader is free to create /user/.crosspoint state next to them.
  fprintf(stderr, "[obook5] CrossMux starting\n");
  setenv("CROSSPOINT_SIM_SD", "/user", 0);
  SimulatorLifecycle::initProcessArgs(argv);

  // The OBOOK5 panel is mounted 180 degrees relative to the X4 Pro profile
  // CrossMux uses.  Its Portrait mapping is a 90-degree clockwise rotation;
  // PortraitInverted is the counter-clockwise mapping the workshop platform
  // already uses (X = 799 - y, Y = x) and keeps touch in sync.
  renderer.setOrientation(GfxRenderer::PortraitInverted);

  std::signal(SIGINT, request_quit);
  std::signal(SIGTERM, request_quit);
  std::signal(SIGPIPE, SIG_IGN);

  setup();
  while (!display.shouldQuit()) {
    gpio.beginFrame();
    loop();
    display.presentIfNeeded();
    usleep(1000);
  }
  _exit(0);
}
