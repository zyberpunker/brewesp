#include "thermostat_controller.h"

namespace {
const ProbeReading *selectControlProbe(const FermentationConfig &config, const SensorSnapshot &sensors) {
  if (config.sensors.control_sensor == "secondary" && config.sensors.secondary_enabled) {
    return &sensors.chamber;
  }
  return &sensors.beer;
}
}  // namespace

const ControllerState &ThermostatController::evaluate(const FermentationConfig &config, const SensorSnapshot &sensors,
                                                      uint32_t now_ms) {
  (void)now_ms;
  state_ = ControllerState();

  if (!config.valid) {
    state_.controller_reason = "no active config";
    return state_;
  }
  if (config.mode != "thermostat") {
    state_.fault = "unsupported mode";
    state_.controller_reason = "profile mode not implemented";
    return state_;
  }

  const ProbeReading *control_probe = selectControlProbe(config, sensors);
  const bool using_secondary = control_probe == &sensors.chamber;
  const String control_name = using_secondary ? "chamber" : "beer";
  if (!control_probe->present) {
    state_.fault = control_name + " sensor missing";
    state_.controller_reason = "control sensor missing";
    state_.controller_state = "fault";
    return state_;
  }
  if (!control_probe->valid) {
    state_.fault = control_name + " sensor invalid";
    state_.controller_reason = "control sensor invalid";
    state_.controller_state = "fault";
    return state_;
  }
  if (control_probe->stale) {
    state_.fault = control_name + " sensor stale";
    state_.controller_reason = "control sensor stale";
    state_.controller_state = "fault";
    return state_;
  }

  const float setpoint = config.thermostat.setpoint_c;
  const float hysteresis = config.thermostat.hysteresis_c;
  const float control_temp = control_probe->adjusted_c;
  bool heating_demand = control_temp < (setpoint - hysteresis);
  bool cooling_demand = control_temp > (setpoint + hysteresis);
  bool heating_blocked = false;
  bool cooling_blocked = false;

  if (config.sensors.control_sensor == "primary" && config.sensors.secondary_enabled && sensors.chamber.present &&
      sensors.chamber.valid && !sensors.chamber.stale) {
    const float chamber_limit = config.sensors.secondary_limit_hysteresis_c;
    if (cooling_demand && sensors.chamber.adjusted_c <= (setpoint - chamber_limit)) {
      cooling_blocked = true;
    }
    if (heating_demand && sensors.chamber.adjusted_c >= (setpoint + chamber_limit)) {
      heating_blocked = true;
    }
  }

  state_.automatic_control_active = true;
  state_.controller_state = "idle";
  state_.controller_reason = "within hysteresis";
  if (heating_demand && !heating_blocked) {
    if ((now_ms - heating_last_off_ms_) >= config.thermostat.heating_delay_s * 1000UL) {
      state_.heating_command = true;
      state_.controller_state = "heating";
      state_.controller_reason = "below setpoint band";
    } else {
      state_.controller_reason = "heating delay active";
    }
  } else if (heating_blocked) {
    state_.controller_reason = "chamber high limit active";
  }

  if (cooling_demand && !cooling_blocked) {
    if ((now_ms - cooling_last_off_ms_) >= config.thermostat.cooling_delay_s * 1000UL) {
      state_.cooling_command = true;
      state_.controller_state = "cooling";
      state_.controller_reason = "above setpoint band";
    } else if (!state_.heating_command) {
      state_.controller_reason = "cooling delay active";
    }
  } else if (cooling_blocked && !state_.heating_command) {
    state_.controller_reason = "chamber low limit active";
  }

  if (state_.heating_command && state_.cooling_command) {
    state_.cooling_command = false;
    state_.controller_state = "heating";
    state_.controller_reason = "mutual exclusion enforced";
  }
  return state_;
}

void ThermostatController::apply(OutputDriver &heating, OutputDriver &cooling, uint32_t now_ms) {
  if (!state_.fault.isEmpty()) {
    forceOff(heating, cooling, now_ms);
    return;
  }

  if (state_.heating_command != heating_on_) {
    if (heating.setState(state_.heating_command)) {
      heating_on_ = state_.heating_command;
      if (!heating_on_) {
        heating_last_off_ms_ = now_ms;
      }
    }
  }
  if (state_.cooling_command != cooling_on_) {
    if (cooling.setState(state_.cooling_command)) {
      cooling_on_ = state_.cooling_command;
      if (!cooling_on_) {
        cooling_last_off_ms_ = now_ms;
      }
    }
  }
  if (state_.heating_command && cooling_on_) {
    if (cooling.setState(false)) {
      cooling_on_ = false;
      cooling_last_off_ms_ = now_ms;
    }
  }
  if (state_.cooling_command && heating_on_) {
    if (heating.setState(false)) {
      heating_on_ = false;
      heating_last_off_ms_ = now_ms;
    }
  }
}

const ControllerState &ThermostatController::state() const {
  return state_;
}

void ThermostatController::forceOff(OutputDriver &heating, OutputDriver &cooling, uint32_t now_ms) {
  state_.heating_command = false;
  state_.cooling_command = false;
  state_.automatic_control_active = false;

  if (heating_on_ && heating.setState(false)) {
    heating_on_ = false;
    heating_last_off_ms_ = now_ms;
  }
  if (cooling_on_ && cooling.setState(false)) {
    cooling_on_ = false;
    cooling_last_off_ms_ = now_ms;
  }
}
