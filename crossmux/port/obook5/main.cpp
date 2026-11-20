#include <csignal>
#include <cstdlib>
#include <unistd.h>

#include "HalDisplay.h"
#include "HalGPIO.h"
#include "SimulatorLifecycle.h"

extern void setup();
extern void loop();
extern volatile sig_atomic_t obook5_quit_requested;

namespace {
void request_quit(int) { obook5_quit_requested = 1; }
}  // namespace

int main(int argc, char** argv) {
  // The simulator HALs use CROSSPOINT_SIM_SD as their storage root.  On the
  // OBOOK5 that is the user partition; books live in /user/毕昇读库 and the
  // reader is free to create /user/.crosspoint state next to them.
  setenv("CROSSPOINT_SIM_SD", "/user", 0);
  SimulatorLifecycle::initProcessArgs(argv);

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
