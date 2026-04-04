import {
  Activity,
  ChevronRight,
  CircleAlert,
  CircleHelp,
  Clock3,
  Flame,
  Pause,
  Play,
  Power,
  Snowflake,
  SlidersHorizontal,
  Target,
  Thermometer,
  Wifi,
} from "lucide-react";
import { startTransition, useEffect, useMemo, useState } from "react";
import {
  useMutation,
  useQuery,
  useQueryClient,
} from "@tanstack/react-query";

import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader } from "@/components/ui/card";
import {
  fetchFermentationPlan,
  fetchLiveState,
  fetchOutputRouting,
  fetchTelemetry,
  scanKasaRelays,
  sendOutputCommand,
  sendProfileCommand,
  updateOutputRouting,
  updateFermentationPlan,
} from "@/device-detail/api";
import { TemperatureChart } from "@/device-detail/TemperatureChart";
import { fetchProfileDetail, fetchProfiles } from "@/profiles/api";
import type { ProfileDetail, ProfileSummary } from "@/profiles/types";
import type {
  DeviceDetailBootstrap,
  OutputRoutingPayload,
  FermentationPlan,
  TelemetryWindow,
} from "@/device-detail/types";

function formatTemperature(value: number | null | undefined) {
  if (typeof value !== "number" || Number.isNaN(value)) {
    return "n/a";
  }

  return `${value.toFixed(1)}\u00B0C`;
}

function formatRuntimePhase(phase: string | null | undefined) {
  if (!phase) {
    return "Idle";
  }

  return phase.replaceAll("_", " ");
}

function formatDurationCompact(totalSeconds: number | undefined) {
  if (typeof totalSeconds !== "number" || Number.isNaN(totalSeconds)) {
    return "n/a";
  }
  if (totalSeconds % 86400 === 0) {
    return `${totalSeconds / 86400} d`;
  }
  if (totalSeconds % 3600 === 0) {
    return `${totalSeconds / 3600} h`;
  }
  return `${Math.round(totalSeconds / 60)} min`;
}

function toneFromStatus(status: string) {
  if (status === "online") {
    return "online";
  }
  if (status === "stale") {
    return "stale";
  }
  if (status === "offline") {
    return "offline";
  }
  return "neutral";
}

const MIN_TARGET_C = -20;
const MAX_TARGET_C = 50;

function clampTarget(value: number) {
  return Math.min(MAX_TARGET_C, Math.max(MIN_TARGET_C, value));
}

type ConfirmAction =
  | "apply_target"
  | "pause_profile"
  | "release_manual_hold"
  | "all_off";

type DeviceSettingsDraft = {
  hysteresis_c: string;
  cooling_delay_s: string;
  heating_delay_s: string;
  primary_offset_c: string;
  secondary_enabled: boolean;
  control_sensor: string;
  secondary_offset_c: string;
  secondary_limit_hysteresis_c: string;
  deviation_c: string;
  sensor_stale_s: string;
};

type FermentationSetupDraft = {
  name: string;
  mode: "thermostat" | "profile";
};

type OutputRoutingDraft = {
  heating_driver: string;
  heating_selected_host: string;
  heating_manual_host: string;
  heating_alias: string;
  cooling_driver: string;
  cooling_selected_host: string;
  cooling_manual_host: string;
  cooling_alias: string;
};

const TELEMETRY_WINDOWS: Array<{
  value: TelemetryWindow;
  label: string;
  description: string;
}> = [
  { value: "2h", label: "2h", description: "Last 2 hours" },
  { value: "12h", label: "12h", description: "Last 12 hours" },
  { value: "2d", label: "2d", description: "Last 2 days" },
  { value: "all", label: "All", description: "All available history" },
];

function buildSettingsDraft(plan: FermentationPlan): DeviceSettingsDraft {
  return {
    hysteresis_c: plan.thermostat.hysteresis_c.toString(),
    cooling_delay_s: plan.thermostat.cooling_delay_s.toString(),
    heating_delay_s: plan.thermostat.heating_delay_s.toString(),
    primary_offset_c: plan.sensors.primary_offset_c.toString(),
    secondary_enabled: plan.sensors.secondary_enabled,
    control_sensor: plan.sensors.secondary_enabled
      ? plan.sensors.control_sensor
      : "primary",
    secondary_offset_c: (plan.sensors.secondary_offset_c ?? 0).toString(),
    secondary_limit_hysteresis_c: (
      plan.sensors.secondary_limit_hysteresis_c ?? 1.5
    ).toString(),
    deviation_c: plan.alarms.deviation_c.toString(),
    sensor_stale_s: plan.alarms.sensor_stale_s.toString(),
  };
}

function buildFermentationSetupDraft(
  plan: FermentationPlan,
): FermentationSetupDraft {
  return {
    name: plan.name,
    mode: plan.mode,
  };
}

function slugifyProfileId(name: string) {
  const slug = name
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "");

  return `${slug || "fermentation"}-plan`;
}

function buildDefaultProfilePlan(name: string, setpoint: number) {
  return {
    id: slugifyProfileId(name),
    steps: [
      {
        id: "step-1",
        label: name,
        target_c: setpoint,
        hold_duration_s: 0,
        advance_policy: "manual_release" as const,
      },
    ],
  };
}

function buildOutputRoutingDraft(
  routing: OutputRoutingPayload,
): OutputRoutingDraft {
  const relayHosts = new Set(routing.discovered_relays.map((relay) => relay.host));
  const heatingHost = routing.heating.host ?? "";
  const coolingHost = routing.cooling.host ?? "";

  return {
    heating_driver: routing.heating.driver || "kasa_local",
    heating_selected_host: relayHosts.has(heatingHost) ? heatingHost : "",
    heating_manual_host: relayHosts.has(heatingHost) ? "" : heatingHost,
    heating_alias: routing.heating.alias ?? "",
    cooling_driver: routing.cooling.driver || "kasa_local",
    cooling_selected_host: relayHosts.has(coolingHost) ? coolingHost : "",
    cooling_manual_host: relayHosts.has(coolingHost) ? "" : coolingHost,
    cooling_alias: routing.cooling.alias ?? "",
  };
}

function parseSettingsDraft(draft: DeviceSettingsDraft) {
  const hysteresis = Number(draft.hysteresis_c);
  const coolingDelay = Number(draft.cooling_delay_s);
  const heatingDelay = Number(draft.heating_delay_s);
  const primaryOffset = Number(draft.primary_offset_c);
  const deviation = Number(draft.deviation_c);
  const sensorStale = Number(draft.sensor_stale_s);
  const secondaryOffset = Number(draft.secondary_offset_c);
  const secondaryLimit = Number(draft.secondary_limit_hysteresis_c);

  if (
    [
      hysteresis,
      coolingDelay,
      heatingDelay,
      primaryOffset,
      deviation,
      sensorStale,
    ].some((value) => Number.isNaN(value))
  ) {
    return {
      error: "Fill in all required settings with numeric values before saving.",
      values: null,
    };
  }

  if (
    draft.secondary_enabled &&
    [secondaryOffset, secondaryLimit].some((value) => Number.isNaN(value))
  ) {
    return {
      error: "Secondary probe settings must be numeric when the chamber probe is enabled.",
      values: null,
    };
  }

  if (hysteresis < 0.1 || hysteresis > 5) {
    return {
      error: "Hysteresis must be between 0.1 and 5.0 C.",
      values: null,
    };
  }

  if (coolingDelay < 0 || coolingDelay > 3600) {
    return {
      error: "Cooling delay must be between 0 and 3600 seconds.",
      values: null,
    };
  }

  if (heatingDelay < 0 || heatingDelay > 3600) {
    return {
      error: "Heating delay must be between 0 and 3600 seconds.",
      values: null,
    };
  }

  if (primaryOffset < -5 || primaryOffset > 5) {
    return {
      error: "Primary probe offset must be between -5.0 and 5.0 C.",
      values: null,
    };
  }

  if (deviation < 0 || deviation > 25) {
    return {
      error: "Deviation alarm must be between 0.0 and 25.0 C.",
      values: null,
    };
  }

  if (sensorStale < 1 || sensorStale > 600) {
    return {
      error: "Sensor stale timeout must be between 1 and 600 seconds.",
      values: null,
    };
  }

  if (
    draft.secondary_enabled &&
    (draft.control_sensor !== "primary" && draft.control_sensor !== "secondary")
  ) {
    return {
      error: "Control sensor must be primary or secondary.",
      values: null,
    };
  }

  if (draft.secondary_enabled && (secondaryOffset < -5 || secondaryOffset > 5)) {
    return {
      error: "Secondary probe offset must be between -5.0 and 5.0 C.",
      values: null,
    };
  }

  if (draft.secondary_enabled && (secondaryLimit < 0.1 || secondaryLimit > 25)) {
    return {
      error: "Secondary limit hysteresis must be between 0.1 and 25.0 C.",
      values: null,
    };
  }

  return {
    error: null,
    values: {
      hysteresis_c: hysteresis,
      cooling_delay_s: Math.round(coolingDelay),
      heating_delay_s: Math.round(heatingDelay),
      primary_offset_c: primaryOffset,
      secondary_enabled: draft.secondary_enabled,
      control_sensor: draft.secondary_enabled ? draft.control_sensor : "primary",
      secondary_offset_c: draft.secondary_enabled ? secondaryOffset : 0,
      secondary_limit_hysteresis_c: draft.secondary_enabled ? secondaryLimit : 1.5,
      deviation_c: deviation,
      sensor_stale_s: Math.round(sensorStale),
    },
  };
}

