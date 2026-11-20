#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <GfxRenderer.h>

#include "CrossPointSettings.h"

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

  // Keep CrossMux in its normal Portrait layout.  OBOOK5's 180-degree panel
  // mounting is handled by HalDisplay/HalGPIO, so the reader's saved
  // orientation setting keeps working too.
  renderer.setOrientation(GfxRenderer::Portrait);

  std::signal(SIGINT, request_quit);
  std::signal(SIGTERM, request_quit);
  std::signal(SIGPIPE, SIG_IGN);

  setup();

  // The OBOOK5 panel rotation is handled entirely by the HAL.  Keep the reader
  // on the native Portrait layout regardless of any previously saved value.
  SETTINGS.orientation = CrossPointSettings::ORIENTATION::PORTRAIT;

  while (!display.shouldQuit()) {
    gpio.beginFrame();
    loop();
    display.presentIfNeeded();
    usleep(1000);
  }
  _exit(0);
}
