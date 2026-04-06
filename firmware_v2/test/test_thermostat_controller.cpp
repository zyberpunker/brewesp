#include <unity.h>

#include "../src/thermostat_controller.h"

namespace {
class FakeOutputDriver : public OutputDriver {
 public:
  explicit FakeOutputDriver(bool on = false, bool known = true) {
    status_.on = on;
    status_.known = known;
    status_.description = known ? (on ? "known on" : "known off") : "unknown";
  }

  bool begin() override {
    return true;
  }

  bool setState(bool on) override {
    ++set_state_calls_;
    last_requested_state_ = on;
    if ((on && fail_on_true_) || (!on && fail_on_false_)) {
      return false;
    }
    status_.known = true;
    status_.on = on;
    status_.description = on ? "known on" : "known off";
    return true;
  }

  bool refresh() override {
    return true;
  }

  DriverStatus status() const override {
    return status_;
  }

  void setFailOnFalse(bool fail) {
    fail_on_false_ = fail;
  }

  int setStateCalls() const {
    return set_state_calls_;
  }

  bool lastRequestedState() const {
    return last_requested_state_;
  }

 private:
  DriverStatus status_;
  bool fail_on_true_ = false;
  bool fail_on_false_ = false;
  int set_state_calls_ = 0;
  bool last_requested_state_ = false;
};

FermentationConfig baseConfig() {
  FermentationConfig config;
  config.valid = true;
  config.mode = "thermostat";
  config.thermostat.setpoint_c = 20.0f;
  config.thermostat.hysteresis_c = 0.5f;
  config.thermostat.heating_delay_s = 0;
  config.thermostat.cooling_delay_s = 0;
  config.sensors.control_sensor = "primary";
  config.sensors.secondary_enabled = false;
  return config;
}

SensorSnapshot nominalSensors(float beer_c, float chamber_c = 20.0f) {
  SensorSnapshot snapshot;
  snapshot.beer.present = true;
  snapshot.beer.valid = true;
  snapshot.beer.stale = false;
  snapshot.beer.adjusted_c = beer_c;
  snapshot.chamber.present = true;
  snapshot.chamber.valid = true;
  snapshot.chamber.stale = false;
  snapshot.chamber.adjusted_c = chamber_c;
  return snapshot;
}
}  // namespace

#include "../src/thermostat_controller.cpp"

void test_primary_invalid_sensor_faults_controller() {
  ThermostatController controller;
  FermentationConfig config = baseConfig();
  SensorSnapshot snapshot = nominalSensors(18.0f);
  snapshot.beer.valid = false;
  snapshot.beer.adjusted_c = NAN;

  const ControllerState &state = controller.evaluate(config, ProfileRuntimeState(), snapshot, 0);

  TEST_ASSERT_EQUAL_STRING("fault", state.controller_state.c_str());
  TEST_ASSERT_EQUAL_STRING("control sensor invalid", state.controller_reason.c_str());
  TEST_ASSERT_EQUAL_STRING("beer sensor invalid", state.fault.c_str());
  TEST_ASSERT_FALSE(state.heating_command);
  TEST_ASSERT_FALSE(state.cooling_command);
}

void test_stale_primary_sensor_faults_controller() {
  ThermostatController controller;
  FermentationConfig config = baseConfig();
  SensorSnapshot snapshot = nominalSensors(18.0f);
  snapshot.beer.stale = true;

  const ControllerState &state = controller.evaluate(config, ProfileRuntimeState(), snapshot, 0);

  TEST_ASSERT_EQUAL_STRING("fault", state.controller_state.c_str());
  TEST_ASSERT_EQUAL_STRING("control sensor stale", state.controller_reason.c_str());
  TEST_ASSERT_EQUAL_STRING("beer sensor stale", state.fault.c_str());
}

void test_secondary_control_sensor_is_used_when_enabled() {
  ThermostatController controller;
  FermentationConfig config = baseConfig();
  config.sensors.control_sensor = "secondary";
  config.sensors.secondary_enabled = true;
  SensorSnapshot snapshot = nominalSensors(18.0f, 22.0f);

  const ControllerState &state = controller.evaluate(config, ProfileRuntimeState(), snapshot, 0);

  TEST_ASSERT_FALSE(state.heating_command);
  TEST_ASSERT_TRUE(state.cooling_command);
  TEST_ASSERT_EQUAL_STRING("cooling", state.controller_state.c_str());
}

