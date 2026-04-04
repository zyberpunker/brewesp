import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import {
  ArrowDown,
  ArrowUp,
  ChevronRight,
  FileUp,
  Plus,
  Save,
  Search,
  Thermometer,
  Trash2,
} from "lucide-react";
import {
  startTransition,
  type ChangeEvent,
  useDeferredValue,
  useEffect,
  useRef,
  useState,
} from "react";

import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader } from "@/components/ui/card";
import {
  createProfile,
  deleteProfile,
  fetchProfileDetail,
  fetchProfiles,
  importBeerXml,
  updateProfile,
} from "@/profiles/api";
import type { ProfileDetail, ProfileStep, ProfileSummary } from "@/profiles/types";

const MAX_STEPS = 7;

type DurationUnit = "days" | "hours" | "minutes";

type StepDraft = {
  id: string;
  label: string;
  target_c: string;
  hold_value: string;
  hold_unit: DurationUnit;
  advance_policy: "auto" | "manual_release";
  ramp_enabled: boolean;
  ramp_value: string;
  ramp_unit: DurationUnit;
};

type ProfileDraft = {
  name: string;
  steps: StepDraft[];
};

function createDraftStepId() {
  return `draft-${Math.random().toString(36).slice(2, 10)}`;
}

function durationUnitFromSeconds(seconds: number): {
  value: string;
  unit: DurationUnit;
} {
  if (seconds % 86400 === 0) {
    return { value: String(seconds / 86400), unit: "days" };
  }
  if (seconds % 3600 === 0) {
    return { value: String(seconds / 3600), unit: "hours" };
  }
  return { value: String(seconds / 60), unit: "minutes" };
}

function durationSeconds(value: string, unit: DurationUnit, fieldLabel: string) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    return { seconds: null, error: `${fieldLabel} must be numeric.` };
  }
  if (parsed < 0) {
    return { seconds: null, error: `${fieldLabel} cannot be negative.` };
  }

  const multiplier = unit === "days" ? 86400 : unit === "hours" ? 3600 : 60;
  return {
    seconds: Math.round(parsed * multiplier),
    error: null,
  };
}

function formatDurationCompact(totalSeconds: number) {
  if (totalSeconds % 86400 === 0) {
    const days = totalSeconds / 86400;
    return `${days} d`;
  }
  if (totalSeconds % 3600 === 0) {
    const hours = totalSeconds / 3600;
    return `${hours} h`;
  }
  return `${Math.round(totalSeconds / 60)} min`;
}

function formatProfileTimestamp(value: string | null) {
  if (!value) {
    return "unknown";
  }
  return new Date(value).toLocaleString();
}

function buildStepDraft(step: ProfileStep): StepDraft {
  const hold = durationUnitFromSeconds(step.hold_duration_s);
  const rampSeconds = step.ramp?.duration_s ?? 0;
  const defaultRamp = durationUnitFromSeconds(rampSeconds || 3600);
  const actualRamp = rampSeconds > 0 ? durationUnitFromSeconds(rampSeconds) : defaultRamp;
  return {
    id: step.id,
    label: step.label,
    target_c: step.target_c.toString(),
    hold_value: hold.value,
    hold_unit: hold.unit,
    advance_policy: step.advance_policy,
    ramp_enabled: rampSeconds > 0,
    ramp_value: actualRamp.value,
    ramp_unit: actualRamp.unit,
  };
}

function buildDraftFromProfile(profile: ProfileDetail): ProfileDraft {
  return {
    name: profile.name,
    steps: profile.steps.map(buildStepDraft),
  };
}

function createEmptyProfileDraft(): ProfileDraft {
  return {
    name: "New fermentation profile",
    steps: [
      {
        id: createDraftStepId(),
        label: "Step 1",
        target_c: "20",
        hold_value: "7",
        hold_unit: "days",
        advance_policy: "auto",
        ramp_enabled: false,
        ramp_value: "12",
        ramp_unit: "hours",
      },
    ],
  };
}

