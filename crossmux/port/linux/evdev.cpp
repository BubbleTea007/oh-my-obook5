#include "evdev.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>

namespace {
int read_abs_range(int fd, int code, int fallback_min, int fallback_max,
                   int& min_value) {
  input_absinfo info{};
  if (ioctl(fd, EVIOCGABS(code), &info) == 0 &&
      info.maximum > info.minimum) {
    min_value = info.minimum;
    return info.maximum;
  }
  min_value = fallback_min;
  return fallback_max;
}
}  // namespace

TouchDev::~TouchDev() { close(); }

bool TouchDev::open(const char* path, int screen_width, int screen_height) {
  close();
  screen_width_ = screen_width;
  screen_height_ = screen_height;
  fd_ = ::open(path, O_RDONLY | O_NONBLOCK);
  if (fd_ < 0) return false;

  max_x_ = read_abs_range(fd_, ABS_X, 0, screen_width_ - 1, min_x_);
  max_y_ = read_abs_range(fd_, ABS_Y, 0, screen_height_ - 1, min_y_);
  have_abs_x_ = max_x_ > min_x_;
  have_abs_y_ = max_y_ > min_y_;
  if (!have_abs_x_ || !have_abs_y_) {
    max_x_ = read_abs_range(fd_, ABS_MT_POSITION_X, 0, screen_width_ - 1, min_x_);
    max_y_ = read_abs_range(fd_, ABS_MT_POSITION_Y, 0, screen_height_ - 1, min_y_);
  }
  return true;
}

void TouchDev::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  raw_down_ = false;
  last_down_ = false;
  last_x_ = -1;
  last_y_ = -1;
}

bool TouchDev::poll(TouchPoint& point, int timeout_ms) {
  if (fd_ < 0) return false;

  pollfd pfd{fd_, POLLIN, 0};
  const int ready = ::poll(&pfd, 1, timeout_ms);
  if (ready <= 0 || !(pfd.revents & POLLIN)) return false;

  input_event events[32];
  const ssize_t got = ::read(fd_, events, sizeof(events));
  if (got <= 0) return false;

  bool changed = false;
  const int count = static_cast<int>(got / sizeof(events[0]));
  for (int i = 0; i < count; ++i) {
    const input_event& ev = events[i];
    if (ev.type == EV_ABS) {
      if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
        raw_x_ = ev.value;
        have_abs_x_ = true;
      } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
        raw_y_ = ev.value;
        have_abs_y_ = true;
      }
    } else if (ev.type == EV_KEY && (ev.code == BTN_TOUCH ||
                                     ev.code == BTN_TOOL_FINGER)) {
      raw_down_ = ev.value != 0;
    } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
      changed = true;
    }
  }
  if (!changed) return false;

  const int x = have_abs_x_
                    ? (raw_x_ - min_x_) * (screen_width_ - 1) /
                          std::max(1, max_x_ - min_x_)
                    : 0;
  const int y = have_abs_y_
                    ? (raw_y_ - min_y_) * (screen_height_ - 1) /
                          std::max(1, max_y_ - min_y_)
                    : 0;
  const int cx = std::clamp(x, 0, screen_width_ - 1);
  const int cy = std::clamp(y, 0, screen_height_ - 1);

  last_down_ = raw_down_;
  last_x_ = cx;
  last_y_ = cy;
  point.x = cx;
  point.y = cy;
  point.down = raw_down_;
  return true;
}
