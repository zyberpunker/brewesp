#pragma once

#include <HTTPClient.h>

#include "output_driver.h"

class ShellyOutputDriver : public OutputDriver {
 public:
  ShellyOutputDriver(const OutputConfig &config, const String &role);
  bool begin() override;
  bool setState(bool on) override;
  bool refresh() override;
  DriverStatus status() const override;

 private:
  bool performGet(const String &path, String &response);
  void updateDescription();

  OutputConfig config_;
  String role_;
  DriverStatus status_;
};
