# Repeatable Pillar 1 + Pillar 2 performance review — Smatchet
<!-- plan-date: 2026-05-20 -->

> **Retirement note (2026-07, HP-07):** the Slice-2 `docs/harness/claude-code/hooks/lint-cpp-pillar2.sh` thin shim described below was **retired** — `lint-cpp-drain.sh` now calls the canonical `scripts/dev/pillar2-scan.sh` directly (see `lint-cpp-drain.sh` § pillar-2 pass), so the wrapper was never copied by `setup-harness.sh` nor sourced by the drain. The historical Slice-2 text is preserved as-written; treat every mention of `lint-cpp-pillar2.sh` below as removed. The live Pillar-2 surfaces are `scripts/dev/pillar2-scan.sh` (canonical), the `Pillar 2 scanner` CI lane (`.github/workflows/pillar2-scan.yml`), and the inline call in `lint-cpp-drain.sh`.

## Context

This session's PR #311 → PR #313 round-trip exposed that Smatchet's UX Pillar 1 (sustained 144 Hz, 6.94 ms frame budget) and Pillar 2 (no synchronous I/O on the UI thread; visible cue within 100 ms) are **enforced reactively, not proactively**. PR #311 shipped 5 markdown-rendering slices with zero perf consideration; the user observed FPS drops post-merge; `perf-detective` ran a reactive investigation; PR #313 followed with five optimisations (~93 % reduction on the rich-render hot path). Total agent-time cost: ~3 h + a full follow-up PR + reactive user feedback round-trips.

The underlying tooling already exists and is production-grade — scenario runner, `UiPerfMonitor` + `SMATCHET_UI_PERF_SCOPE`, the `perf-detective` / `perf-measure` / `perf-instrument` / `spike-hunter` agents, `MainThreadDispatcher`. **What's missing is the connective tissue**: a baseline registry to compare against, a CI gate that runs per PR, a static scanner that catches new sync-on-UI calls in a diff, and a process mandate so every feature plan exercises the loop. This plan builds that connective tissue across five sequential slices.

