#pragma once

#include "app_types.h"
#include "output_driver.h"

struct ControllerState {
  bool heating_command = false;
  bool cooling_command = false;
  bool automatic_control_active = false;
  String controller_state = "idle";
  String controller_reason = "no active config";
  String fault;
};

class ThermostatController {
 public:
  const ControllerState &evaluate(const FermentationConfig &config, const SensorSnapshot &sensors, uint32_t now_ms);
  void apply(OutputDriver &heating, OutputDriver &cooling, uint32_t now_ms);
  const ControllerState &state() const;

 private:
  void forceOff(OutputDriver &heating, OutputDriver &cooling, uint32_t now_ms);

  ControllerState state_;
  bool heating_on_ = false;
  bool cooling_on_ = false;
  uint32_t heating_last_off_ms_ = 0;
  uint32_t cooling_last_off_ms_ = 0;
};
