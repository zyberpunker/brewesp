import type {
  FermentationPlan,
  LivePayload,
  OutputRoutingPayload,
  TelemetryPoint,
  TelemetryWindow,
} from "@/device-detail/types";

async function readJson<T>(response: Response): Promise<T> {
  if (!response.ok) {
    throw new Error(`Request failed: ${response.status}`);
  }

  return (await response.json()) as T;
}

export async function fetchLiveState(deviceId: string): Promise<LivePayload> {
  const response = await fetch(`/api/devices/${deviceId}/live`, {
    headers: { Accept: "application/json" },
  });

  return readJson<LivePayload>(response);
}

export async function fetchTelemetry(
  deviceId: string,
  window: TelemetryWindow = "12h",
): Promise<TelemetryPoint[]> {
  const params = new URLSearchParams();

  if (window === "all") {
    params.set("all", "true");
    params.set("limit", "20000");
  } else {
    params.set(
      "hours",
      window === "2h" ? "2" : window === "12h" ? "12" : "48",
    );
  }

  const response = await fetch(`/api/devices/${deviceId}/telemetry?${params.toString()}`, {
    headers: { Accept: "application/json" },
  });

  return readJson<TelemetryPoint[]>(response);
}

export async function fetchFermentationPlan(
  deviceId: string,
): Promise<FermentationPlan> {
  const response = await fetch(`/api/devices/${deviceId}/fermentation-plan`, {
    headers: { Accept: "application/json" },
  });

  return readJson<FermentationPlan>(response);
}

export async function updateFermentationPlan(
  deviceId: string,
  payload: FermentationPlan,
): Promise<FermentationPlan> {
  const response = await fetch(`/api/devices/${deviceId}/fermentation-plan`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify(payload),
  });

  return readJson<FermentationPlan>(response);
}

export async function fetchOutputRouting(
  deviceId: string,
): Promise<OutputRoutingPayload> {
  const response = await fetch(`/api/devices/${deviceId}/output-routing`, {
    headers: { Accept: "application/json" },
  });

  return readJson<OutputRoutingPayload>(response);
}

export async function updateOutputRouting(
  deviceId: string,
  payload: {
    heating: { host: string; alias?: string };
    cooling: { host: string; alias?: string };
  },
): Promise<OutputRoutingPayload> {
  const response = await fetch(`/api/devices/${deviceId}/output-routing`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify(payload),
  });

  return readJson<OutputRoutingPayload>(response);
}

export async function scanKasaRelays(
  deviceId: string,
): Promise<Record<string, string>> {
  const response = await fetch(`/api/devices/${deviceId}/output-routing/discover`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify({}),
  });

  return readJson<Record<string, string>>(response);
}

export async function sendOutputCommand(
  deviceId: string,
  action:
    | "heating_on"
    | "heating_off"
    | "cooling_on"
    | "cooling_off"
    | "all_off",
) {
  const response = await fetch(`/api/devices/${deviceId}/commands/output`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify({ action }),
  });

  return readJson<Record<string, string>>(response);
}

export async function sendProfileCommand(
  deviceId: string,
  command:
    | "profile_pause"
    | "profile_resume"
    | "profile_release_hold"
    | "profile_stop",
) {
  const response = await fetch(`/api/devices/${deviceId}/commands/profile`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify({ command }),
  });

  return readJson<Record<string, string>>(response);
}
