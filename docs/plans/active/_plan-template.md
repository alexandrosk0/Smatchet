# Plan — <feature name>

> **Slug**: `<kebab-case-slug>` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Plan-link convention (tier-less — write ALL plan references this way)**: link other plans by the **tier-less** form `docs/plans/<slug>.md` — NOT `docs/plans/active/<slug>.md` or `docs/plans/shipped/<slug>.md`. The link gates (`test-markdown-links`) resolve a tier-less path against `active/`, `shipped/`, or `deferred/`, so the reference keeps working when a plan is archived (`git mv active → shipped`) without any ref-sweep. A **tiered** link passes the local check while the target still lives in `active/`, then 404s on CI after the archival move — the `markdown-links-local-passes-ci-fails-after-plan-archive` trap; `test-markdown-links.sh --merge-tree-warn` flags tiered links before they rot.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Why this change. The problem or need it addresses. What prompted it (incident, user request, PR feedback, perf regression). Intended outcome — the one-sentence "after this lands, X is true." Cross-link to the originating issue / PR / incident.

## Approach

The recommended approach in 1-3 paragraphs. Not the alternatives — only the one you're proposing. If a non-obvious trade-off shaped the choice, name it in one sentence (deeper rationale → ADR).

## Interface contracts

The exact deltas to shared interfaces, or `N/A — <reason>` when none change. The `architect` agent writes this section for a cross-cutting design; every other plan fills it the same way.

- **What to name**: methods on `ITrackerBackend` and its six role interfaces (`ITrackerIssueReader`, `ITrackerConnectivity`, `ITrackerFieldCatalog`, `ITrackerIssueMutations`, `ITrackerCollaboration`, `ITrackerActivity`); `RegisterCommand({...})` entries; sol2 bindings in `AppController_LuaBindings*.cpp` and their `AppController_LuaStubs.cpp` stubs; MCP tool schemas; per-backend view storage; config, cache and SQLite formats. Backend-specific code stays inside the concrete client (`Source/Core/src/Tracker/AGENTS.md`).
- **Plumbing, hop by hop**: for every `X → Y → Z` path the change flows through, name the signature, struct member or typedef arity at each hop, and call out any channel that must widen. Never write "plumb from context".
- **The chokepoint, not the upstream caller**: for a cross-cutting signature change, count the direct callers of the changed surface; with zero or near zero, the change site is the binding adapter or facade shim that Lua, MCP and UI route through — list that shim in § Files to modify.
- Read the real header under `Source/Core/include/` before naming a contract; command, view-storage and tracker abstractions have established shapes.

## Files to modify

Numbered list. Each entry: `path:line` link + one-line per-file rationale. Group by subsystem when the list runs past ~10 entries.

