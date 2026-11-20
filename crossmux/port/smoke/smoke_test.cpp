#include "evdev.h"
#include "fbdev.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
void draw_pattern(uint8_t* bits, int width, int height, int px, int py) {
  const int stride = (width + 7) / 8;
  std::memset(bits, 0xFF, static_cast<size_t>(stride) * height);
  auto plot = [&](int x, int y, bool black) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
    if (black) bits[y * stride + (x >> 3)] &= static_cast<uint8_t>(~mask);
    else bits[y * stride + (x >> 3)] |= mask;
  };
  // Border.
  for (int x = 0; x < width; ++x) {
    plot(x, 4, true);
    plot(x, height - 5, true);
  }
  for (int y = 0; y < height; ++y) {
    plot(4, y, true);
    plot(width - 5, y, true);
  }
  // Checkerboard band.
  for (int y = 40; y < 120; ++y) {
    for (int x = 40; x < width - 40; ++x) {
      if (((x / 16) + (y / 16)) & 1) plot(x, y, true);
    }
  }
  // Touch marker.
  for (int y = -12; y <= 12; ++y) {
    for (int x = -12; x <= 12; ++x) {
      const int d = x * x + y * y;
      if (d <= 144) plot(px + x, py + y, true);
    }
  }
}
}  // namespace

int main() {
  FbDev fb;
  if (!fb.open()) {
    std::perror("open /dev/fb0");
    return 1;
  }
  const FbInfo info = fb.info();
  std::printf("fb: %dx%d %dbpp stride=%d\n", info.width, info.height,
              info.bpp, info.stride);

  TouchDev touch;
  const bool have_touch = touch.open("/dev/input/event1", info.width, info.height);
  std::printf("touch: %s\n", have_touch ? "ready" : "unavailable");

  std::vector<uint8_t> bits(static_cast<size_t>((info.width + 7) / 8) *
                            info.height);
  int px = info.width / 2;
  int py = info.height / 2;
  draw_pattern(bits.data(), info.width, info.height, px, py);
  fb.present1bpp(bits.data(), info.width, info.height, true);
  fb.panDisplay();

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    TouchPoint point{};
    if (have_touch && touch.poll(point, 50)) {
      std::printf("touch x=%d y=%d down=%d\n", point.x, point.y,
                  point.down ? 1 : 0);
      if (point.down) {
        px = point.x;
        py = point.y;
        draw_pattern(bits.data(), info.width, info.height, px, py);
        fb.present1bpp(bits.data(), info.width, info.height, true);
        fb.panDisplay();
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  std::printf("smoke test done\n");
  return 0;
}
