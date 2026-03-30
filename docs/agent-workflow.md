# BrewESP Agent Workflow

This document defines a repo-specific multi-agent workflow for planning,
implementation, review, and verification work in BrewESP.

Use this alongside [AGENTS.md](C:/Users/ola/git/brewesp/AGENTS.md):

- `AGENTS.md` holds hard repository rules and safety constraints.
- `.codex/agents/*.md` holds role-specific prompt text.
- `.codex/agents/work-item-template.yaml` defines the handoff shape between
  agents.

## Goals

The workflow should:

- split work into small tasks with independent write scopes
- make firmware safety constraints explicit
- avoid accidental cross-area edits
- require evidence before a task is considered ready for review
- keep docs, schemas, and behavior aligned when contracts change

## Roles

### `planner`

Break the incoming goal or project item into a small number of concrete work
items with stable task ids, allowed paths, required checks, and a checkable
definition of done.

### `coder`

Implement exactly one work item and stay inside its allowed paths.

### `reviewer`

Review the patch against scope, definition of done, and BrewESP repository
rules. Focus on concrete findings and regressions.

### `verifier`

Run or assess the most relevant quality gates and report hard pass/fail signals.

### `safety-reviewer`

Required for tasks that touch the physical control path or device/config
contracts that can affect safe runtime behavior.

## Required work item fields

Every planned work item should follow the shared schema in
`.codex/agents/work-item-template.yaml`.

Minimum required fields:

- `task_id`
- `title`
- `goal`
- `allowed_paths`
- `forbidden_paths`
- `depends_on`
- `risk_level`
- `requires_safety_review`
- `required_checks`
- `definition_of_done`

## Write-scope rules

Prefer tasks with a single primary write area:

- `firmware/`
- `services/web/`
- `infra/`
- `docs/`

Allow cross-area edits only when the task explicitly requires it, for example:

- schema or MQTT contract updates that must be reflected in code and docs
- infra changes that require service docs to stay accurate
- firmware behavior changes that need wiring, config, or contract docs updated

When possible, split work like this:

1. one task for code behavior
2. one task for docs alignment
3. one task for follow-up cleanup or optional UI polish

## High-risk paths

Changes in the following areas should default to `risk_level: high` and
`requires_safety_review: true`:

- `firmware/src/controller/`
- `firmware/include/controller/`
- `firmware/src/output/`
- `firmware/include/output/`
- `firmware/src/sensor/`
- `firmware/include/sensor/`
- `firmware/src/config/`
- `firmware/include/config/`
- `docs/mqtt-contract.md`
- `docs/schemas/`

These areas can change:

- heating/cooling mutual exclusion
- safe shutdown behavior
- stale or invalid sensor handling
- retained config semantics
- backend integration boundaries

## Quality gates

Use the strongest relevant checks available for the touched area.

### Firmware tasks

Minimum gate:

- `pio run` from `firmware/`

Expected when relevant:

- manual inspection of control-path logic for heating/cooling mutual exclusion
- manual review of sensor-fault behavior

### Web tasks

Minimum gate:

- `docker compose -f infra/compose.yaml build web`

Good lightweight fallback when a full build is unnecessary or unavailable:

- Python import or compile smoke checks inside `services/web/`

### Infra tasks

Minimum gate:

- `docker compose -f infra/compose.yaml config`

Expected when service definitions changed:

- build the affected service image

### Docs-only tasks

Minimum gate:

- reviewer confirms changed docs match current code or intentionally describe a
  planned change

### Contract or schema tasks

Minimum gate:

- reviewer verifies alignment across code, docs, and the updated contract or
  schema files

## Approval rules

A work item is ready for `In review` only when:

- the patch stayed within scope
- required checks ran or the gap is explicitly reported
- the definition of done is satisfied
- required docs or schema updates are included
- safety review completed for high-risk tasks

Do not mark work `Done` until it is merged or explicitly accepted as complete.

## Example task decomposition

Incoming goal:

> Add a stale-sensor failsafe and show the fault in the web UI.

Good split:

1. `fw-stale-sensor-failsafe`
   Allowed paths: `firmware/`, `docs/`
   Requires safety review: `true`
2. `web-fault-badge`
   Allowed paths: `services/web/`
   Requires safety review: `false`
3. `docs-fault-contract`
   Allowed paths: `docs/`
   Requires safety review: `false`

Bad split:

1. `implement-everything`
   Allowed paths: entire repo

The bad split makes review, parallel work, and blame assignment weaker.

## Suggested directory layout

```text
.codex/
  agents/
    planner.md
    coder.md
    reviewer.md
    verifier.md
    safety-reviewer.md
    work-item-template.yaml
docs/
  agent-workflow.md
```

## Operating model

1. Planner reads the goal, `AGENTS.md`, and relevant docs.
2. Planner emits one or more work items using the shared schema.
3. A coder agent is assigned exactly one work item.
4. Reviewer checks the patch against scope and done criteria.
5. Verifier records quality-gate evidence.
6. Safety reviewer is required for high-risk work.
7. Only then should the task move toward `In review`.