Before you add a row here:
- **Grep before naming a new TU.** Before listing a new `<Foo>.{h,cpp}`, run `rg -l '<Foo>' Source/Core/` to confirm it doesn't already exist under that name or a synonym — pure helpers routinely already shipped under a different name, and a parallel duplicate is expensive to unwind.
- **Anchor the dual-target build to the gated files.** If the diff touches `SMATCHET_WITH_*` source-list gating, name the specific files and anchor `cmake --build … --target SmatchetStandalone SmatchetCore_DX12` to them (don't rely on the generic § Verification line alone).
- **Inline a not-yet-merged sibling's shape.** If a slice depends on or copies a pattern from a sibling slice that hasn't merged yet, include that pattern's intended shape (3–5 lines of code or a fixture-name list) inline here, so the implementing agent doesn't have to invent it.
- **Name files that exist.** Glob every production `.h` / `.cpp` the plan names; a renamed or moved file costs the implementer a cross-walk per phase.
- **CMake wiring.** Core `.cpp` files are picked up by `file(GLOB_RECURSE CORE_SOURCES …)` in `Source/Core/CMakeLists.txt` — no list edit. A new `tests/{Core,Plugins}/**/*.test.cpp` must be added to `add_executable(SmatchetTests …)` in `tests/CMakeLists.txt`; configure FATALs on an unlisted test. For every `add_library(<X> INTERFACE)` the plan adds, name the `target_link_libraries(<consumer> … <X>)` that consumes it.
- **Splitting a monolithic `.cpp`** (the `AnnotateAnalysisUi*.cpp` shape): state the closure rule ("everything `<target-fn>` calls that is not already in another TU goes to `<bucket>`") and copy every non-self include of the original into the new internal header.
- **Pure-helper TU split** (a unit whose pure helpers sit in an anonymous namespace of a `.cpp` that pulls cpr / httplib / SQLite / ImGui / GLFW / Unreal headers): pre-authorise `Source/Core/{include,src}/<Unit>Pure.{h,cpp}` (or `<Unit>Parse.{h,cpp}` for a parser) in the write set; the helpers move into a named namespace (`smatchet::<area>::pure` as in `McpJsonRpcPure` and `AiEndpointSanitize`, or the unit's own as in `P4AnnotateParse`), the original `.cpp` keeps its call sites through `using` declarations inside its anonymous namespace, and the new TU includes none of those headers (grep-guard it). Precedents: `IssueCreatePipelineHelpers`, `P4AnnotateParse`, `McpJsonRpcPure`, `AiEndpointSanitize`, `ILuaBindingHost` + `AppController_LuaBindingsCore`.
- **Symbol and CMake-variable tables** go under a `### File-level changes` subsection with `Symbol` / `CMake variable` columns, so `bash agent-layer/agents/scripts/core/plan-doc-table-probe.sh <plan>` can check every row before the plan is sealed.

## Existing utilities reused

Bullet list. Each entry: `Class::method` or `function_name` + `file:line` + one-line reason. Prevents the "reinvented the wheel" review finding.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

For any plan that moves bodies out of an over-cap file (a whale agent prompt, an `AGENTS.md` section, a long TU) into a sink (skill, `docs/agent-rules/*`, helper TU), do this one-pass sizing BEFORE writing the diff — `N/A — <reason>` when the plan extracts nothing:

1. **Classify each chunk EXTRACT vs STAYS.** EXTRACT = procedure body / deterministic mechanics / reference detail a reader only needs when actively doing the thing. STAYS = the judgment, the trigger, the one-line "what + why" navigation, and the pointer to the extracted sink.
2. **Compute the target.** `target ≈ Σ(STAYS) + pointer-overhead` (one cross-link line per extracted chunk). State the projected post-extraction line count and confirm it clears the cap (agent prompt ≤ 250 / `AGENTS.md` ≤ 150 — `AGENTS.md` § Enforcement contract-card).
3. **Sink must net-shrink the source.** If the extraction doesn't drop the source under its cap (or meaningfully toward soft-warn), reconsider the seam — splitting along the wrong boundary just relocates bytes. Sinks are soft-warn-only, so the win is measured at the SOURCE.

Keep this tight: one tag per chunk in § Files to modify is enough (`EXTRACT → <sink>` / `STAYS`); this section just states the target number + that it clears the cap.

## UX Pillar callouts

Per `AGENTS.md` § UX Pillars (1 Performance, 2 UI-never-freezes, 3 Never-crash, 4 Accessibility-aspirational). For each, one sentence — either "no impact (reason)" or "impact + how it's mitigated."

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: …
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: …
- **Pillar 3 (never crash)**: …
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: …

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`. For each gate, declare: **fires** (one-line how) or **N/A** (one-line reason). No restating mechanics — link out for the canonical text.

1. **PR-fast CI** — name the scenario most directly exercising the changed path. Map: [`scripts/dev/perf-scenario-map.json`](../../../scripts/dev/perf-scenario-map.json) (read by the `perf-gatekeeper` skill). Subset declared in `scripts/dev/perf-pr-fast-set.json`.
2. **Pillar 2 static scanner** — any new sync-I/O reachable from `ImGui::*`? If yes, worker-thread plan + `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` annotation.
3. **Dispatcher drain** — touches `MainThreadDispatcher::Drain()`?
4. **Visible-cue bucket-E harness** — adds a new sync-stall code path > 100 ms?
5. **Marker inventory** — adds `SMATCHET_UI_PERF_SCOPE` markers? If yes, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against the named scenario(s) before opening the PR.

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — intentional regression + baseline-bump PR queued only.

## Risks / non-goals

Bulleted. Each risk: one-line description + mitigation or "accepted (reason)." Non-goals: what this plan explicitly does not do, so a reviewer doesn't ask why X is missing.

Check at least: save-format breaks (views, config, the SQLite cache — schema changes are additive-only); backend leakage into the shared tracker interfaces; Unreal vs standalone divergence (`SMATCHET_EMBEDDED_IN_UNREAL`, `SMATCHET_WITH_MCP`, `SMATCHET_WITH_LUA_AUTOMATION`); localization key churn; MCP wire-format changes; dual-target compile failures (`SmatchetCore_DX12` also compiles `Source/Core/`). A latency or size budget on a `std::regex` path (a ReDoS sentinel, say) is pinned only after a 256 B / 512 B / 1 KiB / 2 KiB probe on the standard library of every toolchain that compiles that code (MSVC for Windows; clang for the Linux and Android lanes).

## Implementation handoff

Slices in order, or `N/A — single slice, <owner>`. Per slice:

- **Owner subsystem** — the leaf `AGENTS.md` directory that governs it (registry: `CONTEXT-MAP.md`).
- **Write set** — the files the slice may touch; everything else is out of scope.
- **Pre-resolved invariant decisions** — each leaf invariant or interface contract the slice could collide with, and the option chosen.
- **Inline inventory** — the exact symbol / literal matches (`<file>:<line>:<role>`) the implementer needs, so nobody re-derives them.
- **Order** — what must land first and why.

A slice that spans two subsystems is split, or the reason it must stay together is stated.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic doctest, `test-authoring` skill § Unit rig)**: …
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: …
- **Bash-driver scenario / screenshot / sanitizer**: …
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Required for every plan — do not delete.
- **Manual residue**: if any verification step ends up manual, name the deferred-automation action plan + add a `docs/self-improvement/categories/tooling.md` entry. No silent residue.

## Open questions

Specific questions the author could not settle, each with who answers it (user / owner subsystem / a probe), or `None`. "What should the API look like?" is not specific.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

Bulleted. Sister-features the user might assume are included; name + one-line "follow-up plan" or "no-action" rationale.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
