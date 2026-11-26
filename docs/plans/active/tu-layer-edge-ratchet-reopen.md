# Plan — Reopen: ratchet upward includes from translation units

> **Slug**: `tu-layer-edge-ratchet-reopen` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Plan-link convention (tier-less — write ALL plan references this way)**: link other plans by the **tier-less** form `docs/plans/<slug>.md` — NOT `docs/plans/active/<slug>.md` or `docs/plans/shipped/<slug>.md`. The link gates (`test-markdown-links`) resolve a tier-less path against `active/`, `shipped/`, or `deferred/`, so the reference keeps working when a plan is archived (`git mv active → shipped`) without any ref-sweep. A **tiered** link passes the local check while the target still lives in `active/`, then 404s on CI after the archival move — the `markdown-links-local-passes-ci-fails-after-plan-archive` trap; `test-markdown-links.sh --merge-tree-warn` flags tiered links before they rot.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

**DRAFT — awaiting the owner's accept / reject. No implementation is authorised by this plan.**

This is a reopen proposal. In the session that spawned this plan the orchestrating agent recommended "a ratchet on upward includes that also counts edges from `.cpp` files to headers" before it had read the project's records (session history; unverified from the tree). The records show the project already considered translation-unit (TU) edges and kept them out of the layer DAG on purpose. The owner asked for the case for reopening, so they can accept or reject it.

### Recorded decision

1. [`include_cycle_audit.py`](../../../agent-layer/agents/scripts/core/include_cycle_audit.py), docstring of `_is_header` (line 265):
   > "A .cpp/.cc/.cxx is a translation unit — a graph SINK (nothing #includes it), so it can never lie inside an include cycle, and "a .cpp pulls in the higher-layer headers it implements against" is the NORMAL dependency direction, not a layer violation. The layer-DAG is a header->header constraint (that is what governs compile-time coupling + cycles), so only header includers participate in the back-edge / cycle check."

   The same file, `find_violations` docstring: back-edges need both endpoints in a NAMED layer (rank >= 1), because rank 0 "is the leaf/unranked floor" with "NO position in the Ui>AppController>Commands>Tracker>Sync/Persistence>Config hierarchy".
2. [`docs/plans/core-include-dag.md`](../core-include-dag.md), § Approach: the gate "FAILs on any SCC > 1 node OR any lower→higher layer edge"; § Deviations / grill-with-docs outcome fixes the ranking `Ui(6) > AppController(5) > Commands(4) > Tracker(3) > Sync/Persistence(2) > Config(1) > root-leaf headers(0)`. The plan does not discuss TU includers in its own text; the header-only scope is recorded in the audit it shipped (item 1).
3. [`85-ui-include-direction.sh`](../../../agent-layer/agents/scripts/project/lint-rules.d/85-ui-include-direction.sh), header comment: the include-cycle gate's "back-edge check is deliberately header->header … so a domain .cpp -> Ui/ header edge sailed through … This rule closes exactly that direction where it is never legitimate." And: "Commands/ is intentionally NOT in scope: its Scenarios/ TUs drive UI-level machinery on the UI thread by contract … sanctioned couplings that would need grandfathering."
4. The enforcement contract-card row for `no-ui-include-in-domain` in [`agent-layer/AGENTS.md`](../../../agent-layer/AGENTS.md) restates it: "the include-cycle back-edge check is header→header by design, so a domain-TU→Ui edge escapes it; Commands/ excluded".

So the project chose: header→header DAG for everything, plus one targeted TU rule for the one direction that is never legitimate (domain → Ui).

The owner has separately approved (unverified from the tree; taken from the task brief) an agent-layer change that makes `no-ui-include-in-domain` also catch bare-name includes of Ui headers. Verified today: in the rule's domain dirs, exactly one bare-name Ui include remains — `Source/Core/src/Tracker/TrackerGridFieldDisplay.cpp:9` includes `SmatchetFieldRender.h`, which resolves only to `Source/Core/include/Ui/`. The three other domain→Ui TU edges (`Tracker/TrackerDateTimeFieldEditor.cpp:9,11,13`) are path-qualified and each carries a `SMATCHET_DEVIATION(rule=no-ui-include-in-domain; …; revisit=2027-02-16)`. This plan is about what stays uncovered after that fix.

### What this plan asks

Should the project add any gate on upward includes from TUs beyond `no-ui-include-in-domain`? The measurement says a general TU ratchet would be mostly noise; the honest proposal is one narrow absolute-0 direction rule, or nothing.

## Approach

### Measurement (today, this branch)

