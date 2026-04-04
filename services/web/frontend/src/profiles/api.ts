import type { ProfileDetail, ProfileSummary } from "@/profiles/types";

async function readJson<T>(response: Response): Promise<T> {
  if (!response.ok) {
    let message = `Request failed: ${response.status}`;
    try {
      const payload = (await response.json()) as { error?: string };
      if (payload.error) {
        message = payload.error;
      }
    } catch {
      // Ignore non-JSON error responses and keep the generic message.
    }
    throw new Error(message);
  }

  return (await response.json()) as T;
}

export async function fetchProfiles(): Promise<ProfileSummary[]> {
  const response = await fetch("/api/fermentation-profiles", {
    headers: { Accept: "application/json" },
  });
  return readJson<ProfileSummary[]>(response);
}

export async function fetchProfileDetail(slug: string): Promise<ProfileDetail> {
  const response = await fetch(`/api/fermentation-profiles/${slug}`, {
    headers: { Accept: "application/json" },
  });
  return readJson<ProfileDetail>(response);
}

export async function createProfile(payload: {
  name: string;
  steps: Array<{
    label: string;
    target_c: number;
    hold_duration_s: number;
    advance_policy: "auto" | "manual_release";
    ramp?: {
      mode: "time";
      duration_s: number;
    };
  }>;
}): Promise<ProfileDetail> {
  const response = await fetch("/api/fermentation-profiles", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify(payload),
  });
  return readJson<ProfileDetail>(response);
}

export async function updateProfile(
  slug: string,
  payload: {
    name: string;
    steps: Array<{
      label: string;
      target_c: number;
      hold_duration_s: number;
      advance_policy: "auto" | "manual_release";
      ramp?: {
        mode: "time";
        duration_s: number;
      };
    }>;
  },
): Promise<ProfileDetail> {
  const response = await fetch(`/api/fermentation-profiles/${slug}`, {
    method: "PUT",
    headers: {
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify(payload),
  });
  return readJson<ProfileDetail>(response);
}

export async function deleteProfile(slug: string): Promise<void> {
  const response = await fetch(`/api/fermentation-profiles/${slug}`, {
    method: "DELETE",
    headers: {
      Accept: "application/json",
    },
  });
  await readJson<{ status: string }>(response);
}

export async function importBeerXml(file: File): Promise<ProfileDetail> {
  const body = new FormData();
  body.append("file", file);
  const response = await fetch("/api/fermentation-profiles/import/beerxml", {
    method: "POST",
    body,
  });
  return readJson<ProfileDetail>(response);
}
