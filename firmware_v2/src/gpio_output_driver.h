#pragma once

#include "output_driver.h"

class GpioOutputDriver : public OutputDriver {
 public:
  GpioOutputDriver(const OutputConfig &config, const String &role);
  bool begin() override;
  bool setState(bool on) override;
  bool refresh() override;
  DriverStatus status() const override;

 private:
  bool writePin(bool on);
  OutputConfig config_;
  String role_;
  DriverStatus status_;
};
