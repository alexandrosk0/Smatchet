# Plan — Rename Blame → Annotate, then cleanup Annotate UI preferences

> **Status:** WIP plan, ready for a worker to execute. Phase 1 (rename) lands first as
> its own PR; Phase 2 (prefs cleanup) rebases onto the new names as a follow-up PR.
>
> **Reviewer corrections (verified against `develop` @ `77b1740`, 2026-05-30).** Every line
> anchor and exception in this plan was checked against the live tree. Phase-2 anchors are all
> exact. Five fixes were applied inline; the substantive ones:
> 1. **Agent paths were flat (`agents/<name>.md`) — actual tree is nested.** `p4-blame` lives at
>    `agents/project/p4-blame.md`; the 5 cross-referencing agents (`code-review`,
>    `debug-detective`, `spike-hunter`, `security-review`, `coderabbit-triage`) live under
>    `agents/core/`. The literal `git mv agents/p4-blame.md …` would have failed.
> 2. **Config rename was undercounted.** Two more `blame_*` config keys + their C++ members exist
>    beyond `blame_analysis` — see the expanded map row and step 3. A whole-word symbol pass renames
>    the members but leaves the JSON key string literals, causing a silent member↔key mismatch.
> 3. **`Locales/*.json` does not exist** — all localization (incl. the French strings) is in-code in
>    `SmatchetLocalization.cpp`. References scrubbed.
> 4. **`Source_Core/` → `Source/Core/`** throughout (the underscore path is non-existent; a
>    verification grep against it would false-pass on an empty path).
> 5. **Blast-radius headline corrected** to the measured counts.

## Context

The Perforce line-attribution feature is **user-labeled "Annotate"** in every display
string (menus, prefs tab, French locale), but the entire **internal vocabulary is
"Blame"** — class names (`P4Blame`, `BlameAnalysisUi`, `BlameState`), filenames
(`P4Blame.cpp`, `BlameAnalysisUi_*.cpp`), the config section `blame_analysis`,
localization **keys** (`blame.*`, `menu.blame_analysis`), the scenario ID
`blame-open-entry-tab`, and the `p4-blame` agent. This split is confusing.

This plan unifies on **Annotate** in two sequenced phases (separate PRs, per decision):

- **Phase 1 — mechanical rename** Blame → Annotate everywhere (this is the bulk of the
  work; lands first).
- **Phase 2 — preferences cleanup** of the Annotate UI, rebased onto the new names.

User decisions (confirmed):
- **Hard rename all** — no back-compat. Config key, scenario ID, loc keys all renamed.
  Existing user `smatchet_config.json` `blame_analysis` settings will reset to defaults;
  saved dock-layout id and any Lua/doc referencing the old scenario ID break. Accepted.
- **Rename the agent** (`p4-blame` → `p4-annotate`) and all routing/delegation tables.
- **Rename first, separate PRs.**

---

# PHASE 1 — Blame → Annotate rename

## Blast radius (verified)

`git grep -ci [Bb]lame` = **1499 hits / 107 files** all-in; **980 hits / 105 files** excluding the
regenerated `.understand-anything/` index. Split:
- **Rename targets** (~30 source/test/scenario/loc/CMake files + ~10 agent/doc files).
- **Excluded — regenerated**: `.understand-anything/` (519 hits) — auto-built index,
  regenerates; do not edit.
- **Excluded — historical record**: `docs/design/archive/*`, `backlog/POST_P0_REVIEW.md`,
  `backlog/BACKLOG_CODE_REVIEW.md`, applied self-improvement logs — these describe past
  work as it was; rewriting them falsifies history. *(This exclusion is the one judgment
  call — flag for veto. Active docs ARE updated: `docs/guides/cli.md`, `AGENTS.md`/`delegation.md`,
  `docs/adr/0009-*` if it references the live feature.)*

Root `CMakeLists.txt` has no Blame → core sources are globbed, so file renames need **no
root-CMake edit**. Only `tests/CMakeLists.txt` (5 hits) lists test files explicitly.

## Naming map (mechanical — apply as whole-word, case-preserving)

