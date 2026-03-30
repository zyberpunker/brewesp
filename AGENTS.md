# BrewESP Agent Workflow

This file defines the working rules for Codex and other coding agents in this
repository.

## Git and Project Workflow

- Work from GitHub Project items when possible.
- Create a separate Git branch for each Project item before implementation
  starts.
- Use branch names in the form `codex/<project-item-slug>`.
- Prefer including a short identifier or stable keyword from the Project item in
  the branch name, for example `codex/sensor-stale-failsafe`.
- Do not mix unrelated Project items in the same branch.
- Move a Project item from `Ready` to `In progress` when implementation starts.
- Move a Project item to `In review` only after code changes are complete and
  basic verification has been run.
- Move a Project item to `Done` only after the work is merged or explicitly
  accepted as complete.
- If work is paused before completion, leave the item in `In progress` and note
  the remaining gap clearly.

## Branch, Commit, and PR Rules

- Create the branch before making code changes for the item.
- Keep commits scoped to the current Project item.
- Prefer at least one intentional commit per Project item before opening a PR.
- Use clear commit messages that describe the implemented behavior, not just the
  file area.
- Open a separate PR for each Project item unless the user explicitly asks to
  bundle related items.
- Prefer draft PRs first, then promote when verification is complete and the
  item is genuinely ready for review.
- Do not reuse an old branch for a new Project item.

## Project Item Handling

- Before starting implementation, confirm the current item scope from the
  project card and relevant docs.
- If the documentation and project card disagree, update one or both so the
  scope is explicit before implementing.
- If one item uncovers clearly separate follow-up work, create a new backlog
  item instead of silently expanding scope.
- Keep item titles, docs, and code behavior aligned enough that another person
  can trace why the change exists.

## Implementation Expectations

- Prefer finishing one Project item end-to-end before starting the next.
- Do not work on multiple unrelated Project items in parallel unless the user
  explicitly asks for that.
- Keep firmware, web, and docs aligned when a feature affects more than one
  area.
- Update documentation when behavior, wiring, config contracts, or workflows
  change.
- Treat physical safety behavior as higher priority than UI polish.
- Prefer small, incremental implementations that leave the system working after
  each item.
- Preserve backward compatibility for saved config and runtime safety behavior
  unless a documented migration is part of the item.

## Verification

- Run the most relevant verification available for the changed area before
  considering the item ready for review.
- For firmware changes, at minimum run a local build when possible.
- For firmware changes that affect hardware behavior, prefer a real device check
  when hardware is available.
- For web changes, at minimum run the relevant app/build validation when
  possible.
- For infrastructure or Docker changes, validate the compose or container setup
  that was touched.
- If full verification is not possible, state what was and was not verified.
- Verification should happen before moving the item to `In review`.

## Change Scope

- Avoid broad refactors unless they are required by the current Project item.
- Preserve offline-safe controller behavior.
- Do not introduce backend or MQTT dependencies into the physical control path
  unless explicitly planned and documented.
- Do not silently change hardware pin plans, config contracts, or MQTT topics
  without updating documentation and affected code paths together.

## Documentation Rules

- Update docs in the same branch when an item changes architecture, wiring,
  config schema, UI behavior, or operational workflow.
- Keep `README.md` high-level; put detailed behavior in the relevant file under
  `docs/`.
- If a planned feature is added, removed, or deferred, reflect that in the
  project and documentation in the same round of work.

## Multi-Agent Workflow

- For structured multi-agent execution, use
  [docs/agent-workflow.md](C:/Users/ola/git/brewesp/docs/agent-workflow.md) and
  the role prompts under `.codex/agents/`.
- Treat `AGENTS.md` as the hard policy layer and the `.codex/agents/` files as
  execution-role scaffolding.
- If a role prompt conflicts with `AGENTS.md`, follow `AGENTS.md`.

## Safety and Operational Rules

- Any change that can affect heating, cooling, sensor validity, or fail-safe
  behavior must be reviewed with safety in mind first.
- Never allow heating and cooling to be active together.
- Prefer safe shutdown on uncertain sensor state over optimistic continued
  control.
- Keep the ESP capable of operating safely with the last known valid local
  configuration if MQTT, Wi-Fi, or the backend is unavailable.