Method: `python3 -c` importing `include_cycle_audit` (`texts_head`, `build_graph`, `layer_rank`, `_is_header`), unchanged resolution logic. Includers: `.cpp` files under `Source/Core/src/`. Edge counted when `layer_rank(target) > layer_rank(includer)`.

- TUs in scope: 410. Rank-up TU edges: **245**, in **101** TUs.
- Header→header gate today: 0 violations (`find_violations` empty; baseline `docs/high-integrity/include-cycle-baseline.md` holds 0 edges).

| Includer → target | Edges | TUs | Covered today? |
|---|---|---|---|
| Domain (1–3) → Ui (6) | 4 | 2 | Yes — `no-ui-include-in-domain` (3 deviated; 1 bare-name, caught after the approved fix) |
| Domain (1–3) → AppController (5) | 2 | 2 | Only by the count-based `app-controller-fan-in` ratchet |
| Domain (1–3) → Commands (4) | 0 | 0 | No |
| Sync/Persistence (2) → Tracker (3) | 5 | 3 | No |
| Config (1) → anything above | 0 | 0 | No |
| Commands (4) → AppController (5) | 5 | 5 | Fan-in ratchet only |
| Commands (4) → Ui (6) | 64 | 39 | No (40 of the 64 are `Commands/Scenarios/`) |
| AppController (5) → Ui (6) | 24 | 11 | No |
| Root rank 0 → 1/2/3/4/5/6 | 26 / 27 / 36 / 5 / 5 / 42 | — | Exempt (141 edges total) |

Named-layer includers (rank >= 1) account for 104 edges. 93 of them (Commands → AppController/Ui, AppController → Ui) are the "TU implements against higher-layer headers" pattern the recorded rationale names as normal.

The domain residue:
- Domain → AppController: `Tracker/TrackerGridFieldDisplay.cpp:3`, `Tracker/TrackerLabelsEditor.cpp:2`. (`Source/Plugins/Mcp/McpPlugin.cpp` also includes `AppController.h`; it is in rule 85's scope but outside `include_cycle_audit`'s `Source/Core/` scope. Not examined further.)
- Sync/Persistence → Tracker: `Persistence/LocalCacheManager.cpp:3` → `Tracker/IssueDraft.h`; `Sync/OfflineQueueService.cpp:7,8,21` → `Tracker/IssueCreatePipeline.h`, `Tracker/IssueDraft.h`, `Tracker/TrackerFieldPayloadPure.h`; `Sync/TicketSyncService.cpp:21` → `Tracker/ParentHierarchyPure.h`. These are data types and pure helpers a sync service consumes; the Sync headers stay clean (the header gate is at 0).

What the rank-0 exemption hides: 42 edges from 13 rank-0 TUs (11 at the `Source/Core/src/` root, 2 in `Diagnostics/`) into `Ui/`. Most are UI-ish code that lives at the root (`TicketFieldEditor*.cpp` 27 edges, `SmatchetGrid*.cpp` 7). But service-like root TUs are in there too: `Diagnostics/BugReportService.cpp:23` → `Ui/SmatchetImGuiHostC.h`, `SmatchetMergeWatchNotifyServer.cpp:6` → `Ui/SmatchetToast.h`, `SmatchetTicketChangeNotifications.cpp:9` → `Ui/SmatchetToast.h`, `AiPrefsTestConnection.cpp:15` → `Ui/SmatchetUiSession.h`. Rank 0 has no position in the hierarchy, so "upward" is undefined for these; a rank-based gate cannot see them by construction.

### Options

**(a) Do nothing.** Grandfathering: 0. False-positive risk: none. Cost: none. What stays open: a domain TU can include `AppController.h` as long as the same diff drops another includer (the fan-in gate passes when "HEAD count <= base count", per `appcontroller_fan_in_audit.py` docstring); a domain TU can include a `Commands/` header; root service TUs can include Ui. None of these has a recorded escape that I found.

**(b) One more absolute-0 direction rule.** Candidates, with today's count:
- `no-commands-include-in-domain` (Tracker/Sync/Persistence/Config → `Commands/`): **0**. False-positive risk: real. The main-thread interfaces live in `Commands/` (`IMainThreadPoster.h`, `IAppThreading.h`), and root TUs already include them (`SmatchetMergeWatchNotifyServer.cpp:3`, `TicketFieldEditor_Modal.cpp:5`, `TicketFieldEditor_Worklog.cpp:10`, `AiPrefsTestConnection.cpp:8`). A domain service that needs to post to the UI thread through that interface is the dependency inversion core-include-dag Phase 2 itself introduced, so the first hit would likely be a false positive. Its real fix would be moving those interfaces to the root, which is a different plan.
- `no-appcontroller-include-in-domain` (Tracker/Sync/Persistence/Config → `AppController.h`): **2** (the two Tracker TUs above). Both need a deviation marker or a fix on day one. False-positive risk: low. This option departs from the recorded "a .cpp pulls in the higher-layer headers it implements against is the NORMAL dependency direction" for the domain dirs, on the argument that including the god header from a domain TU is not "implementing against" it, and the Sync leaf doc already routes a domain service to the app through a deps interface ("The service reaches the app through `IOfflineQueueDeps`", `Source/Core/src/Sync/AGENTS.md` line 24). It closes the swap loophole of the count-based fan-in gate for the domain dirs only.
- Config → anything above: 0, but it is a subset of the two rules above plus rule 85. No separate rule.

