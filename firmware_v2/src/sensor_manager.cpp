#include "sensor_manager.h"

#include <cstring>

namespace {
constexpr size_t kMaxDevices = 8;
}

SensorManager::SensorManager() = default;

void SensorManager::begin(int gpio) {
  if (gpio_ == gpio && sensors_) {
    return;
  }
  gpio_ = gpio;
  onewire_.reset(new OneWire(gpio_));
  sensors_.reset(new DallasTemperature(onewire_.get()));
  sensors_->begin();
  sensors_->setWaitForConversion(true);
  last_poll_ms_ = 0;
}

const SensorSnapshot &SensorManager::snapshot() const {
  return snapshot_;
}

void SensorManager::tick(const SystemConfig &system_config, const FermentationConfig &fermentation_config, uint32_t now_ms) {
  if (!sensors_) {
    begin(system_config.sensors.onewire_gpio);
  }

  const uint32_t poll_interval_ms = system_config.sensors.poll_interval_s * 1000UL;
  const uint32_t stale_limit_ms = (fermentation_config.valid ? fermentation_config.alarms.sensor_stale_s : 30) * 1000UL;
  if ((now_ms - last_poll_ms_) < poll_interval_ms) {
    resetSnapshot(now_ms, stale_limit_ms);
    return;
  }

  last_poll_ms_ = now_ms;
  sensors_->requestTemperatures();

  DeviceAddress addresses[kMaxDevices];
  DeviceAddress empty_address = {};
  bool used[kMaxDevices] = {};
  const size_t device_count =
      static_cast<size_t>(min(static_cast<size_t>(sensors_->getDeviceCount()), static_cast<size_t>(kMaxDevices)));
  for (size_t index = 0; index < device_count; ++index) {
    if (!sensors_->getAddress(addresses[index], index)) {
      memset(addresses[index], 0, sizeof(DeviceAddress));
    }
  }

  int beer_index = -1;
  if (!system_config.sensors.beer_probe_rom.isEmpty()) {
    beer_index = findByRom(system_config.sensors.beer_probe_rom, addresses, device_count, used);
  } else if (device_count > 0) {
    beer_index = 0;
  }
  if (beer_index >= 0) {
    used[beer_index] = true;
  }

  int chamber_index = -1;
  if (!system_config.sensors.chamber_probe_rom.isEmpty()) {
    chamber_index = findByRom(system_config.sensors.chamber_probe_rom, addresses, device_count, used);
  } else {
    for (size_t index = 0; index < device_count; ++index) {
      if (!used[index]) {
        chamber_index = static_cast<int>(index);
        used[index] = true;
        break;
      }
    }
  }

  updateProbe(snapshot_.beer, system_config.sensors.beer_probe_rom,
              beer_index >= 0 ? addresses[beer_index] : empty_address, beer_index >= 0,
              fermentation_config.valid ? fermentation_config.sensors.primary_offset_c : 0.0f, now_ms, stale_limit_ms);
  updateProbe(snapshot_.chamber, system_config.sensors.chamber_probe_rom,
              chamber_index >= 0 ? addresses[chamber_index] : empty_address, chamber_index >= 0,
              fermentation_config.valid ? fermentation_config.sensors.secondary_offset_c : 0.0f, now_ms, stale_limit_ms);
}

void SensorManager::resetSnapshot(uint32_t now_ms, uint32_t stale_limit_ms) {
  auto updateStale = [&](ProbeReading &probe) {
    probe.stale = !probe.valid || probe.last_seen_ms == 0 || (now_ms - probe.last_seen_ms) > stale_limit_ms;
  };
  updateStale(snapshot_.beer);
  updateStale(snapshot_.chamber);
}

void SensorManager::updateProbe(ProbeReading &probe, const String &expected_rom, const DeviceAddress address, bool matched,
                                float offset_c, uint32_t now_ms, uint32_t stale_limit_ms) {
  if (!matched) {
    probe.present = false;
    probe.valid = false;
    probe.raw_c = NAN;
    probe.adjusted_c = NAN;
    if (!expected_rom.isEmpty()) {
      probe.rom = expected_rom;
    }
    probe.stale = probe.last_seen_ms == 0 || (now_ms - probe.last_seen_ms) > stale_limit_ms;
    return;
  }

  probe.present = true;
  probe.rom = addressToString(address);
  const float temperature = sensors_->getTempC(address);
  probe.raw_c = temperature;
  probe.valid = temperature != DEVICE_DISCONNECTED_C && !isnan(temperature);
  if (probe.valid) {
    probe.adjusted_c = temperature + offset_c;
    probe.last_seen_ms = now_ms;
  } else {
    probe.adjusted_c = NAN;
  }
  probe.stale = !probe.valid || probe.last_seen_ms == 0 || (now_ms - probe.last_seen_ms) > stale_limit_ms;
}

int SensorManager::findByRom(const String &rom, DeviceAddress *addresses, size_t count, const bool *used) const {
  for (size_t index = 0; index < count; ++index) {
    if (used[index]) {
      continue;
    }
    if (addressToString(addresses[index]).equalsIgnoreCase(rom)) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

String SensorManager::addressToString(const DeviceAddress address) {
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "%02x%02x%02x%02x%02x%02x%02x%02x", address[0], address[1], address[2], address[3],
           address[4], address[5], address[6], address[7]);
  return String(buffer);
}