| Old | New |
|---|---|
| `P4Blame` (class + `P4Blame.{h,cpp}`) | `P4Annotate` (`P4Annotate.{h,cpp}`) |
| `P4BlameParse` (+ `.{h,cpp}`) | `P4AnnotateParse` (+ `.{h,cpp}`) |
| `P4LineBlame` (struct) | `P4LineAnnotate` |
| `BlameAnalysisUi` (class + all `BlameAnalysisUi_*.{h,cpp}`, `BlameAnalysisUi.{h,cpp}`) | `AnnotateAnalysisUi` (`AnnotateAnalysisUi_*`) |
| `BlameState` / `~BlameState` | `AnnotateState` / `~AnnotateState` |
| `BlameRow`, `BlameRowHasNonEmptyCallstackField` | `AnnotateRow`, `AnnotateRowHasNonEmptyCallstackField` |
| `BlameInternal` (namespace) | `AnnotateInternal` |
| `BlameAnalysisConfig`, `BlameUiThemeColors` | `AnnotateAnalysisConfig`, `AnnotateUiThemeColors` |
| `MakeBlameOpenEntryTabScenario` + `BlameOpenEntryTabScenario.cpp` | `MakeAnnotateOpenEntryTabScenario` + `AnnotateOpenEntryTabScenario.cpp` |
| config section `"blame_analysis"` | `"annotate_analysis"` |
| config key `"blame_comment_templates"` + member `BlameCommentTemplates` | `"annotate_comment_templates"` + `AnnotateCommentTemplates` |
| config key `"blame_allow_custom_commands"` + member `BlameAllowCustomCommands` | `"annotate_allow_custom_commands"` + `AnnotateAllowCustomCommands` |
| scenario ID `"blame-open-entry-tab"` | `"annotate-open-entry-tab"` |
| loc keys `blame.*`, `menu.blame_analysis`, `menu.source_blame`, `prefs.tab.blame_analysis`, `prefs.blame_comments` | `annotate.*`, `menu.annotate_analysis`, `menu.source_annotate`, `prefs.tab.annotate_analysis`, `prefs.annotate_comments` |
| dock-node id string `"blame"` (`SmatchetDockNodeIds.cpp`) | `"annotate"` |
| agent `p4-blame` (`agents/project/p4-blame.md`, frontmatter `name:`) | `p4-annotate` (`agents/project/p4-annotate.md`) |

**Manual-judgment exceptions (do NOT blind-replace):**
- `P4BlameAnnotateE2E.test.cpp` → **`P4AnnotateE2E.test.cpp`** (blind replace yields the
  nonsense `P4AnnotateAnnotateE2E`; drop the redundant word).
- `P4AnnotatedLine` already exists and is **already** Annotate-flavored — leave it; it
  coexists with the renamed `P4LineAnnotate` exactly as it coexisted with `P4LineBlame`.
- Verify no collision after rename between `P4LineAnnotate` and `P4AnnotatedLine` call
  sites (distinct identifiers — fine, but confirm no accidental merge).

## Execution (ordered — best run via `mechanic` agent, text-search driven)

1. **Rename files** with `git mv` (preserves history): all `P4Blame*`, `BlameAnalysisUi*`,
   `BlameOpenEntryTabScenario.cpp`, `P4BlameParse.test.cpp`, `P4BlameAnnotateE2E.test.cpp`
   (→ manual name above). Update every `#include "P4Blame.h"` / `BlameAnalysisUi.h` etc.
2. **Symbol rename** across `Source/Core/{src,include}`, `tests/`, `Plugins/` — whole-word
   `Blame`→`Annotate` for the identifiers in the map. Touches the obvious subsystem files
   plus call sites in `SmatchetUI*.cpp`, `SmatchetPreferencesUi*`, `SmatchetActiveProjectGridUi.cpp`,
   `SmatchetGridUiSupport.{h,cpp}`, `SmatchetFieldRender.cpp`, `AppController*.{h,cpp}`,
   `JiraClient.h`, `JiraIssueMutation.cpp`, `ITrackerCollaboration.h`, `Logger.h`,
   `SubprocessCapture.{h,cpp}`, `FieldEditAuditSource.h`, `CodeColorView.h`, etc.
3. **Config keys** in `ConfigManager.{cpp,h}` — three JSON keys + their C++ members, NOT just
   `blame_analysis`. (a) `"blame_analysis"` → `"annotate_analysis"` at `ConfigManager.cpp:466,469,559`
   (load+save). (b) `"blame_comment_templates"` (`:275,881,883`) + member `BlameCommentTemplates`.
   (c) `"blame_allow_custom_commands"` (`:341,688`) + member `BlameAllowCustomCommands`. **Trap:** a
   whole-word symbol pass renames the *members* but leaves the JSON *key string literals* — rename
   both, in lock-step, or you get a silent member↔key mismatch. No back-compat read — old keys
   silently ignored (accepted breakage; this now covers all three).
