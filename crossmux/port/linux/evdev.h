#pragma once

#include <cstdint>

struct TouchPoint {
  int x = 0;
  int y = 0;
  bool down = false;
};

// Minimal Linux evdev touch reader. It reads the first touch contact and
// normalizes raw ABS ranges to the supplied screen size.
class TouchDev {
 public:
  TouchDev() = default;
  ~TouchDev();

  TouchDev(const TouchDev&) = delete;
  TouchDev& operator=(const TouchDev&) = delete;

  bool open(const char* path = "/dev/input/event1", int screen_width = 800,
            int screen_height = 480);
  void close();
  bool isOpen() const { return fd_ >= 0; }

  // Returns true when a complete SYN_REPORT with a changed contact arrived.
  bool poll(TouchPoint& point, int timeout_ms = 0);

 private:
  int fd_ = -1;
  int screen_width_ = 800;
  int screen_height_ = 480;
  int raw_x_ = 0;
  int raw_y_ = 0;
  bool raw_down_ = false;
  bool last_down_ = false;
  int last_x_ = -1;
  int last_y_ = -1;
  int min_x_ = 0;
  int max_x_ = 799;
  int min_y_ = 0;
  int max_y_ = 479;
  bool have_abs_x_ = false;
  bool have_abs_y_ = false;
};