function buildPayloadFromDraft(draft: ProfileDraft) {
  const name = draft.name.trim();
  if (!name) {
    return { payload: null, error: "Profile name is required." };
  }
  if (draft.steps.length < 1) {
    return { payload: null, error: "Profile must include at least one step." };
  }
  if (draft.steps.length > MAX_STEPS) {
    return { payload: null, error: `Profile can include at most ${MAX_STEPS} steps.` };
  }

  const steps = [];
  for (let index = 0; index < draft.steps.length; index += 1) {
    const step = draft.steps[index];
    const label = step.label.trim() || `Step ${index + 1}`;
    const target_c = Number(step.target_c);
    if (!Number.isFinite(target_c)) {
      return { payload: null, error: `Step ${index + 1} temperature must be numeric.` };
    }

    const hold = durationSeconds(step.hold_value, step.hold_unit, `Step ${index + 1} hold`);
    if (hold.error || hold.seconds == null) {
      return { payload: null, error: hold.error };
    }

    let ramp:
      | {
          mode: "time";
          duration_s: number;
        }
      | undefined;
    if (step.ramp_enabled) {
      const parsedRamp = durationSeconds(
        step.ramp_value,
        step.ramp_unit,
        `Step ${index + 1} ramp`,
      );
      if (parsedRamp.error || parsedRamp.seconds == null) {
        return { payload: null, error: parsedRamp.error };
      }
      if (parsedRamp.seconds > 0) {
        ramp = {
          mode: "time",
          duration_s: parsedRamp.seconds,
        };
      }
    }

    steps.push({
      label,
      target_c,
      hold_duration_s: hold.seconds,
      advance_policy: step.advance_policy,
      ...(ramp ? { ramp } : {}),
    });
  }

  return {
    payload: {
      name,
      steps,
    },
    error: null,
  };
}

function sourceTone(source: string) {
  if (source === "builtin") {
    return "online" as const;
  }
  if (source === "beerxml") {
    return "cooling" as const;
  }
  return "neutral" as const;
}