4. **Localization** `SmatchetLocalization.cpp` (the only loc file — there is **no `Locales/*.json`**;
   the French strings are in-code here too) — rename ~20 `blame.*` keys + `menu.source_blame`
   (`:95`), `menu.blame_analysis` (`:104`), `prefs.blame_comments` (`:207`) and every matching
   `LOC("blame.…")` call site (grep the key strings, not just the table).
5. **Scenario ID** `"blame-open-entry-tab"` → `"annotate-open-entry-tab"` in
   `SmatchetScenarioRegistry.cpp:88` + the factory decl `:40`; grep tests/Lua/docs for the
   ID string and update.
6. **Tests** `tests/CMakeLists.txt` (file names + any `ctest -R "…Blame…"` pattern),
   `FakeP4Runner.h`, `P4DescribeCacheE2E.test.cpp`, the renamed test files, scenario stubs.
7. **Agent infra** (note the nested `agents/{core,project}/` layout — paths are NOT flat) —
   `git mv agents/project/p4-blame.md agents/project/p4-annotate.md`; frontmatter `name:`;
   `docs/agent-rules/delegation.md` (6 hits), `agents/core/coderabbit-triage.md` routing regex
   `BlameAnalysisUi*` → `AnnotateAnalysisUi*` (`:129`) + the `p4-blame` cell (`:31`),
   `agents/core/code-review.md`, `agents/core/debug-detective.md`, `agents/core/spike-hunter.md`,
   `agents/core/security-review.md`, `agents/README.md`; `scripts/dev/test-agent-contract.sh`
   (9 hits, asserts agent set). Re-run `bash scripts/setup-harness.sh claude-code` to refresh the
   `.claude/agents/` junction.
8. **Active docs** — `docs/guides/cli.md`, `AGENTS.md` if it names the feature, ADR 0009 only if
   it describes the live feature (not the historical decision text).

## Phase-1 verification
1. Dual-target build: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
2. `scripts/dev/test-all.sh` (renamed tests compile + pass; `ctest -R Annotate`).
3. `scripts/dev/test-agent-contract.sh` green with `p4-annotate`.
4. Launch exe → Inspect → "Annotate…" opens; Preferences → Annotate tab renders.
5. Run the renamed scenario: `annotate-open-entry-tab` executes via CLI.
6. `git grep -ri "blame" -- Source/Core/ tests/ scripts/ agents/ ':(exclude)docs/design/archive/**'`
   (the `.understand-anything/` index is untracked / regenerated) returns **zero** hits.
7. Lint pass on every edited `.cpp`/`.h` (`Source/Core/src/Config/`, `/Commands/` are strict zones).

## Phase-1 implementation log (executed 2026-05-30)

Executed via `mechanic` agent (text-search driven), orchestrator-verified. 68 files
changed (18 `git mv` with history), diff-scoped `clang-format` on 14 files. All gates green:
dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) exit 0; doctest 843 cases /
5770 assertions / 0 failed + Lua tests pass; `test-agent-contract.sh` 25/0; scenario
`annotate-open-entry-tab` runs (`ok:true`); delta-lint gate PASS; zero-hit grep clean over
`Source/Core/ tests/ scripts/ agents/ project.config.json` (sole residual is the
DO-NOT-TOUCH `tests/ui/archived/*.archived` file).

### Deviations from plan (scope additions discovered during the orchestrator inventory)
The plan's naming map was incomplete; three live identifiers/strings were renamed in
addition to the documented map (all consistent with the hard-rename, no-back-compat intent):
1. **`AddIssueCommentBlameContext` → `AddIssueCommentAnnotateContext`** — a 13-site Jira
   "blame-context comment" method (`AppController.h`, `ITrackerCollaboration.h`,
   `JiraClient.h`, `AppController_CatalogAndFieldEdit.cpp`, `JiraIssueMutation.cpp`,
   `AnnotateAnalysisUi_Window.cpp`) + its "not supported by this backend" error string.
2. **audit literal `"blame_context"` → `"annotate_context"`** (`JiraIssueMutation.cpp`,
   `comment_kind` audit value; single site, not test-asserted).
