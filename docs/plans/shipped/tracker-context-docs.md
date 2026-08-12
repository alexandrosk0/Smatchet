# Plan — Tracker subsystem context docs + multi-context scaffold
<!-- plan-date: 2026-06-01 -->

> **Slug**: `tracker-context-docs` (matches this file's basename without `.md`).
>
> **Usage**: stress-tested via `grill-with-docs` (2026-06-01 session). Decisions captured inline below.

## Context

LLM coding agents landing in `Source/Core/src/Tracker/` (~40 files, the densest subsystem) get no orientation and no domain glossary. The repo's plan-doc rules + AGENTS.md grep already *assume* per-context `src/<ctx>/CONTEXT.md` files exist — they do not. Only one global glossary (`docs/CONTEXT.md`) exists, and it carries a **stale** canonical term (`ITrackerClient`, an interface that no longer exists — split into `ITrackerBackend` + 5 role interfaces by `docs/plans/shipped/tracker-interface-split.md`).

Doxygen / generated API docs were considered and rejected: LLMs read source directly, vexp already provides semantic index + skeletons, and Doxygen's banner/`@param` idiom collides with the repo's anti-comment-bloat gates. The higher-leverage investment is sharp human-authored **domain glossary** + durable **orientation** docs.

**Intended outcome**: after this lands, an agent opening Tracker has (a) a format-compliant domain glossary, (b) a stale-tolerant orientation map, (c) a root `CONTEXT-MAP.md` linking the multi-context structure, and (d) a data-driven WARN gate nudging README freshness — and the convention is uniform so later subsystems slot in.

## Approach

Adopt the **multi-context** model from the grill-with-docs skill (option B): a root `CONTEXT-MAP.md` lists contexts; each heavy subsystem owns its own `CONTEXT.md` (glossary) + `README.md` (orientation). The existing `docs/CONTEXT.md` is **left untouched** as the system-wide / cross-cutting glossary and linked from the map (minor `UpdateField` / `TrackerIssueKey` term overlap accepted, not migrated).

Two artifacts per subsystem, separated (option C): `CONTEXT.md` is a glossary *only* (per `CONTEXT-FORMAT.md` — one-sentence term defs, relationships, example dialogue, zero implementation detail); `README.md` is orientation (request flow, role-of-each-file) authored **durable-by-construction** — no `file:line` refs, no "currently N files" counts, carries an as-of-commit freshness header.

Scope this plan to **scaffold + Tracker exemplar only** (option A). Tracker is the densest, highest-value target and sets the template every later subsystem copies. Commands / Persistence / Sync / flat-root → backlog, each its own future grill.

A **data-driven staleness gate** (WARN, never blocks) enrols any `Source/Core/src/<ctx>/README.md`: when a diff vs `origin/develop` touches that context's `.cpp`/`.h` but not its `README.md`, emit a warning. Wired into `scripts/dev/test-all.sh` so it runs at the **local** pre-push gate and in CI (not CI-only).

## Files to modify

**Scaffold (uniform convention):**
1. `CONTEXT-MAP.md` (new, repo root) — lists contexts (system-wide `docs/CONTEXT.md` + Tracker), how they relate, and the growth path. Root location is skill-literal so grill-with-docs infers structure correctly.
2. `agents/scripts/project/test-context-readme-staleness.sh` (new) — data-driven WARN gate; discovers `Source/Core/src/*/README.md`, diffs vs `origin/develop`, warns on code-changed-README-untouched. Goes through `test-shell-lint.sh` (5 rules).
3. `scripts/dev/test-all.sh` (edit) — invoke the staleness gate in the pre-push sequence.

**Tracker exemplar:**
4. `Source/Core/src/Tracker/CONTEXT.md` (new) — domain glossary. Roster: Tracker backend (`ITrackerBackend` + 5 roles), TrackerIssueKey, set-replace semantics, TrackerField/Family/Option, Field catalog (+cache), IssueDraft, IssueCreatePipeline, Field payload, JqlProjectScope/SuggestEngine, Fixture backend. `CachedTicket` cross-referenced (owned by Persistence), not redefined. Grouped: `## Backend abstraction` / `## Field model` / `## Issue creation` / `## Query`.
5. `Source/Core/src/Tracker/README.md` (new) — orientation: backend abstraction shape, the create/update request flow, per-backend divergence points, fixture-vs-live split. Freshness header, no line numbers.

## Existing utilities reused

- `agents/scripts/project/test-lint-rules.sh` `--diff origin/develop` pattern — copy the delta-vs-develop diff mechanism for the staleness gate.
- `agents/scripts/core/is-pure-docs-diff.sh` — referenced by the gate to skip when a diff is pure-docs (no code change → no staleness possible).
- `scripts/dev/test-all.sh` existing gate-invocation block — append, don't restructure.
- `.claude/skills/grill-with-docs/CONTEXT-FORMAT.md` — the glossary format contract `CONTEXT.md` must satisfy.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — pure docs + one shell gate, zero runtime C++.
- **Pillar 2 (UI-thread never blocks)**: no impact — no UI code touched.
- **Pillar 3 (never crash)**: no impact — no product code touched.
- **Pillar 4 (accessibility)**: no impact — no UI code touched.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — diff adds only Markdown under `Source/Core/src/Tracker/` + a shell gate + a `docs/` map. No `.cpp`/`.h` source compiled into either target changes; `is-pure-docs-diff.sh` classifies the C++-tree portion as docs-only.

## Risks / non-goals

- **Risk: orientation README rots.** Mitigated by durable-by-construction authoring (no `file:line`, no counts) + the WARN staleness gate. Accepted residual: WARN can't detect *semantic* staleness, only file-touch divergence.
- **Risk: glossary drifts from code** (as `ITrackerClient` did). Mitigated by sourcing every term from a header read during this grill; cross-link to owning header by name (not line).
- **Risk: staleness gate false-positives** on no-op code edits. Accepted — it's WARN, non-blocking; PR label `context-readme-out-of-band` documents intentional skips and reserves the escape path for a future FAIL graduation.
- **Non-goal: migrating `docs/CONTEXT.md`.** Left untouched per grill decision; term overlap accepted.
- **Non-goal: other subsystems.** Commands / Persistence / Sync / flat-root deferred to per-subsystem follow-up grills (backlog).
- **Non-goal: fixing the stale `ITrackerClient` name in `AGENTS.md` / `docs/CONTEXT.md`.** Flagged below.

## Verification

- **Bucket A (pure-logic ctest)**: `N/A — no C++ added.`
- **Bucket E (ImGui Test Engine)**: `N/A — no UI added.`
- **Bash-driver**: `bash agents/scripts/project/test-context-readme-staleness.sh --diff origin/develop` — assert (a) clean exit when Tracker README is present + untouched-but-no-code-change, (b) WARN line emitted when a synthetic Tracker `.cpp` edit lacks a README touch, (c) data-driven discovery picks up a second `src/<ctx>/README.md`. Add a `tests/bats/context_readme_staleness.bats` covering these three.
- **Shell lint**: `bash agents/scripts/core/test-shell-lint.sh` on the new gate script (5 rules).
- **Build gate**: `N/A — pure-docs + shell; no compile.` (`is-pure-docs-diff.sh` confirms.)
- **Manual residue**: none. Glossary/README *content* correctness is human-reviewed at PR (inherent to docs); no automatable semantic check deferred.

## Out of scope (flagged, not designed)

- **Stale `ITrackerClient` in `AGENTS.md` + `docs/CONTEXT.md`** — real bug (interface split shipped; name never updated). Follow-up: a `mechanic` sweep re-pointing `ITrackerClient` → `ITrackerBackend` (+ role interfaces) across `AGENTS.md`, `docs/CONTEXT.md`, `agents/*.md`. Backlog `docs/self-improvement/categories/process.md`.
- **Commands / Persistence / Sync / flat-root context docs** — per-subsystem follow-up grills.
- **Graduating the staleness gate WARN → FAIL** — deferred until the convention proves out across ≥2 subsystems; the PR-label escape hatch is built now so graduation is config-only.
- **`docs/CONTEXT-MAP.md` vs root** — chose root per skill contract; not revisiting.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
