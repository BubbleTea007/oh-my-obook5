#include "evdev.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

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
    have_abs_x_ = max_x_ > min_x_;
    have_abs_y_ = max_y_ > min_y_;
  }
  return true;
}

void TouchDev::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  raw_down_ = false;
  released_in_report_ = false;
  last_down_ = false;
  last_x_ = -1;
  last_y_ = -1;
}

void TouchDev::handleEvent(const input_event& ev) {
  if (ev.type == EV_ABS) {
    if (ev.code == ABS_MT_TRACKING_ID) {
      if (ev.value >= 0) {
        raw_down_ = true;
      } else {
        // Do not let the position-reset events that follow the lift
        // overwrite the last valid contact position.
        raw_down_ = false;
        released_in_report_ = true;
      }
      return;
    }
    if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
      if (!released_in_report_) raw_x_ = ev.value;
      return;
    }
    if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
      if (!released_in_report_) raw_y_ = ev.value;
      return;
    }
  } else if (ev.type == EV_KEY &&
             (ev.code == BTN_TOUCH || ev.code == BTN_TOOL_FINGER)) {
    if (ev.value != 0) {
      raw_down_ = true;
    } else {
      raw_down_ = false;
      released_in_report_ = true;
    }
  }
}

bool TouchDev::poll(TouchPoint& point, int timeout_ms) {
  if (fd_ < 0) return false;

  if (timeout_ms > 0) {
    pollfd pfd{fd_, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready <= 0 || !(pfd.revents & POLLIN)) return false;
  }

  // Consume exactly one SYN_REPORT worth of events.  Keeping the remaining
  // reports in the kernel queue prevents a fast press/release from being
  // collapsed into a single sample.
  bool have_report = false;
  input_event ev{};
  while (true) {
    const ssize_t got = ::read(fd_, &ev, sizeof(ev));
    if (got != static_cast<ssize_t>(sizeof(ev))) {
      if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (got <= 0) break;
    }
    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
      have_report = true;
      break;
    }
    handleEvent(ev);
  }
  if (!have_report) return false;

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

  const bool changed = raw_down_ != last_down_ || cx != last_x_ || cy != last_y_;
  last_down_ = raw_down_;
  last_x_ = cx;
  last_y_ = cy;
  released_in_report_ = false;

  if (!changed) return false;

  point.x = cx;
  point.y = cy;
  point.down = raw_down_;
  return true;
}