3. **`project.config.json`** `p4-blame` agent registry + curated-map entries → `p4-annotate`
   (the plan's step 7 omitted this live config file).
   Also: `tests/fixtures/config/v5.json` key renamed in lock-step with the loader.

### Deferred to Phase 2 (label-normalization scope)
Two user-visible display *values* now read awkwardly because the case-aware replace touched
copy, not just keys: French `prefs.blame_comments` value became `"Commentaires de annotate"`
(grammatically broken — should be e.g. `"Commentaires d'annotation"`), and the English
"blame context" action label became "annotate context". These are Phase-2 § 2's job
(naming / label consistency) — left as-is in Phase 1 to keep it a pure rename.

---

# PHASE 2 — Annotate UI preferences cleanup (rebased on Phase-1 names)

All identifiers below use the **post-rename** names (`AnnotateAnalysisConfig`,
`AnnotateAnalysisUi_*`, `annotate_analysis`, …).

## 1. Refactor — collapse the 3 autoselect functions
`AnnotateAnalysisUi_Config.cpp` has three structurally identical
`MaybeAutoselect*TrackerField` functions differing only by (target field ref,
name-match predicate, callstack-hint flag). Extract one internal helper; keep the three
public wrappers (2 call sites each: `AnnotateAnalysisUi_Window.cpp` + `AnnotateAnalysisUi.cpp`)
as one-liners. Preserve the defensive misspelling matches (`"last occurances"` /
`"last occurences"`) — they match real Jira **field names**, not our strings.

## 2. Naming / label consistency (form display text)
Normalize prefs-form labels: consistent p4/p4vc casing, spell out "command", and give
the three Jira combos distinct visible labels (currently all `"Jira field"`).

## 3. UX — persist the raw/table view toggle (`showRaw`)
`showRaw` (`AnnotateAnalysisUi_Internal.h`) is ephemeral — toggled at 3 button sites
(`_Window.cpp:188,197,210`), force-reset on close (`_Modals.cpp:284`), coupled with the
streamlined-grid flag (`_Window.cpp:173`). Promote: add `bool ShowRawCallstack=false;` to
`AnnotateAnalysisConfig`, round-trip `"show_raw_callstack"`, seed on hydrate, write back at
the 3 sites; keep the close-handler runtime reset but leave cfg intact (reopen re-seeds).

## 4. Multi-rule path-remap editor (UI-only)
`ApplyPathRemaps` (`CallstackParser.cpp:142`) already does multi-rule longest-prefix
matching (tested, `CallstackParser.test.cpp:162-220`). Only the UI is single-rule. Replace
the two fixed `remapFrom`/`remapTo` buffers with a per-row editor over the full
`PathRemaps` vector (add/remove rows), persist whole vector. Drop the single-rule load
(`AnnotateAnalysisUi.cpp:63-68`) and dead sync (`_Worker.cpp:131-133`).

## 5. Expose the two form-less knobs
`ChangelistCacheMaxEntries` + `UiColors.*` round-trip but have no control. Add
`InputInt("Changelist cache size")` (clamped 16..8192) and a collapsing "Colors" section
of seven `ColorEdit4` controls.

## 6. Value-range guards
Clamp `cl_cache_max` on load too (`ConfigManager.cpp`) so a hand-edited negative can't
reach `_Worker.cpp:103`. `maxFrames` already clamped; `ColorEdit4` constrains colors.

## Phase-2 out of scope / NOT doing
- Defensive misspelling matches stay (match real badly-named Jira fields).

## Phase-2 files
`ConfigManager.{h,cpp}`, `AnnotateAnalysisUi_Internal.h`, `_Config.cpp`, `_Preferences.cpp`,
`AnnotateAnalysisUi.cpp`, `_Window.cpp`, `_Worker.cpp`, `_Modals.cpp`. No `CallstackParser`
change (already multi-rule).

## Phase-2 verification
Build dual-target; launch exe; confirm: labels clean + distinct Jira combos; raw/table
view persists across reopen; multi-rule remap round-trips to `annotate_analysis.path_remaps`
+ `ctest -R CallstackParser` green; `ColorEdit4` recolors live + persists; cache size
clamps; misspelled-field autoselect still works. Lint pass on edited files.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

Both phases touch `Source/Core/`, so the gate **applies** — but every gate is N/A-by-content because there is no hot-path logic change:

- **Phase 1** is a pure mechanical rename (identifiers, file names, config/loc keys, scenario ID). Zero behaviour change; the binary is behaviour-equivalent modulo symbol/string names. Perf-neutral by construction; validated not asserted.
- **Phase 2** edits the Annotate **preferences** UI (collapse 3 autoselect helpers → 1, add config round-trips, multi-rule remap editor, expose two knobs, clamp-on-load). All of it runs on Preferences-window interaction, not in the steady-state grid / annotate render path — outside the 6.94 ms budget.

1. **PR-fast CI** — N/A by content (no algorithmic change). Run one representative scenario (`annotate-open-entry-tab`) post-change to confirm perf-neutrality rather than assert it. Map: `agents/core/perf-gatekeeper.md` § Curated diff → scenario map.
2. **Pillar 2 static scanner** — N/A — no new sync-I/O; neither the rename nor the prefs edits add an `ImGui::*`-reachable blocking call (`pillar2-scan.sh` must stay silent over the renamed tree).
3. **Dispatcher drain** — N/A — `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A — no new sync-stall path > 100 ms.
5. **Marker inventory** — N/A — no `SMATCHET_UI_PERF_SCOPE` markers added; existing markers move with their renamed TU (string scope-names unchanged by the rename).

**Pre-push local check**: run `docs/PERF_WORKFLOW.md` § Gate-check vs baseline against `annotate-open-entry-tab` once post-rename; expect within-noise.

**Override**: not needed — no intentional regression.

## Phase-2 implementation log (executed 2026-05-30)

All 6 items shipped. Files touched: `ConfigManager.{h,cpp}`, `AnnotateAnalysisUi_Internal.h`,
`_Config.cpp`, `_Preferences.cpp`, `AnnotateAnalysisUi.cpp`, `_Window.cpp`, `_Worker.cpp`.

1. **Autoselect collapse** — three `MaybeAutoselect*TrackerField` bodies replaced by one
   file-local `MaybeAutoselectTrackerField(app, targetFieldId, nameMatches, updateCallstackHint)`;
   the three public wrappers are now thin call-throughs. Misspelling matches preserved.
2. **Labels** — `P4 executable`→`p4 executable` (casing consistent with `p4vc`); `… cmd`→`… command`;
   the three identically-labelled `"Jira field"` combos are now `Callstack source field` /
   `Before-changelist field` / `Last-occurrences date field`.
3. **showRaw persisted** — new `AnnotateAnalysisConfig::ShowRawCallstack` round-tripped as
   `show_raw_callstack`; seeded per window-open in `_Window.cpp` (`justOpened`); the 3 toggle
   sites call the new `ApplyShowRawCallstack(bool)` helper (sets runtime flag + cfg + persist);
   the close-handler runtime reset (`_Modals.cpp`) is intentionally kept (reopen re-seeds from cfg).
4. **Multi-rule remap editor** — single `remapFrom`/`remapTo` buffers replaced by a per-row editor
   (`std::vector<RemapEditBuf>` mirroring `cfg.PathRemaps`) with add/Remove; persists the whole
   vector. Single-rule seed (`AnnotateAnalysisUi.cpp`) and dead worker sync (`_Worker.cpp`) removed.
   `CallstackParser` untouched (already multi-rule).
5. **Form-less knobs exposed** — `InputInt("Changelist cache size")` (clamped 16..8192) bound to
   `ChangelistCacheMaxEntries`; a collapsing **Colors** section of 7 `ColorEdit4` controls bound to
   `UiColors.*` (recolor live + persist on commit).
6. **Value-range guard** — `ChangelistCacheMaxEntries` now clamped to 16..8192 on load in
   `ConfigManager::LoadAnnotateAnalysis` (a hand-edited negative can no longer reach the worker).

**Tests:** added `tests/Core/AnnotateAnalysisConfig.test.cpp` (3 cases / 17 assertions) covering
the cl_cache_max clamp-on-load (item 6), `show_raw_callstack` round-trip (item 3), and multi-rule
`path_remaps` round-trip + empty-`from` drop (item 4) via the `ConfigManager` persistence seam. Full
suite 846/0. Satisfies the CI Test-delta gate (Source/Core change carries a test delta).

**Deferred (unchanged):** backlog **N14** (move the once-guarded config hydrate off the UI thread)
remains a separate follow-up — bundling threading work would break this PR's perf-neutral, prefs-only
scope. The English "annotate context" comment-action label from Phase 1 is left to a copy pass.
