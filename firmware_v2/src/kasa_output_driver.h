#pragma once

#include <WiFiClient.h>

#include <vector>

#include "output_driver.h"

class KasaOutputDriver : public OutputDriver {
 public:
  KasaOutputDriver(const OutputConfig &config, const String &role);
  bool begin() override;
  bool setState(bool on) override;
  bool refresh() override;
  DriverStatus status() const override;

 private:
  bool sendCommand(const String &command, String &response);
  static std::vector<uint8_t> encryptPayload(const String &payload);
  static String decryptPayload(const uint8_t *buffer, size_t length);
  void updateDescription();

  OutputConfig config_;
  String role_;
  DriverStatus status_;
  uint32_t last_refresh_ms_ = 0;
};
