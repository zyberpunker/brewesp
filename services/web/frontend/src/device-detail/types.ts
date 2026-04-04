export type TelemetryPoint = {
  recorded_at: string;
  temp_primary_c: number | null;
  temp_secondary_c: number | null;
  setpoint_c: number | null;
  effective_target_c: number | null;
  mode: string | null;
  profile_id: string | null;
  profile_step_id: string | null;
  heating_active: boolean | null;
  cooling_active: boolean | null;
};

export type TelemetryWindow = "2h" | "12h" | "2d" | "all";
export type ManualOutputState = "off" | "heating" | "cooling";

export type OutputAssignment = {
  driver: string;
  host: string;
  port: number;
  alias: string;
};

export type DiscoveredRelay = {
  host: string;
  alias: string | null;
  driver: string;
  port: number;
  model: string | null;
  is_on: boolean | null;
  last_seen_at: string | null;
  last_seen_label: string;
};

export type OutputRoutingPayload = {
  device_id: string;
  heating: OutputAssignment;
  cooling: OutputAssignment;
  discovered_relays: DiscoveredRelay[];
};

export type ProfileRuntime = {
  active_profile_id?: string | null;
  active_step_id?: string | null;
  active_step_index?: number | null;
  phase?: string | null;
  step_started_at?: number | null;
  step_hold_started_at?: number | null;
  effective_target_c?: number | null;
  waiting_for_manual_release?: boolean | null;
  paused?: boolean | null;
};

export type LivePayload = {
  device_id: string;
  display_status: "online" | "stale" | "offline" | "unknown";
  mqtt_connected: boolean;
  last_heartbeat_at: string | null;
  last_heartbeat_label: string;
  heating_state: string;
  cooling_state: string;
  last_temp_c: number | null;
  last_target_temp_c: number | null;
  last_rssi?: number | null;
  last_heap_free?: number | null;
  last_mode: string | null;
  controller_state: string | null;
  controller_reason: string | null;
  fault: string | null;
  automatic_control_active?: boolean | null;
  active_config_version?: number | null;
  beer_probe_present?: boolean | null;
  beer_probe_valid?: boolean | null;
  beer_probe_stale?: boolean | null;
  beer_probe_status?: string | null;
  beer_probe_rom?: string | null;
  chamber_probe_present?: boolean | null;
  chamber_probe_valid?: boolean | null;
  chamber_probe_stale?: boolean | null;
  chamber_probe_status?: string | null;
  chamber_probe_rom?: string | null;
  secondary_sensor_enabled?: boolean | null;
  control_sensor?: string | null;
  profile_runtime?: ProfileRuntime | null;
  fermentation_config?: {
    desired_version?: number | null;
    last_applied_version?: number | null;
    last_applied_result?: string | null;
    last_applied_message?: string | null;
    last_applied_at?: string | null;
    last_applied_at_label?: string | null;
  } | null;
  last_payload?: Record<string, unknown> | null;
};

export type FermentationPlan = {
  schema_version: number;
  version: number;
  device_id: string;
  name: string;
  mode: "thermostat" | "profile" | "manual";
  thermostat: {
    setpoint_c: number;
    hysteresis_c: number;
    cooling_delay_s: number;
    heating_delay_s: number;
  };
  sensors: {
    primary_offset_c: number;
    secondary_enabled: boolean;
    secondary_offset_c?: number;
    secondary_limit_hysteresis_c?: number;
    control_sensor: string;
  };
  alarms: {
    deviation_c: number;
    sensor_stale_s: number;
  };
  manual?: {
    output: ManualOutputState;
  };
  profile?: {
    id: string;
    steps: Array<{
      id: string;
      label: string;
      target_c: number;
      hold_duration_s: number;
      ramp_duration_s?: number;
      advance_policy: "auto" | "manual_release";
    }>;
  };
};

export type DeviceDetailBootstrap = {
  deviceId: string;
  initialLivePayload: LivePayload;
  initialTelemetry: TelemetryPoint[];
};
