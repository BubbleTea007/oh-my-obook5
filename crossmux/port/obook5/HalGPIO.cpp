#include "HalGPIO.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "evdev.h"

HalGPIO gpio;

namespace {
TouchDev touch;
bool touchDown = false;
bool touchPressed = false;
bool touchReleased = false;
bool touchActivity = false;
bool tapPending = false;
bool longPending = false;
bool swipePending = false;
bool suppressed = false;
int touchX = 0;
int touchY = 0;
int startX = 0;
int startY = 0;
int endX = 0;
int endY = 0;
unsigned long downMs = 0;
unsigned long heldMs = 0;
bool moved = false;
constexpr int kTapSlop = 28;
constexpr int kSwipeMin = 60;
constexpr unsigned long kLongMs = 500;
}

void HalGPIO::begin() {
  if (!BoardConfig::hasTouch()) {
    fprintf(stderr, "[obook5] touch disabled by board profile\n");
    return;
  }
  const bool ok = touch.open("/dev/input/event1", EInkDisplay::DISPLAY_WIDTH,
                             EInkDisplay::DISPLAY_HEIGHT);
  fprintf(stderr, "[obook5] touch %s\n", ok ? "ready" : "open failed");
}

void HalGPIO::beginFrame() {
  touchPressed = false;
  touchReleased = false;
  touchActivity = false;
  tapPending = false;
  longPending = false;
  swipePending = false;
}

void HalGPIO::update() {
  if (!touch.isOpen()) return;
  TouchPoint p{};
  if (!touch.poll(p, 0)) return;

  const int w = EInkDisplay::DISPLAY_WIDTH;
  const int h = EInkDisplay::DISPLAY_HEIGHT;
  const int nx = std::clamp(p.x, 0, w - 1);
  const int ny = std::clamp(p.y, 0, h - 1);

  if (p.down && !touchDown) {
    fprintf(stderr, "[obook5] touch down %d,%d\n", nx, ny);
    touchDown = true;
    touchPressed = true;
    touchActivity = true;
    downMs = millis();
    startX = nx;
    startY = ny;
    endX = nx;
    endY = ny;
    moved = false;
    suppressed = false;
  } else if (p.down) {
    endX = nx;
    endY = ny;
    heldMs = millis() - downMs;
    if (std::abs(endX - startX) > kTapSlop ||
        std::abs(endY - startY) > kTapSlop)
      moved = true;
    if (heldMs >= kLongMs && !moved && !suppressed) longPending = true;
    touchActivity = true;
  } else if (!p.down && touchDown) {
    fprintf(stderr, "[obook5] touch up %d,%d\n", nx, ny);
    touchDown = false;
    touchReleased = true;
    touchActivity = true;
    endX = nx;
    endY = ny;
    heldMs = millis() - downMs;
    if (!suppressed) {
      if (!moved && heldMs < kLongMs) tapPending = true;
      if (moved) {
        const int dx = endX - startX;
        const int dy = endY - startY;
        if (std::abs(dx) >= kSwipeMin || std::abs(dy) >= kSwipeMin)
          swipePending = true;
      }
    }
  }
  touchX = nx;
  touchY = ny;
}

bool HalGPIO::isPressed(uint8_t) const { return false; }
bool HalGPIO::wasPressed(uint8_t) const { return false; }
bool HalGPIO::wasAnyPressed() const { return touchPressed; }
bool HalGPIO::wasReleased(uint8_t) const { return false; }
bool HalGPIO::wasAnyReleased() const { return touchReleased; }
unsigned long HalGPIO::getHeldTime() const { return heldMs; }
unsigned long HalGPIO::getPowerButtonHeldTime() const { return 0; }
bool HalGPIO::rawInputActive() { return touchDown; }

bool HalGPIO::wasCapacitivePagePressed() const { return false; }
bool HalGPIO::isCapacitivePagePressed(uint8_t) const { return false; }
bool HalGPIO::hasTouch() const { return BoardConfig::hasTouch(); }
bool HalGPIO::hasHomeKey() const { return false; }
bool HalGPIO::hasToggleSwitch() const { return false; }
bool HalGPIO::isToggleSwitchOn() const { return false; }
bool HalGPIO::wasHomeKeyPressed() const { return false; }
bool HalGPIO::wasHomeKeyTapped() const { return false; }
bool HalGPIO::wasHomeKeyLongPressed() const { return false; }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const {
  if (!tapPending) return false;
  tapPending = false;
  nx = static_cast<float>(endX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(endY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

bool HalGPIO::wasTouchDown(float& nx, float& ny) const {
  if (!touchPressed) return false;
  nx = static_cast<float>(touchX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(touchY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

bool HalGPIO::wasTouchReleased() const { return touchReleased; }

bool HalGPIO::wasTouchReleasedAt(float& nx, float& ny) const {
  if (!touchReleased) return false;
  nx = static_cast<float>(endX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(endY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny,
                                  unsigned long& held) const {
  if (!touchDown || moved || suppressed) return false;
  nx = static_cast<float>(startX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(startY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  held = millis() - downMs;
  return true;
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const {
  if (!touchDown) return false;
  nx = static_cast<float>(touchX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(touchY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const {
  if (!longPending) return false;
  longPending = false;
  nx = static_cast<float>(touchX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(touchY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

void HalGPIO::suppressTouchContact() { suppressed = true; }
unsigned long HalGPIO::lastTouchHeldMs() const { return heldMs; }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd,
                       float& nyEnd) const {
  if (!swipePending) return false;
  swipePending = false;
  nxStart = static_cast<float>(startX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  nyStart = static_cast<float>(startY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  nxEnd = static_cast<float>(endX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  nyEnd = static_cast<float>(endY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

bool HalGPIO::wasTouchActivity() const { return touchActivity; }
void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(bool) {}
bool HalGPIO::restoreTouchAfterDisplayReset() { return true; }
bool HalGPIO::consumeSimulatorSleepRequest() { return false; }
void HalGPIO::startDeepSleep() {}
bool HalGPIO::verifyPowerButtonWakeup() { return false; }
bool HalGPIO::isUsbConnected() const { return false; }
bool HalGPIO::wasUsbStateChanged() const { return false; }
bool HalGPIO::isXteinkDevice() const { return true; }
bool HalGPIO::hasEdgeSideButtons() const { return false; }
HalGPIO::WakeupReason HalGPIO::getWakeupReason() const { return WakeupReason::Other; }