export function ProfilesApp() {
  const queryClient = useQueryClient();
  const fileInputRef = useRef<HTMLInputElement | null>(null);
  const [selectedSlug, setSelectedSlug] = useState<string | null>(null);
  const [isCreatingNew, setIsCreatingNew] = useState(false);
  const [draft, setDraft] = useState<ProfileDraft | null>(null);
  const [isDirty, setIsDirty] = useState(false);
  const [editorError, setEditorError] = useState<string | null>(null);
  const [search, setSearch] = useState("");
  const deferredSearch = useDeferredValue(search);

  const profilesQuery = useQuery({
    queryKey: ["fermentation-profiles"],
    queryFn: fetchProfiles,
  });

  const selectedProfileQuery = useQuery({
    queryKey: ["fermentation-profile", selectedSlug],
    queryFn: () => fetchProfileDetail(selectedSlug ?? ""),
    enabled: Boolean(selectedSlug) && !isCreatingNew,
  });

  const saveMutation = useMutation({
    mutationFn: async (nextDraft: ProfileDraft) => {
      const built = buildPayloadFromDraft(nextDraft);
      if (built.error || !built.payload) {
        throw new Error(built.error ?? "Failed to build the profile payload.");
      }
      if (isCreatingNew) {
        return createProfile(built.payload);
      }
      if (!selectedSlug) {
        throw new Error("No profile selected.");
      }
      return updateProfile(selectedSlug, built.payload);
    },
    onSuccess: async (profile) => {
      await Promise.all([
        queryClient.invalidateQueries({ queryKey: ["fermentation-profiles"] }),
        queryClient.invalidateQueries({
          queryKey: ["fermentation-profile", profile.slug],
        }),
      ]);
      startTransition(() => {
        setSelectedSlug(profile.slug);
        setIsCreatingNew(false);
        setDraft(buildDraftFromProfile(profile));
        setIsDirty(false);
        setEditorError(null);
      });
    },
  });

  const deleteMutation = useMutation({
    mutationFn: (slug: string) => deleteProfile(slug),
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ["fermentation-profiles"] });
      startTransition(() => {
        setSelectedSlug(null);
        setDraft(null);
        setIsCreatingNew(false);
        setIsDirty(false);
        setEditorError(null);
      });
    },
  });

  const importMutation = useMutation({
    mutationFn: (file: File) => importBeerXml(file),
    onSuccess: async (profile) => {
      await Promise.all([
        queryClient.invalidateQueries({ queryKey: ["fermentation-profiles"] }),
        queryClient.invalidateQueries({
          queryKey: ["fermentation-profile", profile.slug],
        }),
      ]);
      startTransition(() => {
        setSelectedSlug(profile.slug);
        setIsCreatingNew(false);
        setDraft(buildDraftFromProfile(profile));
        setIsDirty(false);
        setEditorError(null);
      });
    },
  });

  useEffect(() => {
    if (isCreatingNew || selectedSlug || !profilesQuery.data?.length) {
      return;
    }
    setSelectedSlug(profilesQuery.data[0].slug);
  }, [isCreatingNew, profilesQuery.data, selectedSlug]);

  useEffect(() => {
    if (!selectedProfileQuery.data || isCreatingNew || isDirty) {
      return;
    }
    setDraft(buildDraftFromProfile(selectedProfileQuery.data));
    setEditorError(null);
  }, [isCreatingNew, isDirty, selectedProfileQuery.data]);

  const profiles = profilesQuery.data ?? [];
  const normalizedSearch = deferredSearch.trim().toLowerCase();
  const filteredProfiles = profiles.filter((profile) => {
    if (!normalizedSearch) {
      return true;
    }
    return (
      profile.name.toLowerCase().includes(normalizedSearch) ||
      profile.slug.toLowerCase().includes(normalizedSearch)
    );
  });
  const selectedSummary =
    profiles.find((profile) => profile.slug === selectedSlug) ?? null;

  const openProfile = (profile: ProfileSummary) => {
    if (
      isDirty &&
      !window.confirm("Discard unsaved profile changes before switching?")
    ) {
      return;
    }

    startTransition(() => {
      setSelectedSlug(profile.slug);
      setIsCreatingNew(false);
      setIsDirty(false);
      setEditorError(null);
    });
  };

  const openNewDraft = () => {
    if (
      isDirty &&
      !window.confirm("Discard unsaved profile changes before creating a new profile?")
    ) {
      return;
    }

    startTransition(() => {
      setSelectedSlug(null);
      setIsCreatingNew(true);
      setDraft(createEmptyProfileDraft());
      setIsDirty(false);
      setEditorError(null);
    });
  };

  const updateDraft = (nextDraft: ProfileDraft) => {
    setDraft(nextDraft);
    setIsDirty(true);
  };

  const addStep = () => {
    if (!draft || draft.steps.length >= MAX_STEPS) {
      return;
    }
    updateDraft({
      ...draft,
      steps: [
        ...draft.steps,
        {
          id: createDraftStepId(),
          label: `Step ${draft.steps.length + 1}`,
          target_c: draft.steps[draft.steps.length - 1]?.target_c ?? "20",
          hold_value: "1",
          hold_unit: "days",
          advance_policy: "auto",
          ramp_enabled: false,
          ramp_value: "12",
          ramp_unit: "hours",
        },
      ],
    });
  };

  const removeStep = (index: number) => {
    if (!draft || draft.steps.length === 1) {
      return;
    }
    updateDraft({
      ...draft,
      steps: draft.steps.filter((_, stepIndex) => stepIndex !== index),
    });
  };

  const moveStep = (index: number, direction: -1 | 1) => {
    if (!draft) {
      return;
    }
    const targetIndex = index + direction;
    if (targetIndex < 0 || targetIndex >= draft.steps.length) {
      return;
    }
    const nextSteps = [...draft.steps];
    const [step] = nextSteps.splice(index, 1);
    nextSteps.splice(targetIndex, 0, step);
    updateDraft({
      ...draft,
      steps: nextSteps,
    });
  };

  const saveDraft = () => {
    if (!draft) {
      return;
    }
    setEditorError(null);
    saveMutation.mutate(draft, {
      onError: (error) => {
        setEditorError(
          error instanceof Error ? error.message : "Failed to save the profile.",
        );
      },
    });
  };

  const deleteSelectedProfile = () => {
    if (!selectedSummary) {
      return;
    }
    if (
      !window.confirm(
        `Delete the profile "${selectedSummary.name}"? Devices that already copied it will not be changed.`,
      )
    ) {
      return;
    }
    deleteMutation.mutate(selectedSummary.slug, {
      onError: (error) => {
        setEditorError(
          error instanceof Error ? error.message : "Failed to delete the profile.",
        );
      },
    });
  };

  const triggerImport = () => {
    fileInputRef.current?.click();
  };

  const handleImportFileChange = (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    event.target.value = "";
    if (!file) {
      return;
    }
    setEditorError(null);
    importMutation.mutate(file, {
      onError: (error) => {
        setEditorError(
          error instanceof Error ? error.message : "Failed to import the BeerXML file.",
        );
      },
    });
  };

  return (
    <div className="relative min-h-screen overflow-x-hidden bg-[var(--bg)] text-[var(--ink)]">
      <div className="absolute inset-x-0 top-0 -z-10 h-[24rem] bg-[radial-gradient(circle_at_top_left,rgba(47,108,96,0.22),transparent_32%),radial-gradient(circle_at_top_right,rgba(63,134,198,0.16),transparent_28%),linear-gradient(180deg,#f7f3ec_0%,#f4f1ea_100%)]" />
      <main className="mx-auto flex w-[min(1360px,calc(100vw-1.5rem))] flex-col gap-4 py-5 md:w-[min(1360px,calc(100vw-2rem))] md:py-7">
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
              Profiles
            </span>
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <Button variant="subtle" onClick={openNewDraft}>
              <Plus className="size-4" />
              New profile
            </Button>
            <Button
              variant="subtle"
              onClick={triggerImport}
              disabled={importMutation.isPending}
            >
              <FileUp className="size-4" />
              Import BeerXML
            </Button>
          </div>
          <input
            ref={fileInputRef}
            className="hidden"
            type="file"
            accept=".xml,text/xml,application/xml"
            onChange={handleImportFileChange}
          />
        </section>

        <section className="grid gap-4 xl:grid-cols-[360px_1fr]">
          <Card className="overflow-hidden">
            <CardHeader className="flex-col gap-4 border-b border-black/8 pb-5">
              <div>
                <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                  Profile library
                </p>
                <h1 className="mt-2 text-3xl font-extrabold tracking-[-0.05em]">
                  Fermentation profiles
                </h1>
                <p className="mt-2 text-sm leading-6 text-[var(--muted)]">
                  Built-in, imported and manual profiles live here. Apply them from
                  the device page when a controller should use one.
                </p>
              </div>
              <label className="relative block w-full">
                <Search className="pointer-events-none absolute left-4 top-1/2 size-4 -translate-y-1/2 text-[var(--muted)]" />
                <input
                  className="min-h-11 w-full rounded-[18px] border border-black/8 bg-white/80 px-11 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                  type="search"
                  placeholder="Search profiles"
                  value={search}
                  onChange={(event) => setSearch(event.target.value)}
                />
              </label>
            </CardHeader>
            <CardContent className="grid gap-3 pt-6">
              {profilesQuery.isLoading ? (
                <div className="rounded-[22px] bg-[var(--surface-strong)] p-4 text-sm text-[var(--muted)]">
                  Loading profiles...
                </div>
              ) : filteredProfiles.length ? (
                filteredProfiles.map((profile) => (
                  <button
                    key={profile.slug}
                    type="button"
                    className={`grid gap-3 rounded-[22px] border px-4 py-4 text-left transition ${
                      profile.slug === selectedSlug && !isCreatingNew
                        ? "border-[var(--accent)] bg-[color:color-mix(in_srgb,var(--accent)_8%,white)] shadow-[0_14px_28px_rgba(47,108,96,0.08)]"
                        : "border-black/8 bg-white/72 hover:bg-white"
                    }`}
                    onClick={() => openProfile(profile)}
                  >
                    <div className="flex flex-wrap items-start justify-between gap-2">
                      <div>
                        <strong className="block text-base font-bold">{profile.name}</strong>
                        <p className="mt-1 text-sm text-[var(--muted)]">{profile.slug}</p>
                      </div>
                      <Badge tone={sourceTone(profile.source)}>{profile.source}</Badge>
                    </div>
                    <div className="flex flex-wrap items-center gap-2 text-xs font-semibold uppercase tracking-[0.14em] text-[var(--muted)]">
                      <span>{profile.step_count} steps</span>
                      <span>{formatDurationCompact(profile.total_duration_s)}</span>
                      {profile.is_builtin ? <span>Locked</span> : null}
                    </div>
                    <p className="text-sm text-[var(--muted)]">
                      Updated {formatProfileTimestamp(profile.updated_at)}
                    </p>
                  </button>
                ))
              ) : (
                <div className="rounded-[22px] bg-[var(--surface-strong)] p-4 text-sm leading-6 text-[var(--muted)]">
                  No profiles matched the current filter.
                </div>
              )}
            </CardContent>
          </Card>

          <Card className="overflow-hidden">
            <CardHeader className="flex-col gap-4 border-b border-black/8 pb-5">
              {draft ? (
                <>
                  <div className="flex flex-wrap items-start justify-between gap-3">
                    <div>
                      <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                        {isCreatingNew ? "New profile" : "Profile editor"}
                      </p>
                      <h2 className="mt-2 text-3xl font-extrabold tracking-[-0.05em]">
                        {draft.name || "Untitled profile"}
                      </h2>
                    </div>
                    <div className="flex flex-wrap gap-2">
                      <Button
                        variant="primary"
                        onClick={saveDraft}
                        disabled={saveMutation.isPending}
                      >
                        <Save className="size-4" />
                        Save profile
                      </Button>
                      {!isCreatingNew && selectedSummary && !selectedSummary.is_builtin ? (
                        <Button
                          variant="danger"
                          onClick={deleteSelectedProfile}
                          disabled={deleteMutation.isPending}
                        >
                          <Trash2 className="size-4" />
                          Delete
                        </Button>
                      ) : null}
                    </div>
                  </div>
                  <div className="grid gap-3 md:grid-cols-3">
                    <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                      <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                        Steps
                      </p>
                      <strong className="mt-2 block text-2xl font-bold">
                        {draft.steps.length}/{MAX_STEPS}
                      </strong>
                    </div>
                    <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                      <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                        Apply
                      </p>
                      <p className="mt-2 text-sm leading-6 text-[var(--muted)]">
                        Use <strong className="text-[var(--ink)]">Choose profile</strong> on a
                        device page to copy this template into a controller.
                      </p>
                    </div>
                    <div className="rounded-[20px] bg-[var(--surface-strong)] p-4">
                      <p className="text-xs font-semibold uppercase tracking-[0.18em] text-[var(--muted)]">
                        Source
                      </p>
                      <strong className="mt-2 block text-2xl font-bold capitalize">
                        {isCreatingNew ? "manual" : selectedSummary?.source ?? "manual"}
                      </strong>
                    </div>
                  </div>
                </>
              ) : (
                <div>
                  <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                    Profile editor
                  </p>
                  <h2 className="mt-2 text-3xl font-extrabold tracking-[-0.05em]">
                    Pick a profile or create a new one
                  </h2>
                </div>
              )}
            </CardHeader>
            <CardContent className="grid gap-5 pt-6">
              {editorError ? (
                <div className="rounded-[20px] bg-[color:color-mix(in_srgb,var(--fault)_10%,white)] px-4 py-3 text-sm text-[var(--fault)]">
                  {editorError}
                </div>
              ) : null}

              {!draft ? (
                <div className="rounded-[24px] bg-[var(--surface-strong)] p-6 text-sm leading-6 text-[var(--muted)]">
                  The profile manager can build up to seven steps, import Brewfather
                  BeerXML exports and keep reusable templates in the web database.
                </div>
              ) : (
                <>
                  <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                    Profile name
                    <input
                      className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                      type="text"
                      value={draft.name}
                      onChange={(event) =>
                        updateDraft({
                          ...draft,
                          name: event.target.value,
                        })
                      }
                    />
                  </label>

                  <div className="grid gap-4">
                    {draft.steps.map((step, index) => (
                      <article
                        key={step.id}
                        className="rounded-[24px] border border-black/8 bg-white/72 p-5 shadow-[0_16px_36px_rgba(26,34,31,0.05)]"
                      >
                        <div className="flex flex-wrap items-start justify-between gap-3">
                          <div>
                            <p className="text-[11px] font-bold uppercase tracking-[0.18em] text-[var(--accent)]">
                              Step {index + 1}
                            </p>
                            <h3 className="mt-2 text-xl font-bold tracking-[-0.03em]">
                              {step.label || `Step ${index + 1}`}
                            </h3>
                          </div>
                          <div className="flex flex-wrap gap-2">
                            <Button
                              variant="secondary"
                              onClick={() => moveStep(index, -1)}
                              disabled={index === 0}
                            >
                              <ArrowUp className="size-4" />
                            </Button>
                            <Button
                              variant="secondary"
                              onClick={() => moveStep(index, 1)}
                              disabled={index === draft.steps.length - 1}
                            >
                              <ArrowDown className="size-4" />
                            </Button>
                            <Button
                              variant="secondary"
                              onClick={() => removeStep(index)}
                              disabled={draft.steps.length === 1}
                            >
                              <Trash2 className="size-4" />
                            </Button>
                          </div>
                        </div>

                        <div className="mt-5 grid gap-4 md:grid-cols-2 xl:grid-cols-[1.2fr_0.8fr_0.8fr]">
                          <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                            Step name
                            <input
                              className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                              type="text"
                              value={step.label}
                              onChange={(event) => {
                                const nextSteps = [...draft.steps];
                                nextSteps[index] = {
                                  ...step,
                                  label: event.target.value,
                                };
                                updateDraft({ ...draft, steps: nextSteps });
                              }}
                            />
                          </label>

                          <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                            Target (C)
                            <div className="relative">
                              <Thermometer className="pointer-events-none absolute left-4 top-1/2 size-4 -translate-y-1/2 text-[var(--muted)]" />
                              <input
                                className="min-h-11 w-full rounded-[18px] border border-black/8 bg-white/80 px-11 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                                type="number"
                                step="0.1"
                                value={step.target_c}
                                onChange={(event) => {
                                  const nextSteps = [...draft.steps];
                                  nextSteps[index] = {
                                    ...step,
                                    target_c: event.target.value,
                                  };
                                  updateDraft({ ...draft, steps: nextSteps });
                                }}
                              />
                            </div>
                          </label>

                          <div className="grid gap-2">
                            <label className="text-sm font-semibold text-[var(--ink)]">
                              Hold duration
                            </label>
                            <div className="grid gap-2 sm:grid-cols-[1fr_140px]">
                              <input
                                className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                                type="number"
                                min="0"
                                step="0.1"
                                value={step.hold_value}
                                onChange={(event) => {
                                  const nextSteps = [...draft.steps];
                                  nextSteps[index] = {
                                    ...step,
                                    hold_value: event.target.value,
                                  };
                                  updateDraft({ ...draft, steps: nextSteps });
                                }}
                              />
                              <select
                                className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                                value={step.hold_unit}
                                onChange={(event) => {
                                  const nextSteps = [...draft.steps];
                                  nextSteps[index] = {
                                    ...step,
                                    hold_unit: event.target.value as DurationUnit,
                                  };
                                  updateDraft({ ...draft, steps: nextSteps });
                                }}
                              >
                                <option value="days">Days</option>
                                <option value="hours">Hours</option>
                                <option value="minutes">Minutes</option>
                              </select>
                            </div>
                          </div>
                        </div>

                        <div className="mt-5 rounded-[20px] bg-[var(--surface-strong)] p-4">
                          <div className="grid gap-4 lg:grid-cols-[auto_1fr_1fr] lg:items-center">
                            <label className="inline-flex items-center gap-2 text-sm font-semibold text-[var(--ink)]">
                              <input
                                type="checkbox"
                                checked={step.ramp_enabled}
                                onChange={(event) => {
                                  const nextSteps = [...draft.steps];
                                  nextSteps[index] = {
                                    ...step,
                                    ramp_enabled: event.target.checked,
                                  };
                                  updateDraft({ ...draft, steps: nextSteps });
                                }}
                              />
                              Enable ramp
                            </label>
                            <div className="grid gap-2">
                              <label className="text-sm font-semibold text-[var(--ink)]">
                                Ramp duration
                              </label>
                              <div className="grid gap-2 sm:grid-cols-[1fr_140px]">
                                <input
                                  className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)] disabled:cursor-not-allowed disabled:opacity-50"
                                  type="number"
                                  min="0"
                                  step="0.1"
                                  disabled={!step.ramp_enabled}
                                  value={step.ramp_value}
                                  onChange={(event) => {
                                    const nextSteps = [...draft.steps];
                                    nextSteps[index] = {
                                      ...step,
                                      ramp_value: event.target.value,
                                    };
                                    updateDraft({ ...draft, steps: nextSteps });
                                  }}
                                />
                                <select
                                  className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)] disabled:cursor-not-allowed disabled:opacity-50"
                                  disabled={!step.ramp_enabled}
                                  value={step.ramp_unit}
                                  onChange={(event) => {
                                    const nextSteps = [...draft.steps];
                                    nextSteps[index] = {
                                      ...step,
                                      ramp_unit: event.target.value as DurationUnit,
                                    };
                                    updateDraft({ ...draft, steps: nextSteps });
                                  }}
                                >
                                  <option value="days">Days</option>
                                  <option value="hours">Hours</option>
                                  <option value="minutes">Minutes</option>
                                </select>
                              </div>
                            </div>
                            <label className="grid gap-2 text-sm font-semibold text-[var(--ink)]">
                              Advance policy
                              <select
                                className="min-h-11 rounded-[18px] border border-black/8 bg-white/80 px-4 text-[var(--ink)] outline-none transition focus:border-[var(--accent)]"
                                value={step.advance_policy}
                                onChange={(event) => {
                                  const nextSteps = [...draft.steps];
                                  nextSteps[index] = {
                                    ...step,
                                    advance_policy: event.target.value as
                                      | "auto"
                                      | "manual_release",
                                  };
                                  updateDraft({ ...draft, steps: nextSteps });
                                }}
                              >
                                <option value="auto">Auto</option>
                                <option value="manual_release">Manual release</option>
                              </select>
                            </label>
                          </div>
                        </div>
                      </article>
                    ))}
                  </div>

                  <div className="flex flex-wrap items-center justify-between gap-3 rounded-[20px] bg-[var(--surface-strong)] px-4 py-4">
                    <p className="text-sm leading-6 text-[var(--muted)]">
                      Time-based ramping is supported in v1. The structure keeps room
                      for future ramp modes without changing how templates are managed.
                    </p>
                    <Button
                      variant="primary"
                      onClick={addStep}
                      disabled={draft.steps.length >= MAX_STEPS}
                    >
                      <Plus className="size-4" />
                      Add step
                    </Button>
                  </div>
                </>
              )}
            </CardContent>
          </Card>
        </section>
      </main>
    </div>
  );
}