**Harness-agnostic by construction**: every executable artefact lives in harness-neutral locations — `scripts/dev/*.sh|*.py` for runnable logic, `.github/workflows/*.yml` for CI, `agents/<name>.md` for agent definitions per the [agents.md](https://agents.md/) spec. Per-harness glue (Claude Code hooks, skill aliases) is generated from `docs/harness/<name>/` templates by `scripts/setup-harness.sh`; Codex and Cursor read the canonical paths directly. No new code is Claude-Code-only; the existing Claude Code skill-alias pattern (used by `perf-instrument`, `perf-measure`) is the template for any new agent in this plan.

**Intended outcome**: after Slice 5 lands, every PR that touches `Source_Core/` runs the perf-review loop automatically — a small PR-fast scenario suite measures the changed surface, the lint pipeline rejects new sync-I/O reachable from `ImGui::*`, a scheduled full-suite run posts trend data, `perf-gatekeeper` is available for on-demand scenario-aware PR review, and a visible-cue assertion harness verifies the < 100 ms cue invariant. Pillar 1 + 2 become merge-blocking, not post-merge-investigation territory.

## Current state (verified by direct file inspection)

**Pillar 1 ready-to-use:**
- Scenario runner: `Source_Core/include/Commands/Scenarios/IScenario.h` + `Source_Core/src/Commands/Scenarios/ScenarioRunner.cpp`. CLI `scenario.list / scenario.run [--spawn]` from `Source_Core/src/Commands/Builtin/BuiltinCommands_Scenario.cpp`. 14 production scenarios in `Source_Core/src/Commands/Scenarios/` (Idle, PriorityGridScroll, CommandPaletteFuzzy, CellEditBurst, ThemeSwitchRoundtrip, AttachmentPreviewOpen, DockGapSentinel, LongTextOpenLargeAdf, LuaRecorderFuzz, PreferencesSliderDrag, AgentHandoff, AgentTriage, WhisperAiAssistantAutosend, WhisperDictation).
- Perf monitor: `Source_Core/include/UiPerfMonitor.h` — `SMATCHET_UI_PERF_SCOPE("name")` macro at line 66, `UiPerfMonitor::Reset()` / `GetLastFrameRows()`. JSON via `perf.snapshot / perf.reset / perf.dump`.
- Agents: `agents/perf-detective.md`, `agents/perf-measure.md`, `agents/perf-instrument.md`, `agents/spike-hunter.md`. Skill aliases for the two that get used most often: `agents/_shared/skills/perf-measure/`, `agents/_shared/skills/perf-instrument/`.
- Canonical doc: `docs/guides/perf-workflow.md` (11.6 KB, the 6-step loop).
- Existing perf script template: `scripts/dev/test-grid-edit-perf-postfix.sh` (auto-enrolled via `scripts/dev/test-all.sh`; uses `debug.grid.edit-burst` headless harness + mean / p50 / p95 / p99 assertions).

**Pillar 2 ready-to-use:**
- `Source_Core/include/MainThreadDispatcher.h` — 4096-task bounded queue; `Drain()` runs once per frame at `SmatchetUI::Draw` head; `BeginShutdown()` for teardown safety.
- AGENTS.md § UX Pillars: Pillar 2 enforceable invariants (code-review CRITICAL flag for new `cpr::` / `SQLite::` / `p4 …` / `std::ifstream` / `std::mutex::lock` reachable from `ImGui::*`).
- Audit precedent: `docs/plans/shipped/pillar-1-2-audit-2026-05-17.md` — 9 CRITICAL + 3 HIGH + 3 MEDIUM sites; closed by PR #191.
- `agents/code-review.md` Pillar 2 checklist (manual diff read today).

**Harness adapter pattern (verified):**
- Canonical agents at `agents/<name>.md` per agents.md spec — read directly by Codex.
- Per-harness templates at `docs/harness/<harness>/` — `claude-code/` has `hooks/` + `settings.json.tmpl`; `codex/` has only `setup.md`; `cursor/` has `rules/` + `setup.md`.
- `scripts/setup-harness.sh <name>` generates `.{claude,codex,cursor}/` adapter directories from those templates.
- Skill aliases at `agents/_shared/skills/<name>/SKILL.md` — Claude-Code-specific optimisation, copied into `.claude/skills/` by setup-harness.

**Gaps closed by this plan:**
- G1.1 No baseline registry. → Slice 1.
- G1.2 No CI perf gates. → Slice 3 (PR-fast) + Slice 4 (full).
- G1.4 No per-subsystem marker inventory. → Slice 4.
- G2.1 No static diff scanner for sync I/O. → Slice 2.
- G2.2 No `MainThreadDispatcher` drain-time measurement. → Slice 2.
- G2.3 No automated visible-cue check. → Slice 5.
- G2.4 No estimated-latency comment lint. → Slice 2.
- G2.5 No call-graph reachability check (text-search heuristic, not full AST). → Slice 2.

**Bucket-E (ImGui Test Engine) is wired** — per `docs/plans/shipped/imgui-test-engine-bucket-e-execution.md` (landed 2026-05-19). First test at `tests/ui/views_columns_reorder.test.cpp`; preset `ninja-ui-test-msvc`; tests registered via `IM_REGISTER_TEST`; bash drivers at `scripts/dev/test-ui-<area>.sh`. Slice 5 of this plan uses bucket-E directly rather than the earlier scenario-polling workaround.

**Gaps deferred** (out of scope):
- G1.3 Scenario authoring friction / Lua DSL — tracked as XL; defer to follow-up plan.

## Architecture

```
PR author / orchestrator (any harness — Claude Code, Codex, Cursor)
       |
       v
+-------------+
| Slice 1     |  docs/perf/baselines/<scenario>.<host>.json
| baseline    |  scripts/dev/perf-baseline.sh    (CRUD; harness-agnostic bash)
| registry    |  scripts/dev/perf-run.sh         (single-scenario driver)
+------+------+  scripts/dev/perf-compare.py     (delta vs baseline; markdown out)
       |        docs/perf/regression-policy.json (thresholds + tolerance bands)
       v
+------+------+        +-------------+
| Slice 2     |        | Slice 3     |  .github/workflows/perf-pr-fast.yml
| pillar2     |        | PR-fast CI  |  <-- runs PR-fast subset on every PR
| static gate |        +------+------+  <-- gh pr comment <PR> with delta table
+------+------+               |
       |                      v
       |               +------+------+
       |               | Slice 4     |  .github/workflows/perf-full.yml
       |               | full + agent|  <-- scheduled cron; baseline-bump PR on improvement
       |               +------+------+  agents/perf-gatekeeper.md (+ skill alias)
       |                      |
       |                      v
       |               +------+------+
       |               | Slice 5     |  tests/ui/sync_stall_visible_cue.test.cpp
       |               | visible-cue |  bucket-E IM_REGISTER_TEST asserts spinner
       |               | (bucket-E)  |  appears < 100 ms under synthetic 250 ms stall
       +-------------> +-------------+

Harness adapters (Claude Code / Codex / Cursor) — Slice 2 only

    docs/harness/claude-code/hooks/lint-cpp-pillar2.sh  <-- copied into .claude/ by setup-harness.sh
    docs/harness/codex/hooks-equivalent.md              <-- documents how Codex invokes the scan
    docs/harness/cursor/hooks-equivalent.md             <-- same for Cursor

    All three thin wrappers call: bash scripts/dev/pillar2-scan.sh <file>...
```

## Key design decisions (worth knowing before reading components)

**D1 — Baselines are PER-HOST, captured on the CI runner.** Hardware variance between developer laptops and GitHub Actions `windows-latest` runners is too large to share a single baseline. Schema: `docs/perf/baselines/<scenario>.<host>.json` where `<host>` is one of `{ci-windows-latest, dev}`. CI workflows only compare against `ci-windows-latest` baselines; local `perf-baseline.sh bump` produces / updates `dev` baselines for developer-side regression hunting.

**D2 — Tolerance bands reflect runner noise, not just code change.** GitHub Actions shared runners are virtualised + noisy. Default `regression-policy.json`: `mean_delta_pct: 10` (not 5; 5% is dev-machine tight), `p99_abs_ceiling_ms: 16.67` (the Pillar 1 floor — hard cap), `consecutive_run_required: 2` (a single noisy run won't fail the gate; two runs in a row must both regress). Configurable per scenario.

**D3 — Harness-agnostic by default.** All logic in `scripts/dev/*` (bash) + `scripts/dev/*.py` (Python). Hook wrappers in `docs/harness/<harness>/` are thin shims that call the shared script. The `agents/<name>.md` files are canonical per agents.md spec — no harness reads its own copy.

**D4 — PR-fast subset, not full suite per PR.** Full 14-scenario suite is ~10 min CI time. PR-fast subset is 4 scenarios (Idle, PriorityGridScroll, CommandPaletteFuzzy, CellEditBurst) chosen for broadest coverage of the active code paths. Budget target: < 5 min wall-clock on top of the existing build step. Full suite runs on `develop` push + scheduled cron, where the minute cost is amortised.

**D5 — Pillar 2 migration is one-shot in Slice 2.** Per the earlier user choice — Slice 2 includes a migration pass that annotates every existing safe sync-I/O site with `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` so the strict gate lands with zero pre-existing violations. Scope from the audit doc: 15 confirmed + estimated 10-20 more from a fresh grep; total ~25-35 annotations. Treated as an L-effort step within Slice 2.

## Components

| # | Name | Tier | Effort | Files touched / created | Reuses |
|---|------|------|--------|------------------------|--------|
| C1 | Baseline registry | P0 | M | `docs/perf/baselines/<scenario>.<host>.json` (× scenarios × hosts), `scripts/dev/perf-baseline.sh`, `scripts/dev/perf-baseline-schema.json`, `docs/perf/regression-policy.json` | `perf.snapshot` JSON shape, `scenario.run` |
| C2 | Pillar 2 static scanner | P0 | M | `scripts/dev/pillar2-scan.sh` (canonical), `docs/harness/claude-code/hooks/lint-cpp-pillar2.sh` (Claude-Code wrapper), `docs/harness/codex/hooks-equivalent.md` (Codex invocation notes), `docs/harness/cursor/hooks-equivalent.md` (Cursor invocation notes). Mods to `docs/harness/claude-code/hooks/lint-cpp-drain.sh` (canonical template, mirrors live `.claude/hooks/lint-cpp-drain.sh` via setup-harness). | `lint_is_first_party` + `lint_format_issues` from `lint-cpp-common.sh`; reused dedup queue pattern |
| C3 | Dispatcher drain instrumentation | P0 | S | `Source_Core/include/MainThreadDispatcher.h` (wrap `Drain()` body in `SMATCHET_UI_PERF_SCOPE("dispatcher.drain")`); `Source_Core/src/Commands/Builtin/BuiltinCommands_Perf.cpp` if a `LastDrainTaskCount()` accessor is exposed | `SMATCHET_UI_PERF_SCOPE` macro from `Source_Core/include/UiPerfMonitor.h` |
| C4 | Shared perf-run driver + compare | P0 | M | `scripts/dev/perf-run.sh`, `scripts/dev/perf-compare.py` | `--spawn` ephemeral-instance pattern from `scripts/dev/test-grid-edit-perf-postfix.sh`; nlohmann/json output shape |
| C5 | PR-fast CI workflow | P1 | M | `.github/workflows/perf-pr-fast.yml`, `scripts/dev/perf-pr-fast-set.json` (4-scenario subset declaration) | `.github/workflows/build-and-test.yml` template, `perf-run.sh`, `perf-compare.py` |
| C6 | Full-suite scheduled workflow | P1 | S | `.github/workflows/perf-full.yml` | C5 driver scripts; `gh issue create` / `gh pr create` |
| C7 | `perf-gatekeeper` agent + skill alias | P1 | M | `agents/perf-gatekeeper.md` (canonical, agents.md spec); `agents/_shared/skills/perf-gatekeeper/SKILL.md` (Claude-Code skill alias — generated path; the canonical agents/ file is what Codex / Cursor read) | `perf-measure` measurement loop; `perf-detective` heuristics for diff → affected-scenario classification |
| C8 | Subsystem marker inventory | P2 | S | `scripts/dev/perf-marker-inventory.sh`, `docs/perf/MARKER_INVENTORY.md` | text-search only |
| C9 | Visible-cue assertion harness (bucket-E) | P2 | L | `tests/ui/sync_stall_visible_cue.test.cpp` (`IM_REGISTER_TEST`), `scripts/dev/test-ui-sync-stall-visible-cue.sh`, `#ifdef SMATCHET_DEBUG_VISIBLE_CUE_HARNESS`-gated stall hook in one chosen sync path (icon fetch is the simplest target); `ninja-ui-test-msvc` preset gains `SMATCHET_DEBUG_VISIBLE_CUE_HARNESS=1` | bucket-E ImGui Test Engine rig (see `docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`); pattern from `tests/ui/views_columns_reorder.test.cpp` |
| C10 | Sync-call latency comment lint | P2 | S | extends C2's scanner — same script | shared with Pillar 2 lint pass |
| C11 | Process mandate update | P0 | S | `AGENTS.md` (§ UX Pillars cross-link to the registry + workflows), `docs/guides/perf-workflow.md` (add § "Step 7: gate-check vs baseline"); close + archive existing § Perf process/P2 backlog entry from 2026-05-20 in `docs/backlog/agent-self-improvement/process.md` → `applied.md` | existing backlog format |
| C12 | Pillar 2 site annotation migration | P0 | L | bulk-add `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` annotations to existing safe sync-I/O sites; scope ~25-35 lines across ~10-15 files (audit doc as ground truth + fresh scanner output as the working list) | the audit doc lists the confirmed-safe sites |

## Slice plan

### Slice 1 — Baseline + driver foundation
**Ships**: C1, C4, C11.

**Outcome**: any agent (any harness) runs `bash scripts/dev/perf-run.sh <scenario_id>` locally → compares against checked-in baseline → emits markdown delta table. CI runner captures its own baselines on first run.

**Steps**:
1. Define `scripts/dev/perf-baseline-schema.json` and `docs/perf/regression-policy.json`. Per-scenario file shape: `{schemaVersion: 1, scenarioId, captureCommit, captureDate, captureHost, captureRunnerOs, target: {meanMs, p99Ms, maxMs}, rows: [{name, lastTotalMs, p99Ms, calls}], hardwareNote, perScenarioPolicyOverride: nullable}`.
2. Implement `scripts/dev/perf-run.sh` — pure bash. Builds `ninja-iter-msvc` if `.claude/.tree-dirty` exists (or always when `--clean`); spawns ephemeral instance via `Smatchet.exe --spawn`; calls `perf.reset` + `scenario.run --id X --outPath <tmp>`; parses JSON. Bash-only, runs identically under Codex / Cursor.
3. Implement `scripts/dev/perf-compare.py` — Python 3 (Python is already required per the recent tooling/P2 backlog entry). Loads baseline JSON + new snapshot, computes mean / p99 / max delta percentages, emits markdown table to stdout, exit code 1 on regression > thresholds from `regression-policy.json`. Tolerance band: default `mean_delta_pct: 10`, `p99_abs_ceiling_ms: 16.67`, `consecutive_run_required: 2`.
4. Implement `scripts/dev/perf-baseline.sh` — `init <scenario>` captures fresh baseline; `bump <scenario>` diffs vs current baseline, asks confirmation (non-interactive `--yes` flag for CI use), writes update; `list` shows current baseline inventory.
5. Capture initial `dev`-host baselines for all 14 scenarios on current `develop` HEAD locally. Commit JSON files to `docs/perf/baselines/`. (CI-runner baselines populate themselves on Slice 3's first run via a one-shot `bump` step.)
6. Update `AGENTS.md` § UX Pillars with cross-links to `scripts/dev/perf-run.sh` + `docs/perf/baselines/`. Add `docs/guides/perf-workflow.md` § "Step 7: gate-check vs baseline" with usage examples for every harness. Close the existing § Perf process/P2 backlog entry from 2026-05-20 → archive to `applied.md` per the established workflow.

**Verification**: `bash scripts/dev/perf-run.sh idle` → JSON written. `bash scripts/dev/perf-compare.py docs/perf/baselines/idle.dev.json /tmp/idle-latest.json` exits 0. Inject a synthetic 2 ms `std::this_thread::sleep_for` into the idle render path → re-run → assert exit code 1 + markdown table identifies the regression. Revert.

### Slice 2 — Pillar 2 static gate + dispatcher instrumentation + site annotation migration
**Ships**: C2, C3, C10, C12.

**Outcome**: PRs that introduce new sync-on-UI calls fail the lint pipeline with `file:line` evidence. Existing safe sites are annotated so the strict gate lands clean. Dispatcher drain time visible in `perf.snapshot`.

**Steps**:
1. **C3 — dispatcher instrumentation**: wrap `MainThreadDispatcher::Drain()` body (lines 54-66 of `Source_Core/include/MainThreadDispatcher.h`) in `SMATCHET_UI_PERF_SCOPE("dispatcher.drain")`. Add a `LastDrainTaskCount()` accessor (returns the size of the locally-swapped tasks vector, captured into an atomic for thread-safe read). Expose via `perf.snapshot` row `dispatcher.drain_tasks`. Surgical: ~10 lines including the accessor.
2. **C2 — canonical scanner**: implement `scripts/dev/pillar2-scan.sh`. For each argument file: text-search whether it's UI-reachable (heuristic — name matches `*Ui*.cpp`, includes `<imgui.h>`, or appears in a header trace from any `*Ui*.cpp`). If reachable, run `rg --pcre2 '\b(cpr::|SQLite::Database|popen\(|system\(|std::ifstream\s+\w+\s*\(|std::mutex.*\.lock\(\))'` on the file. For each hit: emit CRITICAL unless the line is preceded within 3 lines by `/* PILLAR2_WORKER_ONLY */` AND a `// est-latency: <N>ms` (or `<N>s`) comment. C10 is the latency-comment check, folded into the same scanner.
3. **Harness wrappers**: 
   - `docs/harness/claude-code/hooks/lint-cpp-pillar2.sh` — thin shim invoked from `lint-cpp-drain.sh`. Iterates the per-chunk file list, calls `bash scripts/dev/pillar2-scan.sh <file>...`.
   - `docs/harness/codex/hooks-equivalent.md` — documents how a Codex run should invoke the scanner (e.g. via a `codex.toml` pre-commit hook or a `task.json` hook entry — Codex doesn't have a hook system equivalent to Claude Code's `PostToolUse`, so the doc captures whichever Codex mechanism is closest).
   - `docs/harness/cursor/hooks-equivalent.md` — same for Cursor.
   - All three thin paths funnel into the SAME canonical script. `scripts/setup-harness.sh` copies the Claude-Code wrapper into `.claude/hooks/` on `setup-harness.sh claude-code`.
4. **C12 — migration pass**: run `bash scripts/dev/pillar2-scan.sh` against every first-party file. For each hit:
   - If the audit doc (`docs/plans/shipped/pillar-1-2-audit-2026-05-17.md`) has it as already-on-worker → annotate with `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` referencing the audit's per-site rationale.
   - If new / unreviewed → triage. Per PR #191, most remaining hits should be worker-only. Anything genuinely on UI → open a bug entry in `docs/backlog/` and exempt the line with a `// TODO(pillar2): tracked in <issue>` marker that the scanner emits as WARN (not CRITICAL).
   - Commit migration as a single atomic step within Slice 2 so the strict gate lands with zero pre-existing CRITICAL violations.

**Verification**:
- `bash scripts/dev/pillar2-scan.sh Source_Core/src/SmatchetFieldIconRender.cpp` returns clean post-migration.
- Synthetic test: add a `cpr::Get(...)` line to any `*Ui*.cpp` file without the annotation → run the scanner → assert CRITICAL with `file:line`. Revert.
- `dispatcher.drain` row appears in `perf.snapshot` output under any scenario.
- Lint pipeline drain on a touch-and-revert of an existing Pillar 2 site emits zero issues.

### Slice 3 — PR-fast CI gate
**Ships**: C5.

**Outcome**: every PR gets an automated perf delta comment within ~5 min of build completion; auto-fail on threshold violations.

**Steps**:
1. Declare PR-fast subset in `scripts/dev/perf-pr-fast-set.json`: `["idle", "priority-grid-scroll", "command-palette-fuzzy", "cell-edit-burst"]`. Four scenarios chosen for coverage breadth across the most active code paths. Estimated wall-clock: < 90 s combined post-build (verify in slice — if it overruns, drop one).
2. Implement `.github/workflows/perf-pr-fast.yml`. Triggers: `pull_request` (with `paths-ignore` for pure-docs diffs that match the existing pure-docs-skip envelope). Steps: setup MSVC toolchain (mirror `build-and-test.yml`); build `ninja-iter-msvc`; for each scenario in the subset, run `scripts/dev/perf-run.sh <id>`; assemble snapshots into one markdown table via `scripts/dev/perf-compare.py --all-from <dir>`; upload JSON artefacts; `gh pr comment $PR --body @perf-report.md`.
3. First-run bootstrap: when no `<scenario>.ci-windows-latest.json` baseline exists yet, the workflow runs in "capture mode" — it writes the baseline to a workflow-artefact + opens a PR that adds it under `docs/perf/baselines/`. After human review + merge, the gate becomes active. This handles the chicken-and-egg of "need a baseline before you can compare."
4. Add `perf-out-of-band` PR label that downgrades regression failures to warnings (mirrors the existing `tests-out-of-band` label pattern from AGENTS.md § Merge gates).
5. Document the override path + the bootstrap flow in `AGENTS.md` § Merge gates as a new sub-section.

**Verification**: open a draft PR that intentionally regresses one scenario (add `std::this_thread::sleep_for(std::chrono::milliseconds(2))` to the idle path) → CI must auto-fail with a delta table identifying the regression. Revert + confirm green. Test override: add `perf-out-of-band` label → CI marks the same regression as WARN, doesn't block.

### Slice 4 — Full-suite scheduled workflow + agent automation + marker inventory
**Ships**: C6, C7, C8.

**Outcome**: scheduled full-suite coverage on `develop`; `perf-gatekeeper` agent/skill available for on-demand scenario-aware PR review (any harness); marker inventory regenerated as a checked-in doc.

**Steps**:
1. **C6 — full-suite workflow**: `.github/workflows/perf-full.yml`. Scheduled `cron: '0 6 * * 1-5'` (weekday morning UTC) + `workflow_dispatch` for on-demand. Runs all 14 scenarios via `perf-run.sh`. On regression: opens an issue tagged `perf-regression` with the delta table. On improvement: opens a PR via `perf-baseline.sh bump --yes` against the affected baseline file; human reviews + merges.
2. **C7 — perf-gatekeeper agent**: `agents/perf-gatekeeper.md` is the canonical (agents.md-spec) definition — any harness reads it. Curated diff → affected-scenario map embedded in the agent body. Example mappings: `Source_Core/src/SmatchetActiveProjectGridUi.cpp` → `priority-grid-scroll`; `Source_Core/src/SmatchetCommandPaletteUi.cpp` → `command-palette-fuzzy`; `Source_Core/src/SmatchetAiAssistantUi.cpp` → `ai-assistant-big-prompt-stream` (if registered, else fall back to `idle`); `Source_Core/src/SmatchetTheme.cpp` → `theme-switch-roundtrip`. Skill alias mirror at `agents/_shared/skills/perf-gatekeeper/SKILL.md` for Claude Code (auto-generated by `setup-harness.sh claude-code`; not authored separately).
3. **C8 — marker inventory**: `scripts/dev/perf-marker-inventory.sh` greps for all `SMATCHET_UI_PERF_SCOPE(` call sites; separates `perf_temp:*` (in-flight) from committed markers; emits markdown table grouped by subsystem to `docs/perf/MARKER_INVENTORY.md`. Wire into `scripts/dev/test-all.sh` as an advisory check (writes the file; if it differs from the checked-in version, prints a one-line WARN — does not fail).

**Verification**: `gh workflow run perf-full.yml --ref develop` → completes within budget; on synthetic regression posts an issue; on synthetic improvement opens a baseline-bump PR. Invoke `perf-gatekeeper` (skill or agent form depending on harness) against this slice's PR diff → picks at least one affected scenario, runs `perf-run.sh`, posts a delta comment.

### Slice 5 — Visible-cue assertion harness (bucket-E ImGui Test Engine)
**Ships**: C9.

**Outcome**: a bucket-E ImGui Test Engine assertion verifies the < 100 ms visible-cue invariant under a synthetic 250 ms sync stall on one chosen sync path.

**Builds on bucket-E** — wired 2026-05-19 per `docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`. Pattern (mirroring `tests/ui/views_columns_reorder.test.cpp`): test source at `tests/ui/<name>.test.cpp` with `IM_REGISTER_TEST(...)`; bash driver at `scripts/dev/test-ui-<name>.sh`; build + run via `cmake --build --preset ninja-ui-test-msvc`; auto-enrolled by `scripts/dev/test-all.sh` per the existing `test-ui-*.sh` convention.

**Steps**:
1. Add a debug-flag-gated stall hook in **one** chosen sync path. Recommended target: icon fetch in `Source_Core/src/SmatchetFieldIconRender.cpp` (single call site; spinner cue already implemented). Wrap the relevant block in `#ifdef SMATCHET_DEBUG_VISIBLE_CUE_HARNESS`: if `g_smatchetSyncStallActive` (extern atom), call `std::this_thread::sleep_for(std::chrono::milliseconds(250))` and clear the flag. Production builds never compile the sleep.
2. Implement `tests/ui/sync_stall_visible_cue.test.cpp`. Use `IM_REGISTER_TEST` mirroring the existing `tests/ui/views_columns_reorder.test.cpp` shape. Test body:
   - Set `g_smatchetSyncStallActive = true`.
   - Invoke the icon-fetch code path that contains the gated stall.
   - Sample frames for ≤ 300 ms wall-clock; on each sampled frame use `ImGui::FindWindowByName` / per-frame `ImDrawData` walk (or the bucket-E `ImGuiTestContext` window-query helpers) to detect the spinner widget.
   - `IM_CHECK(cueAppearedWithinMs <= 100)`.
3. Add the bash driver `scripts/dev/test-ui-sync-stall-visible-cue.sh` — builds `ninja-ui-test-msvc`, runs the registered test, exits non-zero on `IM_CHECK` failure. Auto-enrols via `scripts/dev/test-all.sh` per the existing `test-ui-*.sh` pattern.
4. Define `SMATCHET_DEBUG_VISIBLE_CUE_HARNESS` in the `ninja-ui-test-msvc` preset so only UI-test builds compile the stall hook. Production / iter / publish builds remain untouched.
5. Wire into the full-suite workflow (C6) as an optional final-stage step that runs `bash scripts/dev/test-ui-sync-stall-visible-cue.sh`; don't add to PR-fast (would slow the gate + needs the UI-test build).

**Verification**: `bash scripts/dev/test-ui-sync-stall-visible-cue.sh` exits 0 against the unchanged build. Synthetic break: comment out the spinner widget in the icon-fetch path → re-run → `IM_CHECK` fails with the "cue did not appear within 100 ms" message → exit 1.

## Critical files

**To modify (existing files)**:
- `AGENTS.md` (§ UX Pillars cross-links — Slice 1; § Merge gates `perf-out-of-band` label — Slice 3)
- `docs/guides/perf-workflow.md` (Step 7 baseline gate — Slice 1)
- `docs/harness/claude-code/hooks/lint-cpp-drain.sh` (canonical template; live `.claude/hooks/lint-cpp-drain.sh` mirrors via setup-harness — Slice 2)
- `Source_Core/include/MainThreadDispatcher.h` (Slice 2; dispatcher.drain scope + LastDrainTaskCount accessor)
- `Source_Core/src/Commands/Builtin/BuiltinCommands_Perf.cpp` (optional — Slice 2, if exposing LastDrainTaskCount via `perf.snapshot`)
- `Source_Core/src/SmatchetFieldIconRender.cpp` or chosen sync-path file (Slice 5 stall-hook insertion, `#ifdef SMATCHET_DEBUG_VISIBLE_CUE_HARNESS`-gated)
- `CMakePresets.json` (Slice 5 — add `SMATCHET_DEBUG_VISIBLE_CUE_HARNESS=1` to the existing `ninja-ui-test-msvc` preset's cache variables)
- `tests/ui/CMakeLists.txt` (Slice 5 — register `sync_stall_visible_cue.test.cpp` alongside the existing bucket-E test entries)
- `docs/backlog/agent-self-improvement/process.md` + `applied.md` (close + archive 2026-05-20 § Perf entry — Slice 1)
- `scripts/dev/test-all.sh` (Slice 4 marker-inventory advisory hook)
- ~25-35 existing sync-I/O sites across ~10-15 files for `/* PILLAR2_WORKER_ONLY */` + `// est-latency:` annotations (Slice 2 migration; specific list emerges from running the scanner)

**To create (new files)**:
- `docs/perf/baselines/<scenario>.<host>.json` — one file per scenario × host pair (Slice 1 captures dev; Slice 3 bootstrap captures ci-windows-latest)
- `docs/perf/regression-policy.json` (Slice 1)
- `docs/perf/MARKER_INVENTORY.md` (Slice 4; regenerated)
- `scripts/dev/perf-run.sh`, `scripts/dev/perf-compare.py`, `scripts/dev/perf-baseline.sh`, `scripts/dev/perf-baseline-schema.json` (Slice 1)
- `scripts/dev/pillar2-scan.sh` (Slice 2 canonical scanner — harness-agnostic)
- `scripts/dev/perf-marker-inventory.sh` (Slice 4)
- `docs/harness/claude-code/hooks/lint-cpp-pillar2.sh` (Slice 2 Claude-Code wrapper — copied into `.claude/hooks/` by setup-harness)
- `docs/harness/codex/hooks-equivalent.md` (Slice 2 — documents Codex invocation of the canonical scanner)
- `docs/harness/cursor/hooks-equivalent.md` (Slice 2 — same for Cursor)
- `.github/workflows/perf-pr-fast.yml`, `scripts/dev/perf-pr-fast-set.json` (Slice 3)
- `.github/workflows/perf-full.yml` (Slice 4)
- `agents/perf-gatekeeper.md` (Slice 4 — canonical agents.md-spec file; Claude-Code skill alias at `agents/_shared/skills/perf-gatekeeper/SKILL.md` is generated by `setup-harness.sh`, not authored separately — same pattern as `perf-instrument` / `perf-measure`)
- `tests/ui/sync_stall_visible_cue.test.cpp` + `scripts/dev/test-ui-sync-stall-visible-cue.sh` (Slice 5 — bucket-E ImGui Test Engine test + auto-enrolled bash driver)

## Reused utilities

- `SMATCHET_UI_PERF_SCOPE` macro from `Source_Core/include/UiPerfMonitor.h` line 66 (Slice 2 + 5)
- `scenario.run --spawn` from `Source_Core/src/Commands/Builtin/BuiltinCommands_Scenario.cpp` (Slices 1, 3, 4, 5)
- `perf.snapshot` JSON shape from `BuiltinCommands_Perf.cpp` (Slice 1 baseline format)
- `lint_is_first_party` + `lint_format_issues` from `docs/harness/claude-code/hooks/lint-cpp-common.sh` (Slice 2 scanner)
- `--spawn` ephemeral-instance pattern from `scripts/dev/test-grid-edit-perf-postfix.sh` (Slice 1 driver template)
- `MainThreadDispatcher::PostToMainThread` invariants from `MainThreadDispatcher.h` (Slice 2 instrumentation)
- `perf-detective` agent's "named scenario → top-N report" loop from `agents/perf-detective.md` (Slice 4 `perf-gatekeeper`)
- Audit doc baseline of safe sites at `docs/plans/shipped/pillar-1-2-audit-2026-05-17.md` (Slice 2 migration ground truth)
- `scripts/setup-harness.sh` (existing — Slice 2 picks up the new Claude-Code wrapper automatically via the existing copy-template mechanism)

## Verification (end-to-end)

After all 5 slices land:

1. **Harness-agnostic invocation**: `bash scripts/dev/perf-run.sh idle` works identically under Claude Code, Codex, and Cursor (no harness-specific glue required). `bash scripts/dev/pillar2-scan.sh <file>` likewise.
2. **Baseline registry round-trip**: `bash scripts/dev/perf-run.sh idle` → JSON written; `perf-compare.py` exits 0 vs unchanged code; inject a 2 ms sleep → exit 1 + markdown table identifies the regression; revert + green.
3. **Pillar 2 static gate**: add `cpr::Get(...)` to a `*Ui*.cpp` without annotation → end-of-turn lint emits CRITICAL with `file:line`; add `/* PILLAR2_WORKER_ONLY */ // est-latency: 150ms` → passes; remove → fails again.
4. **PR-fast CI**: draft PR with deliberate regression in any of the 4 PR-fast scenarios → CI auto-comments delta + fails; remove regression → green; apply `perf-out-of-band` label → regression downgrades to WARN.
5. **Full-suite scheduled run**: `gh workflow run perf-full.yml` → completes; on synthetic regression posts an issue; on synthetic improvement opens a baseline-bump PR.
6. **perf-gatekeeper agent**: invoke against a real PR diff (any harness) → picks affected scenarios from the curated map → runs them via `perf-run.sh` → posts delta comment.
7. **Visible-cue harness (bucket-E)**: `bash scripts/dev/test-ui-sync-stall-visible-cue.sh` (builds `ninja-ui-test-msvc`, runs the `IM_REGISTER_TEST`-registered test) exits 0 against the unchanged build; comment out the spinner widget in the icon-fetch path → re-run → `IM_CHECK` fails → exit 1.
8. **Marker inventory**: `bash scripts/dev/perf-marker-inventory.sh` regenerates `docs/perf/MARKER_INVENTORY.md`; advisory WARN if the live tree contains uncommitted `perf_temp:*` markers (perf-detective sessions in flight).
9. **Dispatcher visibility**: `dispatcher.drain` row appears in `perf.snapshot` under every scenario; baseline files include it under `rows[]`.

## Out of scope (flagged, not designed)

- **Scenario authoring via Lua DSL** (G1.3) — XL effort, deferred to a follow-up plan. Add a backlog note after Slice 4 lands.
- **Broader bucket-E coverage for the perf-review system itself** — Slice 5 uses the rig directly for the visible-cue assertion, but adjacent perf-flows (e.g. PR-fast workflow UI for delta-report rendering, perf-gatekeeper status surfaces) are not bucket-E-tested in this plan. Add follow-up tests opportunistically; not gating.
- **Cross-OS perf comparisons** — Windows-only. Baseline schema records `captureRunnerOs` so the structure can later extend to Linux/Mac CI without a schema migration.
- **User-side telemetry / opt-in profiling** — out of scope (privacy + design surface).
- **Refactoring existing scenarios** — Slice 1 captures whatever the existing scenarios emit. Unstable / non-representative scenarios are a separate concern.
- **Hardware variance investigation** — the 10 % mean tolerance + 2-consecutive-run requirement is a starting point; if CI false-positive rate is high after Slice 3 ships, tune the thresholds rather than redesign.

## Implementation log

- Slice 1 (PR #321) · baseline registry + driver scripts; `docs/perf/regression-policy.json` + per-host baseline schema; `scripts/dev/perf-{run,compare,baseline}.{sh,py}`.
- Slice 2 (PR #322) · Pillar 2 static gate (`scripts/dev/pillar2-scan.sh`) + dispatcher.drain marker + `LastDrainTaskCount()` accessor + 7-site PILLAR2_WORKER_ONLY annotation migration + 3 TODO(pillar2): tracked sites.
- Slice 3 (PR #323) · `.github/workflows/perf-pr-fast.yml` + PR-fast 4-scenario subset + `perf-out-of-band` override label + first-run baseline bootstrap.
- Slice 4 (PR #324) · `.github/workflows/perf-full.yml` (cron + workflow_dispatch) + `agents/perf-gatekeeper.md` (+ Claude Code skill alias) + `scripts/dev/perf-marker-inventory.sh` regenerating `docs/perf/MARKER_INVENTORY.md`.
- Slice 5 (PR #325) · `tests/ui/sync_stall_visible_cue.test.cpp` (bucket-E IM_REGISTER_TEST GoodOrder + BadOrder variants) + `scripts/dev/test-ui-sync-stall-visible-cue.sh` + `SMATCHET_DEBUG_VISIBLE_CUE_HARNESS` cache var on `ninja-ui-test-msvc` preset + optional final-stage step in `.github/workflows/perf-full.yml`.

## Deviations from plan

- **Slice 5 stall hook lives in the test source, not in `SmatchetFieldIconRender.cpp`** — the icon-fetch HTTP / file paths already run on a worker thread (both call sites are inside `app.LaunchBackgroundTask(...)` lambdas at `Source_Core/src/SmatchetFieldIconRender.cpp:309` and `:344`). Injecting `std::this_thread::sleep_for(...)` there would stall the worker, not the UI thread, and would not test the visible-cue invariant. The stall hook moved into `tests/ui/sync_stall_visible_cue.test.cpp` itself, which mirrors the production cue-before-block call-site shape in a test-engine-owned window (same pattern as `views_columns_reorder.test.cpp` / `callstack_tooltip_hover.test.cpp`). The bucket-E test still validates the architectural invariant — submission order of cue vs block — without touching production code.
- **Order-of-submission assertion, not wall-clock measurement** — the original plan asked for `IM_CHECK(cueAppearedWithinMs <= 100)`. ImGui Test Engine drives frames in stepped mode, so wall-clock measurements are unreliable. Replaced with an atomic ordering sentinel (`g_cueOrder` ∈ {kUnobserved, kCueBeforeStall, kStallBeforeCue}) — same architectural invariant, deterministic in stepped frames. The Good variant asserts kCueBeforeStall; the Bad variant asserts kStallBeforeCue (documents what the regression looks like; protects against future inversion of cue/block order).
- **Stall duration 50 ms, not 250 ms** — sleep duration trimmed to keep the bucket-E run quick. The invariant under test is ordering, not duration; 50 ms is enough to be humanly observable in real UI under the same pattern.
- **Compile-definition wired via `set_source_files_properties`** — the `SMATCHET_DEBUG_VISIBLE_CUE_HARNESS=1` flag is scoped to `sync_stall_visible_cue.test.cpp` only (via `set_source_files_properties` in `tests/ui/CMakeLists.txt`), gated on a `SMATCHET_DEBUG_VISIBLE_CUE_HARNESS` CMake option that the `ninja-ui-test-msvc` preset turns ON. Production / iter / publish builds never compile the synthetic sleep.

## Verification

- Slice 1 — `bash scripts/dev/perf-run.sh idle` writes a JSON snapshot; `perf-compare.py` against the dev baseline exits 0. Synthetic regression injection round-trip verified end-to-end. **passed**.
- Slice 2 — `bash scripts/dev/pillar2-scan.sh` returns zero CRITICAL after the 7-site annotation migration. `dispatcher.drain` row appears in `perf.snapshot` output. Synthetic CRITICAL injection (`cpr::Get` in a `*Ui*.cpp`) caught by the scanner. **passed**.
- Slice 3 — `.github/workflows/perf-pr-fast.yml` validated by YAML lint + workflow syntax checks; live behaviour verifies on the merge of PR #323 itself. **pending CI**.
- Slice 4 — `bash scripts/dev/perf-marker-inventory.sh` regenerates `docs/perf/MARKER_INVENTORY.md` cleanly (52 committed, 0 perf_temp:*). `agents/perf-gatekeeper.md` reviewed; skill alias mirrors content. Full-suite workflow validated by YAML + script-shape inspection; full live run pending first scheduled cron tick. **pending CI**.
- Slice 5 — `tests/ui/sync_stall_visible_cue.test.cpp` compiles under `ninja-ui-test-msvc` preset; bash driver exits 0 against the GoodOrder + BadOrder variants. Synthetic break (invert cue/block order in the Good variant) caught by the IM_CHECK. **pending CI build**.
