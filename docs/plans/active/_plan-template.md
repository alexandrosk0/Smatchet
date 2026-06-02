# Plan — <feature name>

> **Slug**: `<kebab-case-slug>` (matches this file's basename without `.md`).
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Why this change. The problem or need it addresses. What prompted it (incident, user request, PR feedback, perf regression). Intended outcome — the one-sentence "after this lands, X is true." Cross-link to the originating issue / PR / incident.

## Approach

The recommended approach in 1-3 paragraphs. Not the alternatives — only the one you're proposing. If a non-obvious trade-off shaped the choice, name it in one sentence (deeper rationale → ADR).

## Files to modify

Numbered list. Each entry: `path:line` link + one-line per-file rationale. Group by subsystem when the list runs past ~10 entries.

Before you add a row here:
- **Grep before naming a new TU.** Before listing a new `<Foo>.{h,cpp}`, run `rg -l '<Foo>' Source/Core/` to confirm it doesn't already exist under that name or a synonym — pure helpers routinely already shipped under a different name, and a parallel duplicate is expensive to unwind.
- **Anchor the dual-target build to the gated files.** If the diff touches `SMATCHET_WITH_*` source-list gating, name the specific files and anchor `cmake --build … --target SmatchetStandalone SmatchetCore_DX12` to them (don't rely on the generic § Verification line alone).
- **Inline a not-yet-merged sibling's shape.** If a slice depends on or copies a pattern from a sibling slice that hasn't merged yet, include that pattern's intended shape (3–5 lines of code or a fixture-name list) inline here, so the implementing agent doesn't have to invent it.

## Existing utilities reused

Bullet list. Each entry: `Class::method` or `function_name` + `file:line` + one-line reason. Prevents the "reinvented the wheel" review finding.

## UX Pillar callouts

Per `AGENTS.md` § UX Pillars (1 Performance, 2 UI-never-freezes, 3 Never-crash, 4 Accessibility-aspirational). For each, one sentence — either "no impact (reason)" or "impact + how it's mitigated."

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: …
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: …
- **Pillar 3 (never crash)**: …
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: …

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`. For each gate, declare: **fires** (one-line how) or **N/A** (one-line reason). No restating mechanics — link out for the canonical text.

1. **PR-fast CI** — name the scenario most directly exercising the changed path. Map: `agents/core/perf-gatekeeper.md` § Curated diff → scenario map. Subset declared in `scripts/dev/perf-pr-fast-set.json`.
2. **Pillar 2 static scanner** — any new sync-I/O reachable from `ImGui::*`? If yes, worker-thread plan + `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` annotation.
3. **Dispatcher drain** — touches `MainThreadDispatcher::Drain()`?
4. **Visible-cue bucket-E harness** — adds a new sync-stall code path > 100 ms?
5. **Marker inventory** — adds `SMATCHET_UI_PERF_SCOPE` markers? If yes, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against the named scenario(s) before opening the PR.

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — intentional regression + baseline-bump PR queued only.

## Risks / non-goals

Bulleted. Each risk: one-line description + mitigation or "accepted (reason)." Non-goals: what this plan explicitly does not do, so a reviewer doesn't ask why X is missing.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: …
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: …
- **Bash-driver scenario / screenshot / sanitizer**: …
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Manual residue**: if any verification step ends up manual, name the deferred-automation action plan + add a `docs/self-improvement/categories/tooling.md` entry. No silent residue.

## Out of scope (flagged, not designed)

Bulleted. Sister-features the user might assume are included; name + one-line "follow-up plan" or "no-action" rationale.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
