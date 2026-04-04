#include "gpio_output_driver.h"

GpioOutputDriver::GpioOutputDriver(const OutputConfig &config, const String &role) : config_(config), role_(role) {}

bool GpioOutputDriver::begin() {
  pinMode(config_.pin, OUTPUT);
  status_.known = true;
  status_.on = false;
  writePin(false);
  status_.description = "gpio " + String(config_.pin) + " off";
  return true;
}

bool GpioOutputDriver::setState(bool on) {
  status_.known = true;
  status_.on = on;
  writePin(on);
  status_.description = "gpio " + String(config_.pin) + (on ? " on" : " off");
  return true;
}

bool GpioOutputDriver::refresh() {
  return true;
}

DriverStatus GpioOutputDriver::status() const {
  return status_;
}

bool GpioOutputDriver::writePin(bool on) {
  const bool active_high = config_.active_level != "low";
  digitalWrite(config_.pin, (on == active_high) ? HIGH : LOW);
  return true;
}
