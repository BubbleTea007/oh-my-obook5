#include "fbdev.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

FbDev::~FbDev() { close(); }

bool FbDev::open(const char* path) {
  close();
  fd_ = ::open(path, O_RDWR);
  if (fd_ < 0) return false;

  fb_fix_screeninfo fix{};
  fb_var_screeninfo var{};
  if (ioctl(fd_, FBIOGET_FSCREENINFO, &fix) != 0 ||
      ioctl(fd_, FBIOGET_VSCREENINFO, &var) != 0) {
    close();
    return false;
  }

  info_.width = static_cast<int>(var.xres);
  info_.height = static_cast<int>(var.yres);
  info_.bpp = static_cast<int>(var.bits_per_pixel);
  info_.stride = static_cast<int>(fix.line_length);

  mem_size_ = fix.smem_len;
  mem_ = static_cast<uint8_t*>(
      mmap(nullptr, mem_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
  if (mem_ == MAP_FAILED) {
    mem_ = nullptr;
    close();
    return false;
  }
  return true;
}

void FbDev::close() {
  if (mem_) {
    munmap(mem_, mem_size_);
    mem_ = nullptr;
    mem_size_ = 0;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  info_ = {};
}

bool FbDev::present1bpp(const uint8_t* bits, int width, int height,
                        bool one_is_white) {
  if (!mem_ || !bits || width <= 0 || height <= 0) return false;
  if (width > info_.width) width = info_.width;
  if (height > info_.height) height = info_.height;

  const int src_stride = (width + 7) / 8;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bits + y * src_stride;
    uint8_t* dst = mem_ + y * info_.stride;
    for (int x = 0; x < width; ++x) {
      const bool bit =
          (row[x >> 3] & static_cast<uint8_t>(0x80u >> (x & 7))) != 0;
      const bool white = one_is_white ? bit : !bit;
      if (info_.bpp == 16) {
        const uint16_t color = white ? 0xFFFFu : 0x0000u;
        reinterpret_cast<uint16_t*>(dst)[x] = color;
      } else if (info_.bpp == 32) {
        const uint32_t color = white ? 0xFFFFFFFFu : 0x00000000u;
        reinterpret_cast<uint32_t*>(dst)[x] = color;
      } else if (info_.bpp == 8) {
        dst[x] = white ? 0xFFu : 0x00u;
      }
    }
  }
  return true;
}

bool FbDev::panDisplay(int yoffset) {
  if (fd_ < 0) return false;
  fb_var_screeninfo var{};
  if (ioctl(fd_, FBIOGET_VSCREENINFO, &var) != 0) return false;
  var.yoffset = static_cast<uint32_t>(yoffset);
  var.activate = FB_ACTIVATE_NOW;
  return ioctl(fd_, FBIOPAN_DISPLAY, &var) == 0;
}

bool FbDev::clear(uint32_t rgb565) {
  if (!mem_) return false;
  const uint32_t pixels = static_cast<uint32_t>(info_.width * info_.height);
  if (info_.bpp == 16) {
    for (uint32_t i = 0; i < pixels; ++i)
      reinterpret_cast<uint16_t*>(mem_)[i] = static_cast<uint16_t>(rgb565);
    return true;
  }
  if (info_.bpp == 32) {
    const uint32_t v = (rgb565 & 0xF800u) << 8 | (rgb565 & 0x07E0u) << 5 |
                       (rgb565 & 0x001Fu) << 3;
    for (uint32_t i = 0; i < pixels; ++i)
      reinterpret_cast<uint32_t*>(mem_)[i] = v;
    return true;
  }
  if (info_.bpp == 8) {
    memset(mem_, 0xFF, mem_size_);
    return true;
  }
  return false;
}