export function DeviceDetailApp({
  deviceId,
  initialLivePayload,
  initialTelemetry,
}: DeviceDetailBootstrap) {
  const queryClient = useQueryClient();
  const [draftTarget, setDraftTarget] = useState<number>(
    clampTarget(initialLivePayload.last_target_temp_c ?? 20),
  );
  const [telemetryWindow, setTelemetryWindow] = useState<TelemetryWindow>("12h");
  const [isDraftTargetDirty, setIsDraftTargetDirty] = useState(false);
  const [confirmAction, setConfirmAction] = useState<ConfirmAction | null>(null);
  const [isDiagnosticsOpen, setIsDiagnosticsOpen] = useState(false);
  const [isFermentationSetupOpen, setIsFermentationSetupOpen] = useState(false);
  const [isProfileChooserOpen, setIsProfileChooserOpen] = useState(false);
  const [selectedLibraryProfileSlug, setSelectedLibraryProfileSlug] = useState<string | null>(
    null,
  );
  const [profileChooserError, setProfileChooserError] = useState<string | null>(null);
  const [fermentationSetupDraft, setFermentationSetupDraft] =
    useState<FermentationSetupDraft | null>(null);
  const [fermentationSetupError, setFermentationSetupError] = useState<string | null>(
    null,
  );
  const [isSettingsOpen, setIsSettingsOpen] = useState(false);
  const [settingsDraft, setSettingsDraft] = useState<DeviceSettingsDraft | null>(null);
  const [settingsError, setSettingsError] = useState<string | null>(null);
  const [isRoutingOpen, setIsRoutingOpen] = useState(false);
  const [routingDraft, setRoutingDraft] = useState<OutputRoutingDraft | null>(null);
  const [routingError, setRoutingError] = useState<string | null>(null);
  const [isRoutingDirty, setIsRoutingDirty] = useState(false);

  const liveQuery = useQuery({
    queryKey: ["device-live", deviceId],
    queryFn: () => fetchLiveState(deviceId),
    initialData: initialLivePayload,
    refetchInterval: 2000,
  });

  const telemetryQuery = useQuery({
    queryKey: ["device-telemetry", deviceId, telemetryWindow],
    queryFn: () => fetchTelemetry(deviceId, telemetryWindow),
    initialData: telemetryWindow === "12h" ? initialTelemetry : undefined,
    refetchInterval: 30000,
  });
  const fermentationPlanQuery = useQuery({
    queryKey: ["device-fermentation-plan", deviceId],
    queryFn: () => fetchFermentationPlan(deviceId),
  });
  const outputRoutingQuery = useQuery({
    queryKey: ["device-output-routing", deviceId],
    queryFn: () => fetchOutputRouting(deviceId),
    enabled: isRoutingOpen,
    refetchInterval: isRoutingOpen ? 5000 : false,
  });
  const profileLibraryQuery = useQuery({
    queryKey: ["fermentation-profiles"],
    queryFn: fetchProfiles,
    enabled: isProfileChooserOpen,
  });
  const selectedLibraryProfileQuery = useQuery({
    queryKey: ["fermentation-profile", selectedLibraryProfileSlug],
    queryFn: () => fetchProfileDetail(selectedLibraryProfileSlug ?? ""),
    enabled: isProfileChooserOpen && Boolean(selectedLibraryProfileSlug),
  });

  const live = liveQuery.data;
  const telemetry = telemetryQuery.data ?? [];
  const fermentationPlan = fermentationPlanQuery.data;
  const retainedTarget = fermentationPlan?.thermostat?.setpoint_c;
  const libraryProfiles = profileLibraryQuery.data ?? [];
  const selectedLibraryProfile = selectedLibraryProfileQuery.data;

  useEffect(() => {
    if (!isDraftTargetDirty) {
      if (typeof retainedTarget === "number") {
        setDraftTarget(clampTarget(retainedTarget));
        return;
      }
      if (typeof live.last_target_temp_c === "number") {
        setDraftTarget(clampTarget(live.last_target_temp_c));
      }
    }
  }, [isDraftTargetDirty, live.last_target_temp_c, retainedTarget]);

  useEffect(() => {
    if (
      isDraftTargetDirty &&
      typeof retainedTarget === "number" &&
      clampTarget(retainedTarget) === draftTarget
    ) {
      setIsDraftTargetDirty(false);
    }
  }, [draftTarget, isDraftTargetDirty, retainedTarget]);

  useEffect(() => {
    if (isFermentationSetupOpen && fermentationPlan && !fermentationSetupDraft) {
      setFermentationSetupDraft(buildFermentationSetupDraft(fermentationPlan));
    }
  }, [
    fermentationPlan,
    fermentationSetupDraft,
    isFermentationSetupOpen,
  ]);

  useEffect(() => {
    if (isRoutingOpen && outputRoutingQuery.data && !isRoutingDirty) {
      setRoutingDraft(buildOutputRoutingDraft(outputRoutingQuery.data));
    }
  }, [isRoutingDirty, isRoutingOpen, outputRoutingQuery.data]);

  useEffect(() => {
    if (
      !isProfileChooserOpen ||
      !libraryProfiles.length
    ) {
      return;
    }
    if (!selectedLibraryProfileSlug) {
      setSelectedLibraryProfileSlug(libraryProfiles[0].slug);
      return;
    }
    if (!libraryProfiles.some((profile) => profile.slug === selectedLibraryProfileSlug)) {
      setSelectedLibraryProfileSlug(libraryProfiles[0].slug);
    }
  }, [isProfileChooserOpen, libraryProfiles, selectedLibraryProfileSlug]);

  const refreshQueries = async () => {
    await Promise.all([
      queryClient.invalidateQueries({ queryKey: ["device-live", deviceId] }),
      queryClient.invalidateQueries({ queryKey: ["device-telemetry", deviceId] }),
      queryClient.invalidateQueries({
        queryKey: ["device-fermentation-plan", deviceId],
      }),
      queryClient.invalidateQueries({
        queryKey: ["device-output-routing", deviceId],
      }),
    ]);
  };

  const outputMutation = useMutation({
    mutationFn: (action: Parameters<typeof sendOutputCommand>[1]) =>
      sendOutputCommand(deviceId, action),
    onSettled: refreshQueries,
  });

  const profileMutation = useMutation({
    mutationFn: (command: Parameters<typeof sendProfileCommand>[1]) =>
      sendProfileCommand(deviceId, command),
    onSettled: refreshQueries,
  });

  const targetMutation = useMutation({
    mutationFn: async (nextTarget: number) => {
      const current = await fetchFermentationPlan(deviceId);
      const nextPlan = {
        ...current,
        version: (current.version ?? 0) + 1,
        thermostat: {
          ...current.thermostat,
          setpoint_c: Number(clampTarget(nextTarget).toFixed(1)),
        },
      };
      return updateFermentationPlan(deviceId, nextPlan);
    },
    onSettled: refreshQueries,
  });

  const settingsMutation = useMutation({
    mutationFn: async (draft: DeviceSettingsDraft) => {
      const parsed = parseSettingsDraft(draft);
      if (parsed.error || !parsed.values) {
        throw new Error(
          parsed.error ?? "Failed to parse device settings before save.",
        );
      }

      const current = await fetchFermentationPlan(deviceId);
      const nextPlan: FermentationPlan = {
        ...current,
        version: (current.version ?? 0) + 1,
        thermostat: {
          ...current.thermostat,
          hysteresis_c: parsed.values.hysteresis_c,
          cooling_delay_s: parsed.values.cooling_delay_s,
          heating_delay_s: parsed.values.heating_delay_s,
        },
        sensors: {
          ...current.sensors,
          primary_offset_c: parsed.values.primary_offset_c,
          secondary_enabled: parsed.values.secondary_enabled,
          control_sensor: parsed.values.control_sensor,
          secondary_offset_c: parsed.values.secondary_offset_c,
          secondary_limit_hysteresis_c:
            parsed.values.secondary_limit_hysteresis_c,
        },
        alarms: {
          ...current.alarms,
          deviation_c: parsed.values.deviation_c,
          sensor_stale_s: parsed.values.sensor_stale_s,
        },
      };

      return updateFermentationPlan(deviceId, nextPlan);
    },
    onSettled: refreshQueries,
  });
  const fermentationSetupMutation = useMutation({
    mutationFn: async (draft: FermentationSetupDraft) => {
      const current = await fetchFermentationPlan(deviceId);
      const nextName = draft.name.trim() || "Default fermentation";
      const nextPlan: FermentationPlan = {
        ...current,
        version: (current.version ?? 0) + 1,
        name: nextName,
        mode: draft.mode,
      };

      if (draft.mode === "profile") {
        nextPlan.profile =
          current.profile ??
          buildDefaultProfilePlan(nextName, current.thermostat.setpoint_c);
      }

      return updateFermentationPlan(deviceId, nextPlan);
    },
    onSettled: refreshQueries,
  });
  const applyLibraryProfileMutation = useMutation({
    mutationFn: async (profile: ProfileDetail) => {
      const current = await fetchFermentationPlan(deviceId);
      const nextPlan: FermentationPlan = {
        ...current,
        version: (current.version ?? 0) + 1,
        mode: "profile",
        profile: profile.device_profile,
      };
      return updateFermentationPlan(deviceId, nextPlan);
    },
    onSettled: refreshQueries,
  });
  const outputRoutingMutation = useMutation({
    mutationFn: (draft: OutputRoutingDraft) => {
      const heatingHost =
        draft.heating_manual_host.trim() || draft.heating_selected_host.trim();
      const coolingHost =
        draft.cooling_manual_host.trim() || draft.cooling_selected_host.trim();

      return updateOutputRouting(deviceId, {
        heating: {
          driver: draft.heating_driver,
          host: heatingHost,
          alias: draft.heating_alias.trim(),
        },
        cooling: {
          driver: draft.cooling_driver,
          host: coolingHost,
          alias: draft.cooling_alias.trim(),
        },
      });
    },
    onSettled: refreshQueries,
  });
  const scanRelaysMutation = useMutation({
    mutationFn: () => scanKasaRelays(deviceId),
    onSuccess: async () => {
      await queryClient.invalidateQueries({
        queryKey: ["device-output-routing", deviceId],
      });
    },
  });

  const profileRuntime = live.profile_runtime;
  const canAdjustTarget = fermentationPlan?.mode === "thermostat";
  const profilePhase = formatRuntimePhase(profileRuntime?.phase);
  const activeTarget = clampTarget(draftTarget);
  const runtimeTone =
    live.fault != null
      ? "fault"
      : live.heating_state === "on"
        ? "heating"
        : live.cooling_state === "on"
          ? "cooling"
          : "neutral";
  const lastPayload = useMemo(
    () => JSON.stringify(live.last_payload ?? {}, null, 2),
    [live.last_payload],
  );
  const isConfirmPending =
    outputMutation.isPending || profileMutation.isPending || targetMutation.isPending;
  const isFermentationSetupPending = fermentationSetupMutation.isPending;
  const isProfileChooserPending = applyLibraryProfileMutation.isPending;
  const isSettingsPending = settingsMutation.isPending;
  const isRoutingPending =
    outputRoutingMutation.isPending || scanRelaysMutation.isPending;
  const telemetrySampleLabel =
    telemetry.length === 1 ? "1 sample" : `${telemetry.length} samples`;
  const telemetryWindowDescription =
    TELEMETRY_WINDOWS.find((option) => option.value === telemetryWindow)?.description ??
    "All available history";

  useEffect(() => {
    if (
      !confirmAction &&
      !isDiagnosticsOpen &&
      !isSettingsOpen &&
      !isFermentationSetupOpen &&
      !isProfileChooserOpen &&
      !isRoutingOpen
    ) {
      return;
    }

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key !== "Escape") {
        return;
      }

      if (confirmAction && !isConfirmPending) {
        setConfirmAction(null);
      }

      if (isDiagnosticsOpen) {
        setIsDiagnosticsOpen(false);
      }

      if (isFermentationSetupOpen && !isFermentationSetupPending) {
        setIsFermentationSetupOpen(false);
        setFermentationSetupError(null);
      }

      if (isProfileChooserOpen && !isProfileChooserPending) {
        setIsProfileChooserOpen(false);
        setProfileChooserError(null);
      }

      if (isSettingsOpen && !isSettingsPending) {
        setIsSettingsOpen(false);
        setSettingsError(null);
      }

      if (isRoutingOpen && !isRoutingPending) {
        setIsRoutingOpen(false);
        setRoutingError(null);
      }
    };

    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [
    confirmAction,
    isConfirmPending,
    isDiagnosticsOpen,
    isFermentationSetupOpen,
    isFermentationSetupPending,
    isProfileChooserOpen,
    isProfileChooserPending,
    isRoutingOpen,
    isRoutingPending,
    isSettingsOpen,
    isSettingsPending,
  ]);

  const confirmationCopy =
    confirmAction === "apply_target"
      ? {
          title: "Apply target change?",
          body: `This will publish ${formatTemperature(activeTarget)} as the retained target for ${deviceId}. Use this only when you intend to change the controller target.`,
          confirmLabel: "Apply target",
          confirmTone: "primary" as const,
        }
      : confirmAction === "pause_profile"
        ? {
            title: "Pause active profile?",
            body: "This pauses profile progress and leaves the current step on hold until an operator resumes it.",
            confirmLabel: "Pause profile",
            confirmTone: "danger" as const,
          }
        : confirmAction === "release_manual_hold"
          ? {
              title: "Release manual hold?",
              body: "This lets the controller continue past the current manual-release hold. Confirm only when the step is ready to advance.",
              confirmLabel: "Release hold",
              confirmTone: "primary" as const,
            }
          : confirmAction === "all_off"
            ? {
                title: "Turn all outputs off?",
                body: "This immediately sends an off command to both heating and cooling outputs. Confirm only if you intend to stop all output activity now.",
                confirmLabel: "All outputs off",
                confirmTone: "danger" as const,
              }
            : null;

  const runConfirmedAction = () => {
    if (confirmAction === "apply_target") {
      targetMutation.mutate(activeTarget, {
        onSettled: () => setConfirmAction(null),
      });
      return;
    }
    if (confirmAction === "pause_profile") {
      profileMutation.mutate("profile_pause", {
        onSettled: () => setConfirmAction(null),
      });
      return;
    }
    if (confirmAction === "release_manual_hold") {
      profileMutation.mutate("profile_release_hold", {
        onSettled: () => setConfirmAction(null),
      });
      return;
    }
    if (confirmAction === "all_off") {
      outputMutation.mutate("all_off", {
        onSettled: () => setConfirmAction(null),
      });
    }
  };

  const openFermentationSetupModal = () => {
    if (!fermentationPlan) {
      return;
    }

    startTransition(() => {
      setFermentationSetupDraft(buildFermentationSetupDraft(fermentationPlan));
      setFermentationSetupError(null);
      setIsFermentationSetupOpen(true);
    });
  };

  const openProfileChooser = () => {
    startTransition(() => {
      setSelectedLibraryProfileSlug(fermentationPlan?.profile?.id ?? null);
      setProfileChooserError(null);
      setIsProfileChooserOpen(true);
    });
  };

  const saveFermentationSetup = () => {
    if (!fermentationSetupDraft) {
      return;
    }

    if (!fermentationSetupDraft.name.trim()) {
      setFermentationSetupError("Fermentation name is required.");
      return;
    }

    setFermentationSetupError(null);
    fermentationSetupMutation.mutate(fermentationSetupDraft, {
      onSuccess: () => {
        setIsFermentationSetupOpen(false);
      },
      onError: (error) => {
        setFermentationSetupError(
          error instanceof Error
            ? error.message
            : "Failed to save fermentation setup.",
        );
      },
    });
  };

  const applySelectedLibraryProfile = () => {
    if (!selectedLibraryProfile) {
      return;
    }
    setProfileChooserError(null);
    applyLibraryProfileMutation.mutate(selectedLibraryProfile, {
      onSuccess: () => {
        setIsProfileChooserOpen(false);
      },
      onError: (error) => {
        setProfileChooserError(
          error instanceof Error
            ? error.message
            : "Failed to apply the selected profile.",
        );
      },
    });
  };

  const openSettingsModal = () => {
    if (!fermentationPlan) {
      return;
    }

    startTransition(() => {
      setSettingsDraft(buildSettingsDraft(fermentationPlan));
      setSettingsError(null);
      setIsSettingsOpen(true);
    });
  };

  const openRoutingModal = () => {
    startTransition(() => {
      if (outputRoutingQuery.data) {
        setRoutingDraft(buildOutputRoutingDraft(outputRoutingQuery.data));
      } else {
        setRoutingDraft(null);
      }
      setIsRoutingDirty(false);
      setRoutingError(null);
      setIsRoutingOpen(true);
    });
  };

  const updateSettingsField = <K extends keyof DeviceSettingsDraft>(
    field: K,
    value: DeviceSettingsDraft[K],
  ) => {
    setSettingsDraft((current) => {
      if (!current) {
        return current;
      }

      const nextDraft = {
        ...current,
        [field]: value,
      };

      if (field === "secondary_enabled" && value === false) {
        nextDraft.control_sensor = "primary";
      }

      return nextDraft;
    });
  };

  const updateRoutingField = <K extends keyof OutputRoutingDraft>(
    field: K,
    value: OutputRoutingDraft[K],
  ) => {
    setIsRoutingDirty(true);
    setRoutingDraft((current) => {
      if (!current) {
        return current;
      }

      return {
        ...current,
        [field]: value,
      };
    });
  };

  const saveSettings = () => {
    if (!settingsDraft) {
      return;
    }

    const parsed = parseSettingsDraft(settingsDraft);
    if (parsed.error) {
      setSettingsError(parsed.error);
      return;
    }

    setSettingsError(null);
    settingsMutation.mutate(settingsDraft, {
      onSuccess: () => {
        setIsSettingsOpen(false);
      },
      onError: (error) => {
        setSettingsError(
          error instanceof Error ? error.message : "Failed to save device settings.",
        );
      },
    });
  };

  const saveRouting = () => {
    if (!routingDraft) {
      return;
    }

    setRoutingError(null);
    outputRoutingMutation.mutate(routingDraft, {
      onSuccess: () => {
        setIsRoutingDirty(false);
        setIsRoutingOpen(false);
      },
      onError: (error) => {
        setRoutingError(
          error instanceof Error ? error.message : "Failed to save output routing.",
        );
      },
    });
  };

  return (
    <div className="relative min-h-screen overflow-x-hidden bg-[var(--bg)] text-[var(--ink)]">
      <div className="absolute inset-x-0 top-0 -z-10 h-[24rem] bg-[radial-gradient(circle_at_top_left,rgba(47,108,96,0.22),transparent_32%),radial-gradient(circle_at_top_right,rgba(212,106,58,0.16),transparent_28%),linear-gradient(180deg,#f7f3ec_0%,#f4f1ea_100%)]" />
      <main className="mx-auto flex w-[min(1320px,calc(100vw-1.5rem))] flex-col gap-4 py-5 md:w-[min(1320px,calc(100vw-2rem))] md:py-7">
        <section className="flex flex-wrap items-center justify-between gap-3 rounded-[24px] bg-white/72 px-4 py-3 ring-1 ring-black/6 backdrop-blur">
          <div className="flex flex-wrap items-center gap-2 text-sm font-semibold text-[var(--muted)]">
            <a
              className="inline-flex min-h-11 items-center rounded-full px-4 text-[var(--ink)] transition hover:bg-white"
              href="/"
            >
              Overview
            </a>
            <ChevronRight className="size-4" />
            <span className="inline-flex min-h-11 items-center rounded-full bg-[var(--surface-strong)] px-4 text-[var(--ink)]">
              {deviceId}
            </span>
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <Button variant="subtle" onClick={() => setIsDiagnosticsOpen(true)}>
              Diagnostics
            </Button>
            <Button
              variant="subtle"
              onClick={openProfileChooser}
              disabled={fermentationPlanQuery.isLoading}
            >
              Choose profile
            </Button>
            <Button
              variant="subtle"
              onClick={openFermentationSetupModal}
              disabled={fermentationPlanQuery.isLoading || !fermentationPlan}
            >
              Fermentation setup
            </Button>
            <Button
              variant="subtle"
              onClick={openSettingsModal}
              disabled={fermentationPlanQuery.isLoading || !fermentationPlan}
            >
              <SlidersHorizontal className="size-4" />
              Device settings
            </Button>
            <Button variant="subtle" onClick={openRoutingModal}>
              Output routing
            </Button>
          </div>
        </section>

        <section className="grid gap-4 xl:grid-cols-[1.8fr_1fr]">
          <Card className="overflow-hidden">
            <CardContent className="px-7 py-7">
              <div className="flex flex-wrap items-start justify-between gap-5">
                <div className="space-y-3">
                  <p className="text-[11px] font-bold uppercase tracking-[0.22em] text-[var(--accent)]">
                    BrewESP Control Surface
                  </p>
                  <div className="space-y-2">
                    <h1 className="text-4xl font-extrabold tracking-[-0.06em] md:text-6xl">
                      {deviceId}
                    </h1>
                    <div className="flex flex-wrap items-center gap-2">
                      <Badge tone={toneFromStatus(live.display_status)}>
                        <Wifi className="size-3.5" />
                        {live.display_status}
                      </Badge>
                      <Badge tone="neutral">
                        <Activity className="size-3.5" />
                        {live.last_mode ?? "mode n/a"}
                      </Badge>
                      <Badge tone={runtimeTone}>
                        <Clock3 className="size-3.5" />
                        {profilePhase}
                      </Badge>
                    </div>
                  </div>
                  <p className="max-w-2xl text-sm leading-6 text-[var(--muted)] md:text-base">
                    Live operator view focused on product temperature, active target
                    and the runtime state that actually drives the controller.
                  </p>
                </div>

                <div className="grid min-w-[260px] flex-1 gap-3 sm:grid-cols-2 xl:max-w-[420px]">
                  <div className="rounded-[24px] bg-white/72 p-4 ring-1 ring-black/6">
                    <div className="flex items-center gap-2 text-[var(--muted)]">
                      <Thermometer className="size-4" />
                      <span className="text-xs font-semibold uppercase tracking-[0.18em]">
                        Beer
                      </span>
                    </div>
                    <strong className="mt-3 block text-4xl font-extrabold tracking-[-0.06em]">
                      {formatTemperature(live.last_temp_c)}
                    </strong>
                  </div>
                  <div className="rounded-[24px] bg-[color:color-mix(in_srgb,var(--heat)_10%,white)] p-4 ring-1 ring-black/6">
                    <div className="flex items-center gap-2 text-[var(--muted)]">
                      <Target className="size-4" />
                      <span className="text-xs font-semibold uppercase tracking-[0.18em]">
                        Target
                      </span>
                    </div>
                    <strong className="mt-3 block text-4xl font-extrabold tracking-[-0.06em]">
                      {formatTemperature(
                        profileRuntime?.effective_target_c ?? live.last_target_temp_c,
                      )}
                    </strong>
                  </div>
                  <div className="rounded-[24px] bg-white/72 p-4 ring-1 ring-black/6">
                    <div className="flex items-center gap-2 text-[var(--muted)]">
                      <Flame className="size-4" />
                      <span className="text-xs font-semibold uppercase tracking-[0.18em]">
                        Heating
                      </span>
                    </div>
                    <strong className="mt-3 block text-2xl font-bold capitalize">
                      {live.heating_state}
                    </strong>
                  </div>
                  <div className="rounded-[24px] bg-white/72 p-4 ring-1 ring-black/6">
                    <div className="flex items-center gap-2 text-[var(--muted)]">
                      <Snowflake className="size-4" />
                      <span className="text-xs font-semibold uppercase tracking-[0.18em]">
                        Cooling
                      </span>
                    </div>
                    <strong className="mt-3 block text-2xl font-bold capitalize">
                      {live.cooling_state}
                    </strong>
                  </div>
                </div>
              </div>

              {live.fault ? (
                <div className="mt-6 rounded-[24px] border border-[color:color-mix(in_srgb,var(--fault)_20%,black)] bg-[color:color-mix(in_srgb,var(--fault)_10%,white)] px-4 py-3 text-[var(--fault)] shadow-[0_12px_26px_rgba(178,65,59,0.1)]">
                  <div className="flex items-start gap-3">
                    <CircleAlert className="mt-0.5 size-5 shrink-0" />
                    <div>
                      <p className="text-xs font-bold uppercase tracking-[0.18em]">
                        Controller fault
                      </p>
                      <strong className="mt-1 block text-base font-semibold">
                        {live.fault}
                      </strong>
                    </div>
                  </div>
                </div>
              ) : null}
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <div>
                <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                  Quick control
                </p>
                <h2 className="mt-2 text-2xl font-bold tracking-[-0.04em]">
                  Runtime actions
                </h2>
              </div>
            </CardHeader>
            <CardContent className="space-y-5">
              <div className="rounded-[24px] bg-[var(--surface-strong)] p-4">
                <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                  Active target
                </p>
                <div className="mt-4 flex items-center justify-between gap-3">
                  <Button
                    variant="subtle"
                    onClick={() => {
                      setIsDraftTargetDirty(true);
                      setDraftTarget((current) =>
                        clampTarget(Number((current - 0.5).toFixed(1))),
                      );
                    }}
                    disabled={!canAdjustTarget || targetMutation.isPending}
                  >
                    -
                  </Button>
                  <strong className="text-3xl font-extrabold tracking-[-0.05em]">
                    {formatTemperature(draftTarget)}
                  </strong>
                  <Button
                    variant="subtle"
                    onClick={() => {
                      setIsDraftTargetDirty(true);
                      setDraftTarget((current) =>
                        clampTarget(Number((current + 0.5).toFixed(1))),
                      );
                    }}
                    disabled={!canAdjustTarget || targetMutation.isPending}
                  >
                    +
                  </Button>
                </div>
                <p className="mt-3 text-sm leading-6 text-[var(--muted)]">
                  {fermentationPlanQuery.isLoading
                    ? "Loading the retained fermentation config."
                    : canAdjustTarget
                    ? "Updates the retained fermentation config target."
                    : "Direct target edits are disabled while the retained plan is in profile mode."}
                </p>
                <Button
                  className="mt-4 w-full"
                  variant="primary"
                  onClick={() => setConfirmAction("apply_target")}
                  disabled={!canAdjustTarget || targetMutation.isPending}
                >
                  <Target className="size-4" />
                  Apply target
                </Button>
              </div>

              <div className="grid gap-2 sm:grid-cols-2">
                <Button
                  tone="heat"
                  data-active={live.heating_state === "on"}
                  onClick={() => outputMutation.mutate("heating_on")}
                  disabled={outputMutation.isPending}
                >
                  <Flame className="size-4" />
                  Heat on
                </Button>
                <Button
                  tone="heat"
                  onClick={() => outputMutation.mutate("heating_off")}
                  disabled={outputMutation.isPending}
                >
                  <Power className="size-4" />
                  Heat off
                </Button>
                <Button
                  tone="cool"
                  data-active={live.cooling_state === "on"}
                  onClick={() => outputMutation.mutate("cooling_on")}
                  disabled={outputMutation.isPending}
                >
                  <Snowflake className="size-4" />
                  Cool on
                </Button>
                <Button
                  tone="cool"
                  onClick={() => outputMutation.mutate("cooling_off")}
                  disabled={outputMutation.isPending}
                >
                  <Power className="size-4" />
                  Cool off
                </Button>
              </div>

              <div className="grid gap-2">
                {profileRuntime?.paused ? (
                  <Button
                    variant="primary"
                    onClick={() => profileMutation.mutate("profile_resume")}
                    disabled={profileMutation.isPending}
                  >
                    <Play className="size-4" />
                    Resume profile
                  </Button>
                ) : (
                  <Button
                    variant="secondary"
                    onClick={() => setConfirmAction("pause_profile")}
                    disabled={
                      profileMutation.isPending || live.last_mode !== "profile"
                    }
                  >
                    <Pause className="size-4" />
                    Pause profile
                  </Button>
                )}
                <Button
                  variant="secondary"
                  onClick={() => setConfirmAction("release_manual_hold")}
                  disabled={
                    profileMutation.isPending ||
                    !profileRuntime?.waiting_for_manual_release
                  }
                >
                  <Play className="size-4" />
                  Release manual hold
                </Button>
                <Button
                  variant="danger"
                  onClick={() => setConfirmAction("all_off")}
                  disabled={outputMutation.isPending}
                >
                  <Power className="size-4" />
                  All outputs off
                </Button>
              </div>
            </CardContent>
          </Card>
        </section>

        <section className="grid gap-4 xl:grid-cols-[1.8fr_1fr]">
          <Card>
            <CardHeader>
              <div className="flex flex-wrap items-start justify-between gap-3">
                <div>
                  <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                    Thermal history
                  </p>
                  <h2 className="mt-2 text-2xl font-bold tracking-[-0.04em]">
                    Beer vs target over time
                  </h2>
                </div>
                <div className="flex max-w-full flex-col items-start gap-2 md:items-end">
                  <div className="flex flex-wrap gap-2 md:justify-end">
                    {TELEMETRY_WINDOWS.map((option) => (
                      <Button
                        key={option.value}
                        variant={
                          telemetryWindow === option.value ? "primary" : "subtle"
                        }
                        className="min-h-9 px-3 text-xs"
                        onClick={() => setTelemetryWindow(option.value)}
                      >
                        {option.label}
                      </Button>
                    ))}
                  </div>
                  <Badge tone="neutral">
                    <Activity className="size-3.5" />
                    {telemetryWindowDescription} · {telemetrySampleLabel}
                  </Badge>
                </div>
              </div>
            </CardHeader>
            <CardContent className="space-y-4">
              <p className="text-sm leading-6 text-[var(--muted)]">
                Time axis uses 24-hour clock. The range buttons rescale the chart,
                and you can still drag or scroll in the graph for manual zoom.
              </p>
              <TemperatureChart telemetry={telemetry} window={telemetryWindow} />
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <div>
                <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                  Profile runtime
                </p>
                <h2 className="mt-2 text-2xl font-bold tracking-[-0.04em]">
                  Controller progress
                </h2>
              </div>
            </CardHeader>
            <CardContent className="grid gap-3">
              <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-1">
                <div className="rounded-[22px] bg-[var(--surface-strong)] p-4">
                  <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                    Active profile
                  </p>
                  <strong className="mt-2 block text-lg font-bold">
                    {profileRuntime?.active_profile_id ?? "n/a"}
                  </strong>
                </div>
                <div className="rounded-[22px] bg-[var(--surface-strong)] p-4">
                  <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                    Active step
                  </p>
                  <strong className="mt-2 block text-lg font-bold">
                    {profileRuntime?.active_step_id ?? "n/a"}
                  </strong>
                  <p className="mt-2 text-sm text-[var(--muted)]">
                    Index {profileRuntime?.active_step_index ?? "n/a"}
                  </p>
                </div>
                <div className="rounded-[22px] bg-[var(--surface-strong)] p-4">
                  <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                    Phase
                  </p>
                  <strong className="mt-2 block text-lg font-bold capitalize">
                    {profilePhase}
                  </strong>
                  <p className="mt-2 text-sm text-[var(--muted)]">
                    {profileRuntime?.paused
                      ? "Profile timers are paused."
                      : profileRuntime?.waiting_for_manual_release
                        ? "Waiting for operator release."
                        : "Progress owned by firmware."}
                  </p>
                </div>
              </div>
            </CardContent>
          </Card>
        </section>

        <section className="grid gap-4 lg:grid-cols-2 xl:grid-cols-4">
          <Card>
            <CardHeader>
              <h2 className="text-xl font-bold tracking-[-0.04em]">Sensors</h2>
            </CardHeader>
            <CardContent className="grid gap-3 text-sm">
              <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                  Beer probe
                </p>
                <strong className="mt-2 block text-base font-bold capitalize">
                  {live.beer_probe_status ?? "unknown"}
                </strong>
                <p className="mt-2 text-[var(--muted)]">
                  {live.beer_probe_rom ?? "ROM n/a"}
                </p>
              </div>
              <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                  Chamber probe
                </p>
                <strong className="mt-2 block text-base font-bold capitalize">
                  {live.chamber_probe_status ?? "unknown"}
                </strong>
                <p className="mt-2 text-[var(--muted)]">
                  {live.chamber_probe_rom ?? "ROM n/a"}
                </p>
              </div>
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <h2 className="text-xl font-bold tracking-[-0.04em]">Outputs</h2>
            </CardHeader>
            <CardContent className="grid gap-3 text-sm">
              <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                  Heating
                </p>
                <strong className="mt-2 block text-base font-bold capitalize">
                  {live.heating_state}
                </strong>
              </div>
              <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                  Cooling
                </p>
                <strong className="mt-2 block text-base font-bold capitalize">
                  {live.cooling_state}
                </strong>
              </div>
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <h2 className="text-xl font-bold tracking-[-0.04em]">System</h2>
            </CardHeader>
            <CardContent className="grid gap-3 text-sm">
              <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                  MQTT
                </p>
                <strong className="mt-2 block text-base font-bold">
                  {live.mqtt_connected ? "Connected" : "Disconnected"}
                </strong>
              </div>
              <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                  Last heartbeat
                </p>
                <strong className="mt-2 block text-base font-bold">
                  {live.last_heartbeat_label}
                </strong>
              </div>
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <h2 className="text-xl font-bold tracking-[-0.04em]">Apply state</h2>
            </CardHeader>
            <CardContent className="grid gap-3 text-sm">
              <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                  Desired version
                </p>
                <strong className="mt-2 block text-base font-bold">
                  {live.fermentation_config?.desired_version ?? "n/a"}
                </strong>
              </div>
              <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                  Last apply
                </p>
                <strong className="mt-2 block text-base font-bold">
                  {live.fermentation_config?.last_applied_result ?? "pending"}
                </strong>
                <p className="mt-2 text-[var(--muted)]">
                  {live.fermentation_config?.last_applied_message ??
                    live.fermentation_config?.last_applied_at_label ??
                    "No apply feedback yet."}
                </p>
              </div>
            </CardContent>
          </Card>
        </section>

        {confirmationCopy ? (
          <div className="fixed inset-0 z-50 flex items-center justify-center bg-[#1f272466] px-4 backdrop-blur-sm">
            <button
              type="button"
              aria-label="Close confirmation"
              className="absolute inset-0"
              onClick={() => {
                if (!isConfirmPending) {
                  setConfirmAction(null);
                }
              }}
            />
            <div
              role="dialog"
              aria-modal="true"
              aria-labelledby="device-confirm-title"
              className="relative w-full max-w-md rounded-[28px] border border-black/8 bg-[var(--surface)] p-6 shadow-[0_24px_70px_rgba(26,34,31,0.2)]"
            >
              <div className="flex items-start gap-3">
                <div className="flex size-11 shrink-0 items-center justify-center rounded-full bg-[var(--surface-strong)] text-[var(--accent)]">
                  <CircleHelp className="size-5" />
                </div>
                <div className="space-y-3">
                  <div>
                    <h2
                      id="device-confirm-title"
                      className="text-2xl font-bold tracking-[-0.04em]"
                    >
                      {confirmationCopy.title}
                    </h2>
                    <p className="mt-2 text-sm leading-6 text-[var(--muted)]">
                      {confirmationCopy.body}
                    </p>
                  </div>
                  <div className="flex flex-col gap-2 sm:flex-row sm:justify-end">
                    <Button
                      variant="secondary"
                      onClick={() => setConfirmAction(null)}
                      disabled={isConfirmPending}
                    >
                      Cancel
                    </Button>
                    <Button
                      variant={confirmationCopy.confirmTone}
                      onClick={runConfirmedAction}
                      disabled={isConfirmPending}
                    >
                      {confirmationCopy.confirmLabel}
                    </Button>
                  </div>
                </div>
              </div>
            </div>
          </div>
        ) : null}

        {isDiagnosticsOpen ? (
          <div className="fixed inset-0 z-50 flex items-center justify-center bg-[#1f272466] px-4 py-6 backdrop-blur-sm">
            <button
              type="button"
              aria-label="Close diagnostics"
              className="absolute inset-0"
              onClick={() => setIsDiagnosticsOpen(false)}
            />
            <div
              role="dialog"
              aria-modal="true"
              aria-labelledby="device-diagnostics-title"
              className="relative flex max-h-[min(90vh,920px)] w-full max-w-5xl flex-col overflow-hidden rounded-[28px] border border-black/8 bg-[var(--surface)] shadow-[0_24px_70px_rgba(26,34,31,0.2)]"
            >
              <div className="border-b border-black/8 px-6 py-5">
                <div className="flex items-start justify-between gap-4">
                  <div>
                    <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                      Diagnostics
                    </p>
                    <h2
                      id="device-diagnostics-title"
                      className="mt-2 text-2xl font-bold tracking-[-0.04em]"
                    >
                      Controller diagnostics for {deviceId}
                    </h2>
                    <p className="mt-2 max-w-2xl text-sm leading-6 text-[var(--muted)]">
                      Low-level runtime and connectivity data for troubleshooting,
                      plus the latest raw payload from the device.
                    </p>
                  </div>
                  <Button
                    variant="secondary"
                    onClick={() => setIsDiagnosticsOpen(false)}
                  >
                    Close
                  </Button>
                </div>
              </div>

              <div className="grid gap-6 overflow-y-auto px-6 py-6">
                <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-3">
                  <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                    <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                      Controller state
                    </p>
                    <strong className="mt-2 block text-lg font-bold">
                      {live.controller_state ?? "unknown"}
                    </strong>
                  </div>
                  <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                    <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                      Controller reason
                    </p>
                    <strong className="mt-2 block text-lg font-bold">
                      {live.fault ?? live.controller_reason ?? "n/a"}
                    </strong>
                  </div>
                  <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                    <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                      Last heartbeat
                    </p>
                    <strong className="mt-2 block text-lg font-bold">
                      {live.last_heartbeat_label}
                    </strong>
                  </div>
                  <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                    <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                      MQTT
                    </p>
                    <strong className="mt-2 block text-lg font-bold">
                      {live.mqtt_connected ? "Connected" : "Disconnected"}
                    </strong>
                  </div>
                  <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                    <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                      RSSI
                    </p>
                    <strong className="mt-2 block text-lg font-bold">
                      {typeof live.last_rssi === "number" ? live.last_rssi : "n/a"}
                    </strong>
                  </div>
                  <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                    <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                      Heap free
                    </p>
                    <strong className="mt-2 block text-lg font-bold">
                      {typeof live.last_heap_free === "number"
                        ? live.last_heap_free
                        : "n/a"}
                    </strong>
                  </div>
                </div>

                <section>
                  <p className="text-xs font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                    Raw state
                  </p>
                  <pre className="mt-3 overflow-x-auto rounded-[24px] bg-[#1d2422] p-5 font-mono text-[13px] leading-6 text-[#f6f4ef]">
                    {lastPayload}
                  </pre>
                </section>
              </div>
            </div>
          </div>
        ) : null}

        {isProfileChooserOpen ? (
          <div className="fixed inset-0 z-50 flex items-center justify-center bg-[#1f272466] px-4 py-6 backdrop-blur-sm">
            <button
              type="button"
              aria-label="Close profile chooser"
              className="absolute inset-0"
              onClick={() => {
                if (!isProfileChooserPending) {
                  setIsProfileChooserOpen(false);
                  setProfileChooserError(null);
                }
              }}
            />
            <div
              role="dialog"
              aria-modal="true"
              aria-labelledby="profile-chooser-title"
              className="relative flex max-h-[min(90vh,920px)] w-full max-w-6xl flex-col overflow-hidden rounded-[28px] border border-black/8 bg-[var(--surface)] shadow-[0_24px_70px_rgba(26,34,31,0.2)]"
            >
              <div className="border-b border-black/8 px-6 py-5">
                <div className="flex items-start justify-between gap-4">
                  <div>
                    <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                      Profile library
                    </p>
                    <h2
                      id="profile-chooser-title"
                      className="mt-2 text-2xl font-bold tracking-[-0.04em]"
                    >
                      Choose profile for {deviceId}
                    </h2>
                    <p className="mt-2 max-w-2xl text-sm leading-6 text-[var(--muted)]">
                      Applying a library profile copies its steps into this device's
                      retained fermentation plan and switches the mode to profile.
                    </p>
                  </div>
                  <div className="flex flex-wrap gap-2">
                    <a
                      className="inline-flex min-h-11 items-center justify-center gap-2 rounded-full bg-white/80 px-4 text-sm font-semibold text-[var(--ink)] ring-1 ring-black/8 transition hover:bg-white"
                      href="/profiles"
                    >
                      Open profile manager
                    </a>
                    <Button
                      variant="secondary"
                      onClick={() => {
                        setIsProfileChooserOpen(false);
                        setProfileChooserError(null);
                      }}
                      disabled={isProfileChooserPending}
                    >
                      Close
                    </Button>
                  </div>
                </div>
              </div>

              <div className="grid gap-6 overflow-y-auto px-6 py-6 xl:grid-cols-[320px_1fr]">
                <div className="grid gap-3">
                  {profileLibraryQuery.isLoading ? (
                    <div className="rounded-[22px] bg-[var(--surface-strong)] p-4 text-sm text-[var(--muted)]">
                      Loading profiles...
                    </div>
                  ) : libraryProfiles.length ? (
                    libraryProfiles.map((profile) => (
                      <button
                        key={profile.slug}
                        type="button"
                        className={`grid gap-2 rounded-[22px] border px-4 py-4 text-left transition ${
                          profile.slug === selectedLibraryProfileSlug
                            ? "border-[var(--accent)] bg-[color:color-mix(in_srgb,var(--accent)_8%,white)]"
                            : "border-black/8 bg-white/72 hover:bg-white"
                        }`}
                        onClick={() => setSelectedLibraryProfileSlug(profile.slug)}
                      >
                        <div className="flex items-start justify-between gap-2">
                          <strong className="text-base font-bold">{profile.name}</strong>
                          <Badge
                            tone={
                              profile.source === "builtin"
                                ? "online"
                                : profile.source === "beerxml"
                                  ? "cooling"
                                  : "neutral"
                            }
                          >
                            {profile.source}
                          </Badge>
                        </div>
                        <p className="text-sm text-[var(--muted)]">{profile.slug}</p>
                        <p className="text-xs font-semibold uppercase tracking-[0.14em] text-[var(--muted)]">
                          {profile.step_count} steps · {formatDurationCompact(profile.total_duration_s)}
                        </p>
                      </button>
                    ))
                  ) : (
                    <div className="rounded-[22px] bg-[var(--surface-strong)] p-4 text-sm leading-6 text-[var(--muted)]">
                      No reusable profiles are stored yet.
                    </div>
                  )}
                </div>

                <div className="grid gap-4">
                  {selectedLibraryProfileQuery.isLoading ? (
                    <div className="rounded-[24px] bg-[var(--surface-strong)] p-6 text-sm text-[var(--muted)]">
                      Loading selected profile...
                    </div>
                  ) : selectedLibraryProfile ? (
                    <>
                      <div className="grid gap-3 md:grid-cols-3">
                        <div className="rounded-[22px] bg-[var(--surface-strong)] p-4">
                          <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                            Name
                          </p>
                          <strong className="mt-2 block text-xl font-bold">
                            {selectedLibraryProfile.name}
                          </strong>
                        </div>
                        <div className="rounded-[22px] bg-[var(--surface-strong)] p-4">
                          <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                            Steps
                          </p>
                          <strong className="mt-2 block text-xl font-bold">
                            {selectedLibraryProfile.steps.length}
                          </strong>
                        </div>
                        <div className="rounded-[22px] bg-[var(--surface-strong)] p-4">
                          <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                            Source
                          </p>
                          <strong className="mt-2 block text-xl font-bold capitalize">
                            {selectedLibraryProfile.source}
                          </strong>
                        </div>
                      </div>

                      <div className="grid gap-4">
                        {selectedLibraryProfile.steps.map((step, index) => (
                          <div
                            key={step.id}
                            className="rounded-[22px] border border-black/8 bg-white/72 p-4"
                          >
                            <div className="flex flex-wrap items-start justify-between gap-3">
                              <div>
                                <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                                  Step {index + 1}
                                </p>
                                <strong className="mt-2 block text-lg font-bold">
                                  {step.label}
                                </strong>
                              </div>
                              <div className="flex flex-wrap gap-2">
                                <Badge tone="neutral">
                                  {formatTemperature(step.target_c)}
                                </Badge>
                                <Badge tone="neutral">
                                  {formatDurationCompact(step.hold_duration_s)}
                                </Badge>
                                {step.ramp ? (
                                  <Badge tone="neutral">
                                    Ramp {formatDurationCompact(step.ramp.duration_s)}
                                  </Badge>
                                ) : null}
                              </div>
                            </div>
                            <p className="mt-3 text-sm text-[var(--muted)]">
                              Advance:{" "}
                              <strong className="text-[var(--ink)]">
                                {step.advance_policy === "manual_release"
                                  ? "manual release"
                                  : "auto"}
                              </strong>
                            </p>
                          </div>
                        ))}
                      </div>
                    </>
                  ) : (
                    <div className="rounded-[24px] bg-[var(--surface-strong)] p-6 text-sm leading-6 text-[var(--muted)]">
                      Pick a profile from the library to preview it here before applying.
                    </div>
                  )}
                </div>
              </div>

              <div className="border-t border-black/8 px-6 py-4">
                {profileChooserError ? (
                  <p className="mb-3 rounded-[18px] bg-[color:color-mix(in_srgb,var(--fault)_10%,white)] px-4 py-3 text-sm text-[var(--fault)]">
                    {profileChooserError}
                  </p>
                ) : null}
                <div className="flex flex-col gap-2 sm:flex-row sm:justify-end">
                  <Button
                    variant="secondary"
                    onClick={() => {
                      setIsProfileChooserOpen(false);
                      setProfileChooserError(null);
                    }}
                    disabled={isProfileChooserPending}
                  >
                    Cancel
                  </Button>
                  <Button
                    variant="primary"
                    onClick={applySelectedLibraryProfile}
                    disabled={!selectedLibraryProfile || isProfileChooserPending}
                  >
                    Apply to device
                  </Button>
                </div>
              </div>
            </div>
          </div>
        ) : null}

        {isFermentationSetupOpen && fermentationSetupDraft ? (
          <div className="fixed inset-0 z-50 flex items-center justify-center bg-[#1f272466] px-4 py-6 backdrop-blur-sm">
            <button
              type="button"
              aria-label="Close fermentation setup"
              className="absolute inset-0"
              onClick={() => {
                if (!isFermentationSetupPending) {
                  setIsFermentationSetupOpen(false);
                  setFermentationSetupError(null);
                }
              }}
            />
            <div
              role="dialog"
              aria-modal="true"
              aria-labelledby="fermentation-setup-title"
              className="relative flex w-full max-w-2xl flex-col overflow-hidden rounded-[28px] border border-black/8 bg-[var(--surface)] shadow-[0_24px_70px_rgba(26,34,31,0.2)]"
            >
              <div className="border-b border-black/8 px-6 py-5">
                <div className="flex items-start justify-between gap-4">
                  <div>
                    <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                      Fermentation setup
                    </p>
                    <h2
                      id="fermentation-setup-title"
                      className="mt-2 text-2xl font-bold tracking-[-0.04em]"
                    >
                      Retained plan identity for {deviceId}
                    </h2>
                    <p className="mt-2 max-w-2xl text-sm leading-6 text-[var(--muted)]">
                      Change the retained plan name and whether the device runs in
                      direct thermostat mode or profile mode.
                    </p>
                  </div>
                  <Button
                    variant="secondary"
                    onClick={() => {
                      setIsFermentationSetupOpen(false);
                      setFermentationSetupError(null);
                    }}
                    disabled={isFermentationSetupPending}
                  >
                    Cancel
                  </Button>
                </div>
              </div>

              <div className="grid gap-4 px-6 py-6">
                <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                  Fermentation name
                  <input
                    className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                    type="text"
                    value={fermentationSetupDraft.name}
                    onChange={(event) =>
                      setFermentationSetupDraft((current) =>
                        current
                          ? {
                              ...current,
                              name: event.target.value,
                            }
                          : current,
                      )
                    }
                  />
                </label>
                <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                  Control mode
                  <select
                    className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                    value={fermentationSetupDraft.mode}
                    onChange={(event) =>
                      setFermentationSetupDraft((current) =>
                        current
                          ? {
                              ...current,
                              mode: event.target.value as "thermostat" | "profile",
                            }
                          : current,
                      )
                    }
                  >
                    <option value="thermostat">Thermostat</option>
                    <option value="profile">Profile</option>
                  </select>
                </label>
                <div className="rounded-[20px] bg-[var(--surface-strong)] px-4 py-3 text-sm leading-6 text-[var(--muted)]">
                  Switching to <strong className="text-[var(--ink)]">profile</strong>{" "}
                  keeps or creates a retained profile plan. Switching to{" "}
                  <strong className="text-[var(--ink)]">thermostat</strong> makes the
                  direct target the active retained control mode.
                </div>
              </div>

              <div className="border-t border-black/8 px-6 py-4">
                {fermentationSetupError ? (
                  <p className="mb-3 rounded-[18px] bg-[color:color-mix(in_srgb,var(--fault)_10%,white)] px-4 py-3 text-sm text-[var(--fault)]">
                    {fermentationSetupError}
                  </p>
                ) : null}
                <div className="flex flex-col gap-2 sm:flex-row sm:justify-end">
                  <Button
                    variant="secondary"
                    onClick={() => {
                      setIsFermentationSetupOpen(false);
                      setFermentationSetupError(null);
                    }}
                    disabled={isFermentationSetupPending}
                  >
                    Cancel
                  </Button>
                  <Button
                    variant="primary"
                    onClick={saveFermentationSetup}
                    disabled={isFermentationSetupPending}
                  >
                    Save fermentation setup
                  </Button>
                </div>
              </div>
            </div>
          </div>
        ) : null}

        {isRoutingOpen ? (
          <div className="fixed inset-0 z-50 flex items-center justify-center bg-[#1f272466] px-4 py-6 backdrop-blur-sm">
            <button
              type="button"
              aria-label="Close output routing"
              className="absolute inset-0"
              onClick={() => {
                if (!isRoutingPending) {
                  setIsRoutingOpen(false);
                  setIsRoutingDirty(false);
                  setRoutingError(null);
                }
              }}
            />
            <div
              role="dialog"
              aria-modal="true"
              aria-labelledby="output-routing-title"
              className="relative flex max-h-[min(90vh,920px)] w-full max-w-4xl flex-col overflow-hidden rounded-[28px] border border-black/8 bg-[var(--surface)] shadow-[0_24px_70px_rgba(26,34,31,0.2)]"
            >
              <div className="border-b border-black/8 px-6 py-5">
                <div className="flex items-start justify-between gap-4">
                  <div>
                    <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                      Output routing
                    </p>
                    <h2
                      id="output-routing-title"
                      className="mt-2 text-2xl font-bold tracking-[-0.04em]"
                    >
                      Relay assignment for {deviceId}
                    </h2>
                    <p className="mt-2 max-w-2xl text-sm leading-6 text-[var(--muted)]">
                      Assign discovered Kasa relays, override with a manual host, or
                      remove routing for heating and cooling.
                    </p>
                  </div>
                  <div className="flex flex-wrap gap-2">
                    <Button
                      variant="subtle"
                      onClick={() => scanRelaysMutation.mutate()}
                      disabled={isRoutingPending}
                    >
                      Scan Kasa
                    </Button>
                    <Button
                      variant="secondary"
                      onClick={() => {
                        setIsRoutingOpen(false);
                        setIsRoutingDirty(false);
                        setRoutingError(null);
                      }}
                      disabled={isRoutingPending}
                    >
                      Cancel
                    </Button>
                  </div>
                </div>
              </div>

              <div className="grid gap-6 overflow-y-auto px-6 py-6 md:grid-cols-2">
                {outputRoutingQuery.isError ? (
                  <div className="md:col-span-2 rounded-[20px] bg-[color:color-mix(in_srgb,var(--fault)_10%,white)] px-4 py-6 text-sm text-[var(--fault)]">
                    Failed to load relay assignments and discovered devices.
                  </div>
                ) : routingDraft ? (
                  <>
                    <section className="grid gap-4">
                      <p className="text-xs font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                        Heating output
                      </p>
                      <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                        Driver
                        <select
                          className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                          value={routingDraft.heating_driver}
                          onChange={(event) =>
                            updateRoutingField("heating_driver", event.target.value)
                          }
                        >
                          <option value="kasa_local">Kasa local</option>
                          <option value="shelly_http_rpc">Shelly HTTP RPC</option>
                        </select>
                      </label>
                      <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                        Discovered relay
                        <select
                          className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                          value={routingDraft.heating_selected_host}
                          onChange={(event) => {
                            const selectedRelay = (
                              outputRoutingQuery.data?.discovered_relays ?? []
                            ).find((relay) => relay.host === event.target.value);
                            updateRoutingField(
                              "heating_selected_host",
                              event.target.value,
                            );
                            updateRoutingField("heating_manual_host", "");
                            if (selectedRelay) {
                              updateRoutingField("heating_driver", selectedRelay.driver);
                            }
                          }}
                        >
                          <option value="">None</option>
                          {(outputRoutingQuery.data?.discovered_relays ?? []).map((relay) => (
                            <option key={`heating-${relay.host}`} value={relay.host}>
                              {(relay.alias || relay.host) +
                                (relay.model ? ` · ${relay.model}` : "")}
                            </option>
                          ))}
                        </select>
                      </label>
                      <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                        Manual host override
                        <input
                          className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                          type="text"
                          placeholder="192.168.1.50"
                          value={routingDraft.heating_manual_host}
                          onChange={(event) => {
                            updateRoutingField(
                              "heating_manual_host",
                              event.target.value,
                            );
                            if (event.target.value.trim()) {
                              updateRoutingField("heating_selected_host", "");
                            }
                          }}
                        />
                      </label>
                      <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                        Alias override
                        <input
                          className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                          type="text"
                          placeholder="Heating plug"
                          value={routingDraft.heating_alias}
                          onChange={(event) =>
                            updateRoutingField("heating_alias", event.target.value)
                          }
                        />
                      </label>
                    </section>

                    <section className="grid gap-4">
                      <p className="text-xs font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                        Cooling output
                      </p>
                      <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                        Driver
                        <select
                          className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                          value={routingDraft.cooling_driver}
                          onChange={(event) =>
                            updateRoutingField("cooling_driver", event.target.value)
                          }
                        >
                          <option value="kasa_local">Kasa local</option>
                          <option value="shelly_http_rpc">Shelly HTTP RPC</option>
                        </select>
                      </label>
                      <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                        Discovered relay
                        <select
                          className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                          value={routingDraft.cooling_selected_host}
                          onChange={(event) => {
                            const selectedRelay = (
                              outputRoutingQuery.data?.discovered_relays ?? []
                            ).find((relay) => relay.host === event.target.value);
                            updateRoutingField(
                              "cooling_selected_host",
                              event.target.value,
                            );
                            updateRoutingField("cooling_manual_host", "");
                            if (selectedRelay) {
                              updateRoutingField("cooling_driver", selectedRelay.driver);
                            }
                          }}
                        >
                          <option value="">None</option>
                          {(outputRoutingQuery.data?.discovered_relays ?? []).map((relay) => (
                            <option key={`cooling-${relay.host}`} value={relay.host}>
                              {(relay.alias || relay.host) +
                                (relay.model ? ` · ${relay.model}` : "")}
                            </option>
                          ))}
                        </select>
                      </label>
                      <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                        Manual host override
                        <input
                          className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                          type="text"
                          placeholder="192.168.1.51"
                          value={routingDraft.cooling_manual_host}
                          onChange={(event) => {
                            updateRoutingField(
                              "cooling_manual_host",
                              event.target.value,
                            );
                            if (event.target.value.trim()) {
                              updateRoutingField("cooling_selected_host", "");
                            }
                          }}
                        />
                      </label>
                      <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                        Alias override
                        <input
                          className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                          type="text"
                          placeholder="Cooling plug"
                          value={routingDraft.cooling_alias}
                          onChange={(event) =>
                            updateRoutingField("cooling_alias", event.target.value)
                          }
                        />
                      </label>
                    </section>

                    <section className="md:col-span-2">
                      <p className="text-xs font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                        Known relays
                      </p>
                      <div className="mt-3 grid gap-3 md:grid-cols-2">
                        {(outputRoutingQuery.data?.discovered_relays ?? []).length > 0 ? (
                          (outputRoutingQuery.data?.discovered_relays ?? []).map((relay) => (
                            <div
                              key={relay.host}
                              className="rounded-[20px] bg-[var(--surface-strong)] px-4 py-3 text-sm"
                            >
                              <strong className="block text-base font-bold text-[var(--ink)]">
                                {relay.alias || relay.host}
                              </strong>
                              <p className="mt-1 text-[var(--muted)]">
                                {relay.host}
                                {relay.model ? ` · ${relay.model}` : ""}
                              </p>
                              <p className="mt-1 text-[var(--muted)]">
                                {relay.is_on === true
                                  ? "On"
                                  : relay.is_on === false
                                    ? "Off"
                                    : "Unknown"}{" "}
                                · seen {relay.last_seen_label}
                              </p>
                            </div>
                          ))
                        ) : (
                          <div className="rounded-[20px] bg-[var(--surface-strong)] px-4 py-3 text-sm text-[var(--muted)]">
                            No Kasa devices discovered yet. Run a scan from this
                            modal.
                          </div>
                        )}
                      </div>
                    </section>
                  </>
                ) : (
                  <div className="md:col-span-2 rounded-[20px] bg-[var(--surface-strong)] px-4 py-6 text-sm text-[var(--muted)]">
                    Loading relay assignments and discovered devices.
                  </div>
                )}
              </div>

              <div className="border-t border-black/8 px-6 py-4">
                {routingError ? (
                  <p className="mb-3 rounded-[18px] bg-[color:color-mix(in_srgb,var(--fault)_10%,white)] px-4 py-3 text-sm text-[var(--fault)]">
                    {routingError}
                  </p>
                ) : null}
                <div className="flex flex-col gap-2 sm:flex-row sm:justify-end">
                  <Button
                    variant="secondary"
                    onClick={() => {
                      setIsRoutingOpen(false);
                      setIsRoutingDirty(false);
                      setRoutingError(null);
                    }}
                    disabled={isRoutingPending}
                  >
                    Cancel
                  </Button>
                  <Button
                    variant="primary"
                    onClick={saveRouting}
                    disabled={isRoutingPending || !routingDraft}
                  >
                    Save output routing
                  </Button>
                </div>
              </div>
            </div>
          </div>
        ) : null}

        {isSettingsOpen && settingsDraft ? (
          <div className="fixed inset-0 z-50 flex items-center justify-center bg-[#1f272466] px-4 py-6 backdrop-blur-sm">
            <button
              type="button"
              aria-label="Close device settings"
              className="absolute inset-0"
              onClick={() => {
                if (!isSettingsPending) {
                  setIsSettingsOpen(false);
                  setSettingsError(null);
                }
              }}
            />
            <div
              role="dialog"
              aria-modal="true"
              aria-labelledby="device-settings-title"
              className="relative flex max-h-[min(90vh,860px)] w-full max-w-3xl flex-col overflow-hidden rounded-[28px] border border-black/8 bg-[var(--surface)] shadow-[0_24px_70px_rgba(26,34,31,0.2)]"
            >
              <div className="border-b border-black/8 px-6 py-5">
                <div className="flex items-start justify-between gap-4">
                  <div>
                    <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                      Device settings
                    </p>
                    <h2
                      id="device-settings-title"
                      className="mt-2 text-2xl font-bold tracking-[-0.04em]"
                    >
                      Controller tuning for {deviceId}
                    </h2>
                    <p className="mt-2 max-w-2xl text-sm leading-6 text-[var(--muted)]">
                      Update settings that are tied to the physical setup rather
                      than the fermentation schedule: hysteresis, output delays,
                      probe offsets and sensor safety thresholds.
                    </p>
                  </div>
                  <Button
                    variant="secondary"
                    onClick={() => {
                      setIsSettingsOpen(false);
                      setSettingsError(null);
                    }}
                    disabled={isSettingsPending}
                  >
                    Cancel
                  </Button>
                </div>
              </div>

              <div className="grid gap-6 overflow-y-auto px-6 py-6 md:grid-cols-2">
                <section>
                  <p className="text-xs font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                    Control
                  </p>
                  <div className="mt-3 grid gap-4">
                    <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                      Hysteresis (°C)
                      <input
                        className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                        type="number"
                        min="0.1"
                        max="5"
                        step="0.1"
                        value={settingsDraft.hysteresis_c}
                        onChange={(event) =>
                          updateSettingsField("hysteresis_c", event.target.value)
                        }
                      />
                    </label>
                    <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                      Cooling delay (s)
                      <input
                        className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                        type="number"
                        min="0"
                        max="3600"
                        step="1"
                        value={settingsDraft.cooling_delay_s}
                        onChange={(event) =>
                          updateSettingsField("cooling_delay_s", event.target.value)
                        }
                      />
                    </label>
                    <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                      Heating delay (s)
                      <input
                        className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                        type="number"
                        min="0"
                        max="3600"
                        step="1"
                        value={settingsDraft.heating_delay_s}
                        onChange={(event) =>
                          updateSettingsField("heating_delay_s", event.target.value)
                        }
                      />
                    </label>
                    <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                      Deviation alarm (°C)
                      <input
                        className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                        type="number"
                        min="0"
                        max="25"
                        step="0.1"
                        value={settingsDraft.deviation_c}
                        onChange={(event) =>
                          updateSettingsField("deviation_c", event.target.value)
                        }
                      />
                    </label>
                    <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                      Sensor stale timeout (s)
                      <input
                        className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                        type="number"
                        min="1"
                        max="600"
                        step="1"
                        value={settingsDraft.sensor_stale_s}
                        onChange={(event) =>
                          updateSettingsField("sensor_stale_s", event.target.value)
                        }
                      />
                    </label>
                  </div>
                </section>

                <section>
                  <p className="text-xs font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                    Sensors
                  </p>
                  <div className="mt-3 grid gap-4">
                    <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                      Primary probe offset (°C)
                      <input
                        className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                        type="number"
                        min="-5"
                        max="5"
                        step="0.1"
                        value={settingsDraft.primary_offset_c}
                        onChange={(event) =>
                          updateSettingsField("primary_offset_c", event.target.value)
                        }
                      />
                    </label>
                    <label className="flex items-center gap-3 rounded-[18px] border border-black/8 bg-white/60 px-4 py-3 text-sm font-semibold text-[var(--ink)]">
                      <input
                        className="size-4 accent-[var(--accent)]"
                        type="checkbox"
                        checked={settingsDraft.secondary_enabled}
                        onChange={(event) =>
                          updateSettingsField(
                            "secondary_enabled",
                            event.target.checked,
                          )
                        }
                      />
                      Enable secondary chamber probe
                    </label>
                    <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                      Control sensor
                      <select
                        className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)] disabled:cursor-not-allowed disabled:opacity-60"
                        value={settingsDraft.control_sensor}
                        onChange={(event) =>
                          updateSettingsField("control_sensor", event.target.value)
                        }
                        disabled={!settingsDraft.secondary_enabled}
                      >
                        <option value="primary">Primary (beer)</option>
                        <option value="secondary">Secondary (chamber)</option>
                      </select>
                    </label>
                    <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                      Secondary probe offset (°C)
                      <input
                        className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)] disabled:cursor-not-allowed disabled:opacity-60"
                        type="number"
                        min="-5"
                        max="5"
                        step="0.1"
                        value={settingsDraft.secondary_offset_c}
                        onChange={(event) =>
                          updateSettingsField(
                            "secondary_offset_c",
                            event.target.value,
                          )
                        }
                        disabled={!settingsDraft.secondary_enabled}
                      />
                    </label>
                    <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                      Secondary limit hysteresis (°C)
                      <input
                        className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)] disabled:cursor-not-allowed disabled:opacity-60"
                        type="number"
                        min="0.1"
                        max="25"
                        step="0.1"
                        value={settingsDraft.secondary_limit_hysteresis_c}
                        onChange={(event) =>
                          updateSettingsField(
                            "secondary_limit_hysteresis_c",
                            event.target.value,
                          )
                        }
                        disabled={!settingsDraft.secondary_enabled}
                      />
                    </label>
                  </div>
                </section>
              </div>

              <div className="border-t border-black/8 px-6 py-4">
                {settingsError ? (
                  <p className="mb-3 rounded-[18px] bg-[color:color-mix(in_srgb,var(--fault)_10%,white)] px-4 py-3 text-sm text-[var(--fault)]">
                    {settingsError}
                  </p>
                ) : null}
                <div className="flex flex-col gap-2 sm:flex-row sm:justify-end">
                  <Button
                    variant="secondary"
                    onClick={() => {
                      setIsSettingsOpen(false);
                      setSettingsError(null);
                    }}
                    disabled={isSettingsPending}
                  >
                    Cancel
                  </Button>
                  <Button
                    variant="primary"
                    onClick={saveSettings}
                    disabled={isSettingsPending}
                  >
                    Save device settings
                  </Button>
                </div>
              </div>
            </div>
          </div>
        ) : null}
      </main>
    </div>
  );
}
