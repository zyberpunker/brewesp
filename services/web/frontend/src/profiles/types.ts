export type RampMode = "time";

export type ProfileStep = {
  id: string;
  label: string;
  target_c: number;
  hold_duration_s: number;
  advance_policy: "auto" | "manual_release";
  ramp?: {
    mode: RampMode;
    duration_s: number;
  };
};

export type ProfileSummary = {
  slug: string;
  name: string;
  source: string;
  is_builtin: boolean;
  imported_from: string | null;
  step_count: number;
  total_duration_s: number;
  updated_at: string | null;
};

export type ProfileDetail = ProfileSummary & {
  schema_version: number;
  steps: ProfileStep[];
  device_profile: {
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
