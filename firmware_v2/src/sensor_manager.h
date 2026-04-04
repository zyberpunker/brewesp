#pragma once

#include <DallasTemperature.h>
#include <OneWire.h>

#include <memory>

#include "app_types.h"

class SensorManager {
 public:
  SensorManager();
  void begin(int gpio);
  void tick(const SystemConfig &system_config, const FermentationConfig &fermentation_config, uint32_t now_ms);
  const SensorSnapshot &snapshot() const;

 private:
  void resetSnapshot(uint32_t now_ms, uint32_t stale_limit_ms);
  void updateProbe(ProbeReading &probe, const String &expected_rom, const DeviceAddress address, bool matched,
                   float offset_c, uint32_t now_ms, uint32_t stale_limit_ms);
  int findByRom(const String &rom, DeviceAddress *addresses, size_t count, const bool *used) const;
  static String addressToString(const DeviceAddress address);

  std::unique_ptr<OneWire> onewire_;
  std::unique_ptr<DallasTemperature> sensors_;
  int gpio_ = -1;
  uint32_t last_poll_ms_ = 0;
  SensorSnapshot snapshot_;
};