**(c) WARN-first, delta-gated rank-up TU edge count.** Lives in the agent-layer repository: a `--tu-edges` mode in `include_cycle_audit.py` (or a sibling audit), `--diff origin/develop`, `--selftest`, a baseline at `docs/high-integrity/`, the `SMATCHET_DEVIATION(rule=…)` escape, registration in `test-lint-rules.sh` and a contract-card row in `agent-layer/AGENTS.md`. Grandfathering: **104** edges with rank-0 includers exempt (matching the header gate), **245** if rank 0 counts as below everything. False-positive risk: high. 93 of the 104 are the pattern the recorded rationale calls normal, so most new WARNs would be a new command handler including `AppController.h` (already caught by the fan-in gate) or a new Scenario including a Ui header (sanctioned by rule 85's own comment). The signal is the 11 domain edges, which (b) and rule 85 address directly.

**(d) Fail-closed TU gate.** Same code location and the same 104 / 245 baseline as (c). It would block every new Commands → Ui and AppController → Ui TU include, which the project records as normal. This contradicts the recorded decision outright and would need a new layering decision first. Not defensible on this evidence.

### Recommendation

**Do not reopen the general TU ratchet (reject (c) and (d)).** The recorded rationale holds for 93 of 104 named-layer TU edges, and the measured noise ratio confirms it.

**Take the narrow option (b): `no-appcontroller-include-in-domain`, absolute-0 over Tracker/Sync/Persistence/Config, with 2 existing edges deviated.** It is cheap, it is a sibling of a rule the project already accepted, and it closes a gap the fan-in gate leaves open by design. The case for it is modest: no recorded escape motivates it. If the owner prefers fewer rules, (a) is a defensible answer and nothing breaks.

## Interface contracts

N/A — lint tooling only. No product interface, command, MCP schema, config or cache format changes. The only product-tree change is two comment lines (deviation markers).

## Files to modify

For the recommended option (b) only. Agent-layer repository first, then a Smatchet pointer bump.

Agent-layer repository (`agent-layer/`):
1. [`agents/scripts/project/lint-rules.d/85-ui-include-direction.sh`](../../../agent-layer/agents/scripts/project/lint-rules.d/85-ui-include-direction.sh) — add rule id `no-appcontroller-include-in-domain` to `UIINCLUDE_RULES` (line 24); in `scan_ui_include_direction_file`, also flag a quote-form include whose basename is `AppController.h`, with its own deviation token. Scope: the Tracker/Sync/Persistence/Config dirs of `compute_ui_include_direction_violations`, **not** `Source/Plugins/Mcp/` (McpPlugin.cpp is unexamined). Sequence after the approved bare-name change to the same file.
2. [`agents/scripts/project/test-lint-rules.sh`](../../../agent-layer/agents/scripts/project/test-lint-rules.sh) — selftest fixtures beside the existing ones (lines 425–441): a hit, a comment-only non-hit, an `AppControllerImpl.h` non-hit, a deviated non-hit.
3. [`tests/bats/lint_rules.bats`](../../../agent-layer/tests/bats/lint_rules.bats) — cases beside the `no-ui-include-in-domain` block (line 586).
4. [`AGENTS.md`](../../../agent-layer/AGENTS.md) — enforcement contract-card: add the rule id to the `no-ui-include-in-domain` row (or a sibling row) so the selftest token check finds it.

Smatchet:
5. `Source/Core/src/Tracker/TrackerGridFieldDisplay.cpp:3` — deviation marker above `#include "AppController.h"` (or remove the include if the TU can take a narrower deps interface).
6. `Source/Core/src/Tracker/TrackerLabelsEditor.cpp:2` — same.
7. `agent-layer` submodule pointer bump.

## Existing utilities reused

- `scan_ui_include_direction_file` / `compute_ui_include_direction_violations` in `85-ui-include-direction.sh` — scan loop, nearest-non-blank-above deviation grammar, domain-dir file list.
- `appcontroller_fan_in_audit.py` `TARGET_BASENAME = "AppController.h"` and its basename match (excludes `AppControllerImpl.h`) — copy the matching rule, not the count.
- `include_cycle_audit.layer_rank` / `build_graph` — used for this plan's measurement only.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — the plan extracts nothing. The `agent-layer/AGENTS.md` edit is one table cell, but that file is near its 150-line cap; check `agent_size_audit.py` on the change.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — lint script and comments only.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no runtime code.
- **Pillar 3 (never crash)**: no impact — no runtime code.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no impact.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

The recommended diff touches `Source/Core/` only with two comment lines, so every gate is N/A.

1. **PR-fast CI** — N/A: no executable change; no scenario exercises a comment.
2. **Pillar 2 static scanner** — N/A: no new I/O.
3. **Dispatcher drain** — N/A: not touched.
4. **Visible-cue bucket-E harness** — N/A: no new stall path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: N/A — no perf-relevant code.

**Override**: not needed.

## Risks / non-goals

- **Deviation debt.** Two new markers with a revisit date add to the deviation cohort. Mitigation: pick a revisit date that does not push one date past 8 markers (`deviation-cohort` WARN); or fix the two TUs instead.
- **Rule overlap with the fan-in gate.** A new domain includer of `AppController.h` would fail both gates. Accepted: the messages differ (direction vs count) and both are exact signals.
- **Same-file collision with the approved bare-name change.** Mitigation: land that change first, then this one.
- **Observed, not caused here:** `appcontroller_fan_in_audit.py` reports 71 includers against a documented baseline of 70 today. Out of scope; noted so nobody attributes it to this plan.
- **Non-goal:** any TU-edge count or baseline file; rank-0 root TUs; Commands → Ui; AppController → Ui; Sync/Persistence → Tracker (data types and pure helpers, headers already clean).
- **Non-goal:** moving `IMainThreadPoster.h` / `IAppThreading.h` out of `Commands/`.
- **What would make this proposal wrong:**
  - If the owner considers rank order the whole architecture (any upward edge is a defect), then (c) is right and this plan undersells it. The recorded decision says otherwise.
  - If the 5 Sync/Persistence → Tracker edges are a real problem (for example, a Sync TU that cannot build without Tracker client code), then a fifth direction rule is owed. I saw only data-type and pure-helper targets.
  - If the root service TUs including Ui headers (`BugReportService.cpp`, `SmatchetMergeWatchNotifyServer.cpp`, `SmatchetTicketChangeNotifications.cpp`) are the real risk, no rank-based gate helps; that needs a path-based rule or a re-rank of those files, which is a different plan.
  - If the two Tracker TUs genuinely need `AppController.h`, (b) is just two permanent deviations and (a) is better.
  - If there is an escape history for domain → AppController that I did not find, (b) is more valuable than stated.

## Implementation handoff

N/A — single slice, agent-layer lint (`agent-layer/agents/scripts/project/`), then a two-marker Smatchet slice owned by `Source/Core/src/Tracker/AGENTS.md`. Order: approved bare-name change → this rule + markers in one agent-layer PR plus one Smatchet pointer-bump PR (the rule must not land before the markers, or the tree goes red).

## Verification

- **Bucket A (pure-logic doctest, `test-authoring` skill § Unit rig)**: N/A — no product logic.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A — no UI change.
- **Bash-driver scenario / screenshot / sanitizer**: `bash agent-layer/agents/scripts/project/test-lint-rules.sh --selftest`; `--scan-ui-include` returns empty on the tree with the two markers; the new `lint_rules.bats` cases pass.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — comment-only change, run once.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required. Not run while drafting.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: **still owed** — this draft skipped the grill pass; run `grill-with-docs` against it before the plan is final, and record the outcome.
- **Manual residue**: none.

## Open questions

1. Is `Source/Plugins/Mcp/McpPlugin.cpp` including `AppController.h` sanctioned? Answers: Mcp plugin owner. Decides whether Mcp joins the new rule's scope later.
2. Deviate or fix the two Tracker TUs? Answers: Tracker subsystem owner (a probe of which `AppController` members they use).
3. **Decision request for the owner:** accept the recommended option (reject (c)/(d); add `no-appcontroller-include-in-domain` as option (b)), pick another listed option ((a) do nothing, (b) with the Commands candidate, (c) or (d)), or reject the reopen.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Rank-0 root service TUs including Ui headers — follow-up plan only if the owner sees risk there (path-based rule or re-rank).
- Moving the main-thread interfaces out of `Commands/` — prerequisite for any domain → Commands rule; own plan.
- The fan-in count being one over its documented baseline — belongs to the fan-in gate's owner.

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
