#include "HalDisplay.h"

#include <BoardConfig.h>
#include <EInkDisplay.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <csignal>
#include <cstdio>
#include <unistd.h>

#include "fbdev.h"

namespace {
FbDev fb;
std::array<uint8_t, HalDisplay::BUFFER_SIZE> frameBuffer{};
bool inverted = false;
bool fbReady = false;
}

volatile sig_atomic_t obook5_quit_requested = 0;

HalDisplay display;

HalDisplay::HalDisplay() = default;
HalDisplay::~HalDisplay() { fb.close(); }

HalDisplay::Controller HalDisplay::getController() const {
  return BoardConfig::ACTIVE.displayController;
}

HalDisplay::GrayscaleCapabilities HalDisplay::grayscaleCapabilities(
    GrayscaleMode) const {
  return {};
}

const char* HalDisplay::controllerName() const { return "spi_fb"; }

void HalDisplay::begin() {
  fbReady = fb.open("/dev/fb0");
  if (!fbReady) {
    fprintf(stderr, "[obook5] /dev/fb0 open failed\n");
    return;
  }
  fprintf(stderr, "[obook5] fb %dx%d %dbpp stride=%d\n", fb.info().width,
          fb.info().height, fb.info().bpp, fb.info().stride);
  frameBuffer.fill(0xFF);
  fb.present1bpp(frameBuffer.data(), DISPLAY_WIDTH, DISPLAY_HEIGHT, true);
  fb.panDisplay();
}

void HalDisplay::begin(bool /*seamless*/) { begin(); }

void HalDisplay::clearScreen(uint8_t color) const {
  frameBuffer.fill(color);
}

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h, bool /*fromProgmem*/) const {
  if (!imageData) return;
  const int srcStride = (w + 7) / 8;
  for (int row = 0; row < h; ++row) {
    const int dy = y + row;
    if (dy < 0 || dy >= DISPLAY_HEIGHT) continue;
    for (int col = 0; col < w; ++col) {
      const int dx = x + col;
      if (dx < 0 || dx >= DISPLAY_WIDTH) continue;
      const bool bit = (imageData[row * srcStride + (col >> 3)] &
                        (0x80u >> (col & 7))) != 0;
      const uint8_t mask = static_cast<uint8_t>(0x80u >> (dx & 7));
      if (bit) frameBuffer[dy * DISPLAY_WIDTH_BYTES + (dx >> 3)] |= mask;
      else frameBuffer[dy * DISPLAY_WIDTH_BYTES + (dx >> 3)] &= ~mask;
    }
  }
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x,
                                      uint16_t y, uint16_t w, uint16_t h,
                                      bool /*fromProgmem*/) const {
  if (!imageData) return;
  const int srcStride = (w + 7) / 8;
  for (int row = 0; row < h; ++row) {
    const int dy = y + row;
    if (dy < 0 || dy >= DISPLAY_HEIGHT) continue;
    for (int col = 0; col < w; ++col) {
      const int dx = x + col;
      if (dx < 0 || dx >= DISPLAY_WIDTH) continue;
      const bool bit = (imageData[row * srcStride + (col >> 3)] &
                        (0x80u >> (col & 7))) != 0;
      if (bit) continue;  // 0 = ink, 1 = transparent
      frameBuffer[dy * DISPLAY_WIDTH_BYTES + (dx >> 3)] &=
          static_cast<uint8_t>(~(0x80u >> (dx & 7)));
    }
  }
}

void HalDisplay::setInverted(bool value) { inverted = value; }
bool HalDisplay::toggleInverted() { inverted = !inverted; return inverted; }
bool HalDisplay::isInverted() const { return inverted; }

void HalDisplay::displayBuffer(RefreshMode /*mode*/, bool /*turnOffScreen*/,
                               DisplayRefreshContext /*context*/) {
  if (!fbReady) return;
  fb.present1bpp(frameBuffer.data(), DISPLAY_WIDTH, DISPLAY_HEIGHT, !inverted);
  fb.panDisplay();
  // The qingyue426 driver queues the SPI update and returns; the workshop
  // platform layer waits 90 ms before handing the panel back to the caller.
  usleep(90000);
}

void HalDisplay::displayBufferAsync(RefreshMode mode,
                                    DisplayRefreshContext context) {
  displayBuffer(mode, false, context);
}

void HalDisplay::waitRefreshComplete() {}
bool HalDisplay::supportsAsyncRefresh() const { return false; }
void HalDisplay::displayWindow(int, int, int, int) { displayBuffer(); }
void HalDisplay::refreshDisplay(RefreshMode mode, bool turnOffScreen) {
  displayBuffer(mode, turnOffScreen);
}
void HalDisplay::presentIfNeeded() {}
bool HalDisplay::shouldQuit() const { return obook5_quit_requested != 0; }
void HalDisplay::deepSleep() { displayBuffer(); }

uint8_t* HalDisplay::getFrameBuffer() const {
  return const_cast<uint8_t*>(frameBuffer.data());
}

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) {
  if (sizeOut) *sizeOut = BUFFER_SIZE;
  return frameBuffer.data();
}
void HalDisplay::returnFrameBufferStorage() {}

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }
uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }
uint16_t HalDisplay::getDisplayWidthBytes() const {
  return DISPLAY_WIDTH_BYTES;
}
uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }
uint8_t HalDisplay::getGrayscaleLevels() const { return 2; }

uint8_t* HalDisplay::beginGrayscale16() { return nullptr; }
bool HalDisplay::commitGrayscale16() { return false; }
void HalDisplay::cancelGrayscale16() {}
void HalDisplay::copyGrayscaleBuffers(const uint8_t*, const uint8_t*) {}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t*) {}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t*) {}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t*) {}
void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOff,
                                      DisplayRefreshContext context) {
  displayBuffer(fallback, turnOff, context);
}
bool HalDisplay::displayGrayscaleBase(GrayscaleMode, RefreshMode fallback,
                                      bool turnOff) {
  displayBuffer(fallback, turnOff);
  return true;
}
void HalDisplay::preconditionGrayscale() {}
void HalDisplay::preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {
}
void HalDisplay::displayGrayBuffer(bool turnOff, const unsigned char*,
                                   bool) {
  displayBuffer(FAST_REFRESH, turnOff);
}
void HalDisplay::writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t,
                                          uint16_t) {}
bool HalDisplay::supportsStripGrayscale() const { return false; }
bool HalDisplay::supportsAsyncGrayscaleBase() const { return false; }
bool HalDisplay::combinesGrayscaleBase() const { return false; }
bool HalDisplay::supportsTextOnlyCombinedBase() const { return false; }
bool HalDisplay::displayCommitted() const { return true; }
void HalDisplay::cancelGrayscale() {}