void test_secondary_control_setting_falls_back_to_primary_when_disabled() {
  ThermostatController controller;
  FermentationConfig config = baseConfig();
  config.sensors.control_sensor = "secondary";
  config.sensors.secondary_enabled = false;
  SensorSnapshot snapshot = nominalSensors(18.0f, 22.0f);

  const ControllerState &state = controller.evaluate(config, ProfileRuntimeState(), snapshot, 0);

  TEST_ASSERT_TRUE(state.heating_command);
  TEST_ASSERT_FALSE(state.cooling_command);
  TEST_ASSERT_EQUAL_STRING("heating", state.controller_state.c_str());
}

void test_manual_mode_respects_heating_delay() {
  ThermostatController controller;
  FermentationConfig config = baseConfig();
  config.mode = "manual";
  config.manual.output = "heating";
  config.thermostat.heating_delay_s = 10;
  SensorSnapshot snapshot = nominalSensors(18.0f);

  const ControllerState &state = controller.evaluate(config, ProfileRuntimeState(), snapshot, 0);

  TEST_ASSERT_FALSE(state.heating_command);
  TEST_ASSERT_EQUAL_STRING("manual heating delay active", state.controller_reason.c_str());
}

void test_fault_forces_known_outputs_off() {
  ThermostatController controller;
  FermentationConfig config = baseConfig();
  SensorSnapshot snapshot = nominalSensors(18.0f);
  snapshot.beer.valid = false;
  snapshot.beer.adjusted_c = NAN;
  FakeOutputDriver heating(true, true);
  FakeOutputDriver cooling(true, true);

  controller.evaluate(config, ProfileRuntimeState(), snapshot, 0);
  controller.apply(heating, cooling, 0);

  TEST_ASSERT_EQUAL(1, heating.setStateCalls());
  TEST_ASSERT_EQUAL(1, cooling.setStateCalls());
  TEST_ASSERT_FALSE(heating.status().on);
  TEST_ASSERT_FALSE(cooling.status().on);
}

void test_heating_handover_turns_cooling_off_first() {
  ThermostatController controller;
  FermentationConfig config = baseConfig();
  SensorSnapshot snapshot = nominalSensors(18.0f);
  FakeOutputDriver heating(false, true);
  FakeOutputDriver cooling(true, true);

  controller.evaluate(config, ProfileRuntimeState(), snapshot, 0);
  controller.apply(heating, cooling, 0);

  TEST_ASSERT_FALSE(cooling.status().on);
  TEST_ASSERT_TRUE(heating.status().on);
  TEST_ASSERT_TRUE(heating.lastRequestedState());
  TEST_ASSERT_FALSE(cooling.lastRequestedState());
}

void test_failed_cooling_shutdown_surfaces_output_fault() {
  ThermostatController controller;
  FermentationConfig config = baseConfig();
  SensorSnapshot snapshot = nominalSensors(18.0f);
  FakeOutputDriver heating(false, true);
  FakeOutputDriver cooling(true, true);
  cooling.setFailOnFalse(true);

  controller.evaluate(config, ProfileRuntimeState(), snapshot, 0);
  controller.apply(heating, cooling, 0);

  TEST_ASSERT_FALSE(heating.status().on);
  TEST_ASSERT_TRUE(cooling.status().on);
  TEST_ASSERT_EQUAL_STRING("cooling output shutoff failed", controller.state().fault.c_str());
  TEST_ASSERT_EQUAL_STRING("fault", controller.state().controller_state.c_str());
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_primary_invalid_sensor_faults_controller);
  RUN_TEST(test_stale_primary_sensor_faults_controller);
  RUN_TEST(test_secondary_control_sensor_is_used_when_enabled);
  RUN_TEST(test_secondary_control_setting_falls_back_to_primary_when_disabled);
  RUN_TEST(test_manual_mode_respects_heating_delay);
  RUN_TEST(test_fault_forces_known_outputs_off);
  RUN_TEST(test_heating_handover_turns_cooling_off_first);
  RUN_TEST(test_failed_cooling_shutdown_surfaces_output_fault);
  return UNITY_END();
}
