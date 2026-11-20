#pragma once

#include <cstddef>
#include <cstdint>

struct FbInfo {
  int width = 0;
  int height = 0;
  int bpp = 0;
  int stride = 0;
};

// Minimal Linux framebuffer backend. It deliberately knows nothing about
// CrossMux; HalDisplay can use it to present a 1-bit framebuffer.
class FbDev {
 public:
  FbDev() = default;
  ~FbDev();

  FbDev(const FbDev&) = delete;
  FbDev& operator=(const FbDev&) = delete;

  bool open(const char* path = "/dev/fb0");
  void close();

  bool isOpen() const { return fd_ >= 0; }
  const FbInfo& info() const { return info_; }

  // `bits` is a packed 1-bit image, MSB first. When one_is_white is true,
  // bit 1 maps to white and bit 0 maps to black.
  bool present1bpp(const uint8_t* bits, int width, int height,
                   bool one_is_white = true);

  // Ask the framebuffer driver to display the mapped buffer. This is the
  // generic Linux path; OBOOK5-specific e-ink timing stays outside this class.
  bool panDisplay(int yoffset = 0);

  bool clear(uint32_t rgb565 = 0xFFFF);

 private:
  int fd_ = -1;
  uint8_t* mem_ = nullptr;
  size_t mem_size_ = 0;
  FbInfo info_{};
};
