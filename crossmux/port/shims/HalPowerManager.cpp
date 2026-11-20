#include "HalPowerManager.h"

#include <unistd.h>

#include "HalGPIO.h"
#include "SimulatorLifecycle.h"

HalPowerManager powerManager;

void HalPowerManager::begin() {}
void HalPowerManager::startDeepSleep(HalGPIO &gpio,
                                     bool wakeOnHeldButton) const {
  (void)gpio;
  (void)wakeOnHeldButton;
  // There is no ESP deep-sleep on OBOOK5.  CrossMux's sleep path has already
  // painted the retained frame and saved state; exit back to the workshop
  // launcher instead of spinning in the simulator's no-op sleep loop.
  _exit(0);
}
#if FREEINK_DEVICE_PICCO
bool HalPowerManager::holdWhileUsbPowered(
    HalGPIO & /*gpio*/, void (* /*onStatus*/)(bool, uint16_t)) const {
  // No charger model: keep the existing host sleep/wake path available.
  return false;
}
#endif
void HalPowerManager::setPowerSaving(bool enable) {}
uint16_t HalPowerManager::getBatteryPercentage() const { return 100; }

HalPowerManager::Lock::Lock() {}
HalPowerManager::Lock::~Lock() {}
