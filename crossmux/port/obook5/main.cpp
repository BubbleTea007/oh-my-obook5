#include <csignal>
#include <unistd.h>

#include "HalDisplay.h"
#include "HalGPIO.h"

extern void setup();
extern void loop();
extern volatile sig_atomic_t obook5_quit_requested;

namespace {
void request_quit(int) { obook5_quit_requested = 1; }
}

int main() {
  std::signal(SIGINT, request_quit);
  std::signal(SIGTERM, request_quit);

  setup();
  while (!display.shouldQuit()) {
    gpio.beginFrame();
    loop();
    display.presentIfNeeded();
    usleep(1000);
  }
  _exit(0);
}
