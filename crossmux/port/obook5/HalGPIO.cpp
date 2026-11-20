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

// Per-frame edge flags.  CrossMux may ask several times in one frame
// (wasScreenTapped, wasSwipe, list hit-testing), so these are pure queries and
// are cleared only by beginFrame(), matching the simulator HAL.
bool touchDown = false;
bool pressedThisFrame = false;
bool releasedThisFrame = false;
bool activityThisFrame = false;
bool longPressThisFrame = false;

bool moved = false;
bool suppressed = false;
bool longPressFired = false;
int touchX = 0;
int touchY = 0;
int startX = 0;
int startY = 0;
int endX = 0;
int endY = 0;
unsigned long downMs = 0;
unsigned long heldMs = 0;

constexpr int kTapSlop = 28;
constexpr int kSwipeMin = 60;
constexpr unsigned long kLongMs = 500;
constexpr unsigned long kSwipeMaxMs = 1500;
}  // namespace

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
  pressedThisFrame = false;
  releasedThisFrame = false;
  activityThisFrame = false;
  longPressThisFrame = false;
}

void HalGPIO::update() {
  if (!touch.isOpen()) return;

  const int w = EInkDisplay::DISPLAY_WIDTH;
  const int h = EInkDisplay::DISPLAY_HEIGHT;
  TouchPoint p{};

  // Drain every queued report.  An e-ink refresh can block the loop for close
  // to a second, so a full press/move/release sequence may already be waiting
  // in the kernel buffer.
  while (touch.poll(p, 0)) {
    // The panel is mounted 180 degrees from the X4 Pro profile; rotate the
    // touch contact into CrossMux's panel coordinate system before its
    // Portrait tapToLogical() mapping runs.
    const int nx = w - 1 - std::clamp(p.x, 0, w - 1);
    const int ny = h - 1 - std::clamp(p.y, 0, h - 1);

    if (p.down && !touchDown) {
      fprintf(stderr, "[obook5] touch down %d,%d\n", nx, ny);
      touchDown = true;
      pressedThisFrame = true;
      activityThisFrame = true;
      downMs = millis();
      startX = endX = nx;
      startY = endY = ny;
      moved = false;
      suppressed = false;
      longPressFired = false;
    } else if (p.down) {
      endX = nx;
      endY = ny;
      heldMs = millis() - downMs;
      if (std::abs(endX - startX) > kTapSlop ||
          std::abs(endY - startY) > kTapSlop)
        moved = true;
      if (!moved && !suppressed && !longPressFired && heldMs >= kLongMs) {
        longPressThisFrame = true;
        longPressFired = true;
      }
      activityThisFrame = true;
    } else if (!p.down && touchDown) {
      fprintf(stderr, "[obook5] touch up %d,%d\n", nx, ny);
      touchDown = false;
      releasedThisFrame = true;
      activityThisFrame = true;
      endX = nx;
      endY = ny;
      heldMs = millis() - downMs;
    }
    touchX = nx;
    touchY = ny;
  }
}

bool HalGPIO::isPressed(uint8_t) const { return false; }
bool HalGPIO::wasPressed(uint8_t) const { return false; }
bool HalGPIO::wasAnyPressed() const { return pressedThisFrame; }
bool HalGPIO::wasReleased(uint8_t) const { return false; }
bool HalGPIO::wasAnyReleased() const { return releasedThisFrame; }
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
  if (!releasedThisFrame || moved || suppressed) return false;
  nx = static_cast<float>(endX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(endY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

bool HalGPIO::wasTouchDown(float& nx, float& ny) const {
  if (!pressedThisFrame) return false;
  nx = static_cast<float>(touchX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(touchY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

bool HalGPIO::wasTouchReleased() const { return releasedThisFrame; }

bool HalGPIO::wasTouchReleasedAt(float& nx, float& ny) const {
  if (!releasedThisFrame) return false;
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
  if (!touchDown || suppressed) return false;
  nx = static_cast<float>(touchX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(touchY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const {
  if (!longPressThisFrame || suppressed) return false;
  nx = static_cast<float>(touchX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  ny = static_cast<float>(touchY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

void HalGPIO::suppressTouchContact() {
  if (touchDown || releasedThisFrame) suppressed = true;
}
unsigned long HalGPIO::lastTouchHeldMs() const { return heldMs; }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd,
                       float& nyEnd) const {
  if (!releasedThisFrame || suppressed || heldMs > kSwipeMaxMs) return false;
  const int dx = endX - startX;
  const int dy = endY - startY;
  if (std::abs(dx) < kSwipeMin && std::abs(dy) < kSwipeMin) return false;
  nxStart = static_cast<float>(startX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  nyStart = static_cast<float>(startY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  nxEnd = static_cast<float>(endX) / static_cast<float>(EInkDisplay::DISPLAY_WIDTH - 1);
  nyEnd = static_cast<float>(endY) / static_cast<float>(EInkDisplay::DISPLAY_HEIGHT - 1);
  return true;
}

bool HalGPIO::wasTouchActivity() const { return activityThisFrame; }
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
