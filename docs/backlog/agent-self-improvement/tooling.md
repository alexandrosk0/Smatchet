# Agent self-improvement — tooling

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

## Triage log

- **2026-05-18** — Triage sweep per [`docs/design/agent-docs-improvements.md`](../../design/agent-docs-improvements.md) § Action 2. Cadence trigger: category breached the ~20 open-items threshold (was 27).
  - Before: 27 entries
  - Dedup-merged: 1 (Bucket-E tooltip-content-identity helper — 2026-05-17 entry kept as survivor with `Supersedes: 2026-05-16` line; 2026-05-16 sibling dropped)
  - Moved-to-parked: 16 (all P3 entries — see `## Parked` section at the bottom)
  - Escalated-to-external-blockers: 0 (`Headless CI runners` + `Install gitleaks/semgrep/flawfinder` evaluated; neither qualified — both are in-repo workflow / setup-script edits, not upstream)
  - After live: 10 P2 entries; parked block holds 16 P3 entries

<!-- Latest first. Append new P0 / P1 / P2 entries at the top. Append new P3 entries to ## Parked. -->

- 2026-05-21 · orchestrator · [tooling] · P1 — `scripts/dev/merge-watcher.py daemon` mis-reports gates outcome as `GH_API_DOWN` when underlying `merge-gates.sh` returns a real blocked state (CI fail / CR findings / non-zero exit ≠ 3); auto-merge can never fire because the wrapper never sees the real status
  Details: Session 2026-05-21 PR #369. User picked post-ship option 3 "Register with watcher". `merge-watcher-cli.py register 369` succeeded. `merge-watcher.py daemon` polled the gates (PID still alive at session end, last poll 20:09:19 with multiple polls since registration at 19:48). Registry status reads `GH_API_DOWN` on every poll. Direct invocation of `ORCH_USER=$(gh api user --jq .login) bash scripts/dev/merge-gates.sh alexandrosk0 Smatchet 369` returned real data: `Poll N/60 — CI: 2/4 pass (2 fail, 0 pending) | CodeRabbit: COMMENTED (3 actionable — block) (3 open) | User: 0 | reviewDecision: NONE`. So the gates script itself works; the daemon wrapper swallows the structured stdout and tags it `GH_API_DOWN`. Net effect: watcher cannot auto-merge — it never sees a green poll, never sees a real CR-findings count to drive triage, never escalates a real CI-failure halt prompt to the user. The whole post-ship-option-3 path is currently a no-op for `develop`-clone PRs.
  Concrete next action: trace `scripts/dev/merge-watcher.py daemon` poll-loop path. Three likely fault sites: (a) the wrapper invokes `merge-gates.sh` without `ORCH_USER` exported in the daemon env → script exits with `repo required` / `ORCH_USER not set` (we hit both via direct call this session) → wrapper interprets non-zero exit as `GH_API_DOWN`. (b) the wrapper passes the PR number positionally but the script expects `owner repo pr` (signature drift between caller + callee — `bash scripts/dev/merge-gates.sh 369` exits with `poll_merge_gates: repo required` this session). (c) wrapper greps stdout for a magic token that the script no longer emits. Fix: (1) wrapper computes `owner` + `repo` from `gh repo view --json owner,name` once at startup, exports them. (2) Wrapper exports `ORCH_USER=$(gh api user --jq .login)` once at startup. (3) Wrapper invokes `merge-gates.sh $owner $repo $pr` with the full positional shape. (4) On non-zero exit, classify by stderr / exit code (1=blocked, 2=timeout, 3=api-down, 4=closed, 5=pagination, 6=ready-failed per AGENTS.md § Merge gates § Halt prompts). Today it appears to map every non-3 to `GH_API_DOWN`. Add bats coverage for each return-code → registry-state mapping. ~2 h: 30 min wrapper diff + 1 h bats + 30 min doc cross-link in `docs/design/smatchet-merge-watcher.md`. Wins on every registered PR.
  Status: open
  Last-reviewed: 2026-05-21

- 2026-05-21 · orchestrator · [tooling] · P1 — Long-running CI / CodeRabbit polls block the interactive session; should run out-of-band
  Details: This session (2026-05-21) spawned 6+ background polls (`scripts/dev/merge-gates.sh` shape, but inlined as ad-hoc Python invoked from Bash). Each poll runs 30-40 minutes waiting on CI + CR. Notifications fire back into the orchestrator's main session: each one consumes a conversation turn (reads the poll log, reasons about merge / triage / abort, commits + pushes if a fix is needed). Costs the user observed during the session:
    (1) **Context-budget burn** — even when the orchestrator is idle waiting, the session keeps the full PR context loaded; multi-PR fan-out (5 polls live at once during the github-tracker + code-color cascade) amplified this.
    (2) **Interruption pattern** — when the user kicked off new exploratory work mid-session ("double-check the plan", backlog entries, etc.), every poll notification yanked the orchestrator back to merge-bookkeeping. The user can't have a continuous conversation thread while polls are firing.
    (3) **TIMEOUT escalations** — when CR is silent (#350 saw 40 polls = 40 min with no review), the user has to make the same "force-merge or wait?" decision repeatedly across PRs. A central watcher could apply the policy once.
    (4) **Lossy on session crash** — bg polls die if the parent session closes. The user explicitly asked early in this session ("are we done?") whether they could close — current shape says no without losing in-flight polls.
  Concrete next action: design + ship `smatchet-merge-watcher` as a separate process — runs on the host outside any specific Claude Code session. Reads a registry of "actively-watched PR numbers" (file at `.claude/.merge-watch/active.json` or `%LOCALAPPDATA%/Smatchet/merge-watch/active.json`). Polls each PR per the merge-gates contract (CI + CR + STALE-aware + user-comments). On state change:
    - **PASS** → auto-`gh pr ready` (if draft) + REST squash-merge + cascade: detect stacked children via `gh pr list --search "base:<merged-branch>"`, pull develop into each, push, mark them PASS-ready in the registry.
    - **CR_BLOCKED** → if a sibling `coderabbit-triage` worker is registered, spawn a sub-session via `claude --headless` with the PR # + CR feedback as inline context; on triage completion, push + flip poll back to start.
    - **TIMEOUT_NO_CR** → log WARN + apply user-configured fallthrough policy (default: stay paused, send a Smatchet notification asking; opt-in: auto-force-merge).
    - **CI_FAIL** / **CONFLICT** / **USER_COMMENT** → pause the PR + push a Smatchet notification (uses the existing in-app notification surface from `SmatchetToastManager` + Lua-bindings + bash hook) so the user sees it on whichever device runs Smatchet.
  Integration with this session orchestrator: the main session registers PRs into the watcher's queue (`merge-watch register <pr>`), gets a registry-id, can query state (`merge-watch status <pr>`), or unregister. Session can exit at will; the watcher persists. Tests: bats around the registry + the state-transition logic; 1 integration test that walks a fake PR through PASS → cascade → merge using `gh api`-mocking.
  Acceptance: this session's exact pattern (5 PRs cascading + 1 forced timeout) runs end-to-end with zero interactive prompts to the orchestrator. ~6 h initial: 2 h watcher daemon (Python or bash), 1 h registry + cli, 1 h Smatchet-notification surface, 2 h bats + integration tests.
  Status: open
  Last-reviewed: 2026-05-21

- 2026-05-20 · orchestrator · [tooling] · P2 — 8 of 15 candidate perf scenarios don't emit `rows[]`; cannot be baselined under current shape
  Details: The 2026-05-20 Slice-1-baselines bootstrap attempt (now applied) captured only 7 of the 15 candidate scenarios named in the original 14-scenario list. The 8 excluded fall into 3 classes: (a) **bucket-C screenshot scenarios** requiring `--screenshotPath` — `command-palette-fuzzy`, `theme-switch-roundtrip`, `dock-gap-sentinel` (these scenarios fail to start without the arg; `perf-baseline.sh` doesn't pass one, and they're render-bound bucket-C scenarios anyway, not perf-rows-emitting). (b) **roundtrip / state scenarios** that run cleanly but don't include `UiPerfMonitor::Instance().GetLastFrameRows()` in their `OnFinish` payload — `agent-handoff-roundtrip`, `agent-triage-roundtrip`, `whisper-dictation-roundtrip`. (c) **scenarios that exist but don't emit rows** — `cell-edit-burst`, `whisper-ai-assistant-autosend`. Net: 7 scenarios under PR-fast / perf-gatekeeper coverage; 8 surfaces structurally invisible to the perf regression gate.
  Concrete next action: triage the 8 individually. For (a) bucket-C scenarios — confirm they're correctly classified as bucket-C-only and remove them from any perf-scenario lists (update `agents/perf-gatekeeper.md` § Curated diff → scenario map if they were listed there). For (b) + (c) — extend each scenario's `OnFinish` payload to include `rows[]` from `UiPerfMonitor::Instance().GetLastFrameRows()` (~10 lines per scenario, copy from `PriorityGridScrollScenario.cpp` OnFinish). Once a scenario emits rows[], re-run `bash scripts/dev/perf-baseline.sh init <scenario>` to capture its baseline. Net cost ~2 h for 5 row-emit retrofits + ~30 min triage of the bucket-C three. Until done, the 8 surfaces have no perf gate.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-20 · orchestrator · [tooling] · P2 — Release workflow does not fetch `fa-solid-900.ttf` (Font Awesome 6 Solid)
  Details: `assets/fonts/README.md` (shipped in PR #334 per `docs/design/ai-chat-claude-desktop-parity.md` § Phase 4) claims "CI fetches it on release-tag builds; dev workstations drop it manually." `assets/fonts/*.ttf` is gitignored (`.gitignore`) so the binary cannot live in-repo. Grep over `.github/` for `fa-solid-900` returns zero hits — no release / build workflow actually fetches the TTF. Shipped exes will start with `LOG_WARN: g_FaIconsLoaded = false` and the AI-chat hover action row + pin strip will render the text fallback (`Copy` / `Pin` / `×`) instead of icons. UX regression vs. plan intent, but **not** a build / runtime failure thanks to the `SmatchetAreFaIconsLoaded()` graceful-degradation path wired in `SmatchetAiAssistantUi.cpp`.
  Concrete next action: extend the release workflow (likely `.github/workflows/release.yml` and any `package-windows-*` job) to (1) `curl -L https://github.com/FortAwesome/Font-Awesome/raw/6.x/webfonts/fa-solid-900.ttf -o assets/fonts/fa-solid-900.ttf` before the CMake configure step, (2) let the existing POST_BUILD `copy_if_different` lay the TTF next to the exe, (3) make sure the packaging step picks the file up via the existing asset-bundling rule. Also add a configure-time `STATUS` message that reports whether the TTF was found vs. skipped, mirroring the README's contract. ~1 h.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-20 · orchestrator · [tooling] · P2 — Bucket-E live-PR end-to-end probe for coderabbit-react-loop
  Promoted from parked: 2026-05-19 — bucket-E (ImGui Test Engine) is wired per `docs/design/applied/imgui-test-engine-bucket-e-execution.md`; gating premise removed.
  Details: The closing milestone (phase 9 of `docs/design/coderabbit-react-loop.md`, sha `185418f`) shipped synthetic CLI smoke covering the dispatch logic but deferred the live-PR end-to-end probe documented in plan § Verification steps 3-4. Both react paths need a real PR with CodeRabbit feedback / a deliberately-bad CI commit to verify the full spawn → fix → push → resolve cycle end-to-end.
  Concrete next action: add ImGui Test Engine assertions for: (a) the two new Preferences UI toggles' keyboard-nav contract (`coderabbit_react.enabled` + `ci_react.enabled`), (b) the panel state-row reads for in-flight react-loop runs (per-PR iteration-budget snapshot, last-tick timestamp), (c) the `CHECK_RUN.json` sentinel surfacing in the agent-handoff UI panel. Register via `IM_REGISTER_TEST` in a new `tests/ui/coderabbit_react_loop.test.cpp` + bash driver `scripts/dev/test-ui-coderabbit-react-loop.sh`. ~3 h.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-20 · handoff-implementer · [tooling] · P2 — Bucket-E coverage for DeepSeek auto-clear "[model changed - chat cleared]" strip
  Promoted from parked: 2026-05-19 — bucket-E (ImGui Test Engine) is wired per `docs/design/applied/imgui-test-engine-bucket-e-execution.md`; gating premise removed.
  Details: `docs/design/deepseek-provider.md` § Verification plan flagged bucket-E as deferred at plan time. F2's pure-helper logic is covered by `tests/Source_Core/AiModelSignature.test.cpp` (6 scenarios, 168 assertions). The remaining gap is rendered-strip verification: after a Send-with-different-model the chat history clears + `g_ui.assistantLastError` paints `"[model changed - chat cleared]"` in the assistant panel's orange warning strip.
  Concrete next action: add `tests/ui/ai_assistant_model_change_strip.test.cpp` that (1) seeds `g_ui.assistantHistory` with one stub assistant message, (2) flips `cfg.AiProviderKind` between Anthropic and DeepSeek, (3) drives a synthetic Send through `AiClientFactory::SetTestOverride` returning a stub `IAiClient` that ack-streams a one-token reply, (4) asserts the strip renders the expected text after the second turn lands. Register via `IM_REGISTER_TEST` + bash driver `scripts/dev/test-ui-ai-assistant-model-change.sh`. ~2 h.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-20 · orchestrator · [tooling] · P2 — Bucket-E coverage for Preferences > Agentic tab (T7 residue)
  Promoted from parked: 2026-05-18 — bucket-E (ImGui Test Engine) is wired per `docs/design/applied/imgui-test-engine-bucket-e-execution.md`; gating premise removed.
  Details: T7 ships the scheduled-poll worker + Preferences "Agentic" tab (`SmatchetPreferencesUi.cpp` master toggle / interval / source / query / GitHub PAT / Run-now button). The worker thread itself is unit-test-hostile (std::thread + condition variable + 60..3600 s sleeps); we lean on `scripts/dev/test-agentic-triage-cli.sh` for the synchronous triage path the worker calls. The Preferences UI variants (toggle flip → RestartAgenticPoll, Run-now → LaunchBackgroundTask, last-poll/next-poll readout updates) are not exercised by any bucket — manual click verification today.
  Concrete next action: add `tests/ui/agentic_prefs_tab.test.cpp` (bucket-E) parallel to `tests/ui/agent_proposals_panel.test.cpp` covering: toggle-on-without-PAT (no thread spawned), toggle-on-with-PAT (thread spawned + joined on Stop), Run-now button (dispatches a background task), last-poll readout transition from "never" → time-ago string. Runner: `scripts/dev/test-ui-agentic-prefs.sh`. ~2 h.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-19 · perf-detective · [tooling] · P2 — `perf-measure --spawn` `WaitForFile` stale-file footgun
  Details: While investigating PR #311's markdown render perf, `perf-measure --spawn` returned the SAME numbers on three consecutive runs of two different scenarios. Diagnosed only after ~10 min of debugging that `WaitForFile` returns `true` for any non-empty file at the destination path, regardless of mtime. The previous run's result file was sitting at the same `outPath`; the CLI read it as the new run's output while the new spawn was still computing. Subsequent measurements landed but were attributed to the wrong scenario; the false "the instrumentation isn't firing" conclusion almost re-spent the time on adding more scopes.
  Concrete next action: in the perf-measure CLI / scripts/dev wrapper, do one of (a) `unlink(outPath)` before sending `scenario.run` so the race is eliminated upstream, or (b) `WaitForFile` records mtime at entry and requires either a fresh file or `mtime > entry-time` before returning true. Option (a) is simpler and won't surprise existing callers. ~30 min including a regression smoke (drop a stale file, run, assert it doesn't get picked up). Touches `Source_Core/src/Commands/Scenarios/ScenarioRunner.cpp` + scripts that wrap perf-measure.
  Status: open
  Last-reviewed: 2026-05-19

- 2026-05-19 · orchestrator · [tooling] · P2 — Cross-harness CI parity for skill-vs-agent forms
  Details: Surfaced while shipping `docs/design/agents-skill-conversion.md` v3 (`perf-instrument` / `perf-measure` dual-publish). The plan added Claude Code skill aliases at `agents/_shared/skills/perf-{instrument,measure}/SKILL.md` while keeping the agent files for Codex / Cursor. There is no CI rig today that runs Codex + Claude Code in lockstep against the same scenario to confirm functional parity between the two invocation forms. Risk: skill-form drift goes unnoticed until a cross-harness user files a bug. Verification V5 in the plan is currently a manual single-harness check.
  Concrete next action: add `scripts/dev/test-skill-vs-agent-parity.sh` that (a) writes a tiny driver script per harness that invokes the helper, (b) diffs the captured stdout / output file. Wire into `scripts/dev/test-all.sh`. Stretch: also assert the SKILL.md body matches the agent body modulo agent-only telemetry blocks (banner, `## Outcome`, `## Self-improvement`) so prose drift surfaces immediately. ~2 h initial wire-up; per-skill parity test ~15 min thereafter.
  Status: open
  Last-reviewed: 2026-05-19

- 2026-05-18 · test-author · [tooling] · P2 — `--spawn` swallows child stdout/stderr, blinds bucket-E failure diagnosis
  Details: Surfaced while wiring `tests/ui/whisper_ai_assistant_autosend.test.cpp` (PR #258). `LaunchEphemeralInstance` in `Target_Standalone/CliCommandRunner.cpp` (around line 267) inherits the parent's stdout/stderr but the parent's `--spawn` driver redirects both before the child starts. ImGui Test Engine's `ConfigLogToTTY` output, plus any `LOG_*` from the child, disappears — when a bucket-E test fails, the operator has no trail beyond the parent's exit code.
  Concrete next action: extend `LaunchEphemeralInstance` to capture child stdout + stderr to a per-spawn temp file (e.g. `$TMPDIR/Smatchet-spawn-<pid>.log`) and emit the path in the spawn banner the parent prints. Failure-path teardown should keep the file; success-path can delete it. ~1 h.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-18 · orchestrator · [tooling] · P2 — Plan-lock survives squash-merge of its PR
  Details: End-of-session cleanup found a stale `refs/locks/agentic-flow-cr-bundle-prod` lock owned by `handoff-implementer` on `fix/cr-handoff-bundle-prod`, even though that branch's PR (#271) had squash-merged hours earlier and the branch was deleted both remotely and locally. The `lock-slug: <slug>` auto-release token in the PR body is supposed to clear the lock on merge; either the PR body lacked the token, or the GitHub Action watching merge events doesn't fire on squash-merge specifically, or it errored silently. Manual recovery: `bash scripts/dev/lock-release.sh agentic-flow-cr-bundle-prod`. Cost: low per occurrence, but stale locks block sibling agents in subsequent sessions until somebody runs `locks-show.sh` and notices.
  Concrete next action: (1) audit `scripts/dev/setup-locks-ruleset.sh` + the corresponding workflow to confirm it triggers on `pull_request.closed` with `merged=true` (covers squash, rebase, merge), not just `push to default branch`. (2) Verify the `handoff-implementer` (`agents/handoff-implementer.md`) and bundle-PR templates always include `lock-slug: <slug>` in the body. (3) Add a `bash scripts/dev/lock-staleness-sweep.sh` invocation to the standard end-of-session cleanup checklist (`agents/git-janitor.md`). Estimated cost ~45 min audit + doc tweak.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-18 · git-janitor · [tooling] · P3 — Worktree cross-checkout cleanup gap: `[gone]` branches stranded in sibling worktrees
  Details: After the whisper PR train squash-merged, `git branch -D <branch>` from the worktree that opened the PR failed for branches the active worktree didn't own — `git-janitor` correctly refused to reach into sibling worktrees to do checkout / pull / delete. End result: each operator has to manually visit each worktree at `git worktree list`, ff-pull develop, then delete the stale branch. Multi-worktree setups (this repo has 6 active) compound the friction.
  Concrete next action: add `scripts/dev/worktree-prune.sh` that iterates `git worktree list`, for each worktree checks if HEAD branch is `[gone]`, ff-pulls develop, then deletes the stale branch. Refuses to act when worktree has uncommitted work (mirrors git-janitor's discipline). Document in `docs/agent-rules/DELEGATION.md` or `CONTRIBUTING.md` as the end-of-PR-train one-liner. ~45 min.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · test-author · [tooling] · P2 — `test-all.sh` baseline drift across worktrees: 8 fails on agent worktree vs 0 fails on main repo
  Details: Phase B (PR #163) agent reported 168 pass / 8 fail on isolated-worktree dispatch. Same `bash scripts/dev/test-all.sh` on main develop reports 173/0. The 8 worktree-only failures are pre-existing infra (lint-hook-split needs `.claude/hooks/` symlinked into the worktree root; 4 ui-test scripts have batched-PATH issues that pass when run individually). Causes false "regression" alarms in every PR that ships from a worktree, eroding signal.
  Concrete next action: either (a) `test-all.sh` auto-detects worktree context via `git rev-parse --git-common-dir` vs `git rev-parse --git-dir` and skips worktree-incompatible scripts with a CLEAR `SKIPPED (worktree)` line, or (b) `bash scripts/setup-harness.sh claude-code` extends to seed `.claude/hooks/` symlinks into newly-cut worktrees. Option (a) is simpler; option (b) is more thorough. ~2 h for either.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · test-author · [tooling] · P2 — Bucket-E tooltip-content-identity helper for production-driven hover tests
  Supersedes: 2026-05-16 (dedup-merged in the 2026-05-18 sweep — earlier entry restated the same gap against PR #154 instead of PR #156; both pointed at `tests/ui/callstack_tooltip_hover.test.cpp` and the same `BucketE::TooltipContentMatches` proposal).
  Details: While writing `tests/ui/callstack_tooltip_hover.test.cpp` (PR #156 — regression gate for #147) a production-driven variant 4 had to be dropped. A generic `##Tooltip_NN` window probe cannot distinguish "my cell's tooltip" from concurrent host-process tooltips. Even with `WindowFocus` + `NoDocking` + `ImGuiCond_Always` position pinning, production's `IsItemHovered()` against the cell rect returned false in the spawned-child host because something else in the shared `ImGuiContext` claimed `g.HoveredWindow`. Workaround taken: faithful replica of the production callstack path with a TU-local `tooltipFiredThisFrame` flag (same idiom as `views_columns_reorder.test.cpp`), plus a `NoGroupWrap` regression-shape variant that proves the methodology is sensitive to the wrap's presence. Sanity-checked end-to-end: removing the wrap from the replica fails variants 1+2 deterministically.
  Concrete next action: add a `BucketE::TooltipContentMatches(ctx, sentinel)` helper to `tests/ui/` (or shared `tests/ui/_helpers/`) that walks a tooltip window's `DrawList`'s `CmdBuffer` for a text command containing `sentinel`. Use it to distinguish "my cell's tooltip" from concurrent tooltips by feeding a unique marker through `rawForTooltip`. Once available, retrofit `callstack_tooltip_hover.test.cpp` with a variant 4 that drives real `RenderClippedFieldText` and asserts content match — closing the production-drift gap the replica can't cover.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [tooling] · P2 — `scripts/dev/coverage-delta-gate.sh:69` `tests/support/*.h` counts as a "test change"; gate is trivially dismissable
  Details: A PR can add an empty `tests/support/foo.h` to dismiss the gate. The intent was per-test-file coverage parity.
  Concrete next action: tighten to `tests/Source_Core/*.test.cpp|tests/Lua/*.test.cpp|tests/Plugins/**/*.test.cpp` only. Surfaced by retrospective code-review sweep on PR #148.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [tooling] · P2 — `scripts/dev/coverage.sh:35` `--threshold "${2:-0}"; shift 2` triggers `unbound variable` under `set -euo pipefail` when `$2` missing
  Details: `set -euo pipefail` is the file's default; the `shift 2` is unguarded.
  Concrete next action: `shift $(( $# < 2 ? $# : 2 ))` or guard with `[[ $# -ge 2 ]] && shift 2 || shift 1`. Surfaced by retrospective code-review sweep on PR #148.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [tooling] · P2 — `.github/workflows/coverage.yml:50` cache key hashes `CMakeLists.txt` but not `CMakePresets.json`
  Details: `actions/cache@v4` key hashes `CMakeLists.txt` + `**/CMakeLists.txt`. Coverage-flag changes (which live in `CMakePresets.json`) won't bust the FetchContent cache → stale gcov-instrumented `_deps`.
  Concrete next action: include `CMakePresets.json` in the cache key hash inputs. Surfaced by retrospective code-review sweep on PR #148.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-16 · build-doctor · [tooling] · P2 — Phase 9 coverage threshold (≥70%) advisory soak → blocking flip
  Details: Phase 9 (`test-phase-9-coverage-gates`) ships `scripts/dev/coverage.sh` + `.github/workflows/coverage.yml` running with `--threshold 0` and `continue-on-error: true` for the first two weeks. Parent plan's § End-state targets calls for ≥70% line coverage on `Source_Core/src/` (excluding ImGui / UI files) as a hard gate. Same advisory→blocking lifecycle as Phase 7's screenshot-diff.
  Concrete next action: after two consecutive green weeks of `coverage.yml` runs, flip (a) `coverage.yml` `continue-on-error: true` → `false`; (b) `coverage.sh` invocation from `--threshold 0` → `--threshold 70`; (c) consider adding `--threshold 90` carve-out for the high-risk units (IssueCreatePipeline, IssueDraft, TrackerFieldValueParser, CallstackParser, LocalCacheManager, TicketSyncService, ConfigManager migrations, MCP dispatch, Lua bindings) per parent plan. Estimated cost 30 min once the soak baseline is collected.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-16 · build-doctor · [tooling] · P2 — Headless CI runners need GL context for spawn-mode UI tests (bucket C + bucket E)
  Details: `.github/workflows/build-and-test.yml` runs `windows-2022` GitHub Actions runners. The runners advertise no display, and `glfwInit` + `glfwCreateWindow` against a hidden window returns a context whose `glReadPixels(GL_FRONT, ...)` reads an undefined / empty buffer (zero or driver-noise). Bucket-C screenshot diff therefore can't gate on cloud CI today — the Phase 7 advisory step is `continue-on-error: true`. Same blocker applies to the existing bucket-E `test-ui-views-columns-reorder.sh` (already excluded from the CI step).
  Concrete next action: wire mesa (`opengl32sw.dll` on Windows runners) OR a headless GL context via ANGLE-D3D11. Either lets Standalone + ImGui Test Engine + screenshot capture run in CI. Estimated cost ~3-5 h to install mesa on the runner image + verify a screenshot-diff round-trip; or ~1 day to switch to ANGLE-D3D11 if mesa proves too lossy for the L∞ ≤ 4 tolerance. Until then, bucket-C + bucket-E gates run on dev machines only.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-16 · test-author · [tooling] · P2 — Phase 7 pink-clear dock-gap scan (deferred from Phase 7 scenario set)
  Details: AGENTS.md § Debug techniques documents the magenta-clear trick (`glClearColor(1, 0, 1, 1)`) for detecting dock-gap leaks. The Phase 7 `DockGapSentinelScenario` originally planned to flip the clear color during its warm-up frames so any visible pink in the captured PPM = real dock gap. Implementation required a new `UiDrawSession::requestClearColor` flag + a `Target_Standalone/main.cpp` consumer — non-trivial surface for marginal coverage given the L∞ diff against a clean golden already catches dock-shift regressions. `smatchet::test::CountPixels(img, 255, 0, 255, tol)` shipped in `tests/support/GoldenImage.h` to enable the scan once the clear-color toggle lands.
  Concrete next action: add `requestClearColor{R,G,B,A}` fields to `UiDrawSession` + restore-on-clear-after-frame consumer in main.cpp; extend `DockGapSentinelScenario` to set pink-clear during warm-up + bash script to run `CountPixels(img, 255, 0, 255, 8) == 0` as a hard assertion. Estimated cost ~1.5 h.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-16 · test-author · [tooling] · P2 — lint-hook deferred-drain verification gaps (4 checks deferred)
  Details: `scripts/dev/test-lint-hook-split.sh` ships 14 assertions across 7 of 11 plan-spec checks. Four deferred for follow-up:
    - Test 4 — issue surfacing with a real cppcheck violation. Requires fault-injection into a real .cpp under `Source_Core/` (write a deliberate `if (x = 1)` and expect drain exit 2). Either author a `tests/fixtures/` first-party-path subtree + carve-out, or run in a real-source mutation harness.
    - Test 5 — chunked drain across > `SMATCHET_LINT_DRAIN_CHUNK` files. Synthesise 11+ distinct .cpp paths into the queue, run drain, assert remainder re-queued.
    - Test 6 — parallel-subagent per-PID isolation. Stage two `.lint-queue.<distinct-pids>` files with overlapping + disjoint paths, run drain, assert both consumed without data loss.
    - Test 10 — lockfile serialises concurrent drains. Spawn two `lint-cpp-drain.sh` invocations in parallel against a shared queue, assert exactly one processes + the other exits 0 without touching state.
  Concrete next action: 1 h for tests 5+6+10 (pure file synthesis); 2 h for test 4 (needs the production-source mutation discipline). Not blocking the deferred-drain ship — pipeline behaviour is exercised by the 7 covered checks plus the live Part 0 spike.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-15 · test-author · [tooling] · P2 — perf-measure scenario `blame_open_entry_tab` does not exist; Pillar 1 regression gate uncovered
  Details: Item #4 from `~/.claude/plans/make-the-any-presentation-serene-oasis.md` § Verification — "Pillar 1 gate — `perf-measure` on `blame_open_entry_tab` scenario before/after — mean frame ≤ 6.94 ms" — references a scenario name that is not registered with `ScenarioRunner` (only `priority-grid-scroll` + `lua-recorder-fuzz` + `ui-test` exist today; see `Source_Core/src/Commands/Scenarios/`).
  Concrete next action: author `blame-open-entry-tab`: (a) a fake-callstack injection API on `AppController` so the scenario can prime `BlameAnalysisUi::State().callstackBuf` without going through the live Jira fetch path, (b) a scenario class (~100 LoC modelled on `PriorityGridScrollScenario.cpp`) that opens Blame UI → runs `blame.process` → switches to Entry tab → ticks N frames so `UiPerfMonitor` accumulates `DrawColoredCppLine` samples, (c) `OnCancel` cleanup that unwinds the injection. Estimated cost 3 h (1 h injection API, 1.5 h scenario, 0.5 h doc + scripts/dev/test-blame-perf.sh runner). Until then, the tokenizer hot-path lacks a regression gate.
  Status: open
  Last-reviewed: 2026-05-18

## Parked

> P3 entries with no immediate owner. Reassess when adjacent feature lands or when a P2 promotion is justified.

<!-- Latest first within Parked. -->

- 2026-05-21 · architect · [tooling] · P3 — Architect-review checklist needs "name the chokepoint AppController shim, not the upstream caller" rule
  Details: Architect pre-code review of `docs/design/github-tracker-backend.md` found the plan's audit-call-site enumeration ("~30 sites across `*ReactController.cpp`, `JiraIssueMutation.cpp`, `PlaneIssueMutation.cpp`, `GitHubClient.cpp` write methods, `LuaConsole/`, `Mcp/`") was misleading. Lua and MCP have **zero direct** `BackendAuditTrail::Append*` / `UpdateField` / `AddIssueCommentPlain` calls — their writes route through `AppController` shim methods (the binding adapters in `AppController_LuaBindings.cpp` + MCP tool handlers). The actor pass-through is a 4-method change at the shim layer, not a 30-site sweep. A plan that names the upstream caller as the change-site sends the implementation agent to wrong files.
  Concrete next action: add a checklist item to `agents/architect.md` § Cross-cutting review template: "For any cross-cutting signature change (audit-trail, error-policy, retry-shape, locale-string, etc.), grep the upstream caller for direct calls to the changed surface; if there are zero or near-zero direct calls, the true change-site is the AppController shim (or other binding adapter). Name that shim explicitly in the file list." ~10 min agent-doc edit. Wins on every cross-cutting plan that touches a shared API used by Lua / MCP / UI.
  Status: parked
  Last-reviewed: 2026-05-21

- 2026-05-18 · orchestrator · [tooling] · P3 — `git worktree remove --force` leaves the directory on disk on Windows
  Details: End-of-session cleanup removed 35 agent worktrees via `git worktree unlock` + `git worktree remove --force` against each path. Git's metadata was correctly cleared (subsequent `git worktree list` showed only the main checkout), but every directory under `.claude/worktrees/agent-*` persisted on disk — 38 stale dirs requiring a manual `rm -rf agent-*` sweep. Likely cause: open file handles (lint hook caches, MSYS2 stat handles, antivirus scanning) prevent `RemoveDirectoryW` even with `--force`; git unregisters the worktree but logs no error when on-disk removal fails. Net effect: `worktree list` looks clean while disk usage remains, which is easy to miss for weeks.
  Concrete next action: wrap the `worktree remove` step in `agents/git-janitor.md`'s end-of-session checklist with a follow-up `for d in .claude/worktrees/agent-*; do [ -d "$d" ] && rm -rf "$d"; done` sweep — and emit a clear log line per directory deleted so the post-cleanup output reflects on-disk reality, not just git metadata. ~10 min doc + tiny script tweak.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-17 · orchestrator · [tooling] · P3 — Lazy-load AI clients to drop spawn-ready timeout (architectural follow-up)
  Details: The cheap fix from the original P1 entry shipped — `--spawn` ready-timeout bumped 15s→30s with `SMATCHET_SPAWN_READY_MS` env override (`Target_Standalone/CliCommandRunner.cpp:670`). Bucket-E gates unblock. Architectural follow-up remains: profile AI-client init paths (`OpenAiClient`, `AnthropicClient`, `OllamaClient`, `AiNdjsonParser`, Lua glue) and lazy-load so MCP server publishes ready in <15s again, then drop the bump.
  Concrete next action: instrument `AppController` ctor + `IAiClient` subclass init with `SMATCHET_UI_PERF_SCOPE` markers; identify which init paths can defer past MCP-ready; refactor. Once mean spawn-ready is <10s on dev machines, revert the timeout to 15s. ~3-4 h.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-17 · test-author · [tooling] · P3 — Preferences "Test connection" async button deferred from PR #174
  Details: PR #174 (`ai-debug-cli-and-prefs-validation`) planned a "Test connection" button in `SmatchetPreferencesUi.cpp` Assistant tab that would call `IAiClient::ProbeReachability` on a worker thread + post the result back via `MainThreadDispatcher`. Agent deferred at implementation time because the existing Preferences tab uses **per-field autosave** (no single Save button), so the async-result-display pattern would have fought the existing flow. Workaround for user: run `bash scripts/dev/manual-ai-anthropic-probe.sh` or `Smatchet.exe cmd ai.probe --provider anthropic` directly. Cost-to-add: ~30 min if folded into the broader Preferences UI refactor that gives the Assistant tab its own Save button (would also unblock other staged-validation UX). Independent worth alone: lower; CLI command + bash script already provide a clean equivalent.
  Concrete next action: either (a) add a self-contained Assistant-tab Save button + the async test button, or (b) leave the CLI path as the canonical reachability test and remove the button from any future plan docs. Decide at the next AI-feature-touching PR.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-17 · security-review · [tooling] · P3 — Install gitleaks + semgrep + flawfinder in MSYS2 dev image (security-review fallback is grep)
  Details: Current `security-review` agent attempts gitleaks / semgrep / flawfinder when present, falls back to grep heuristics + cppcheck security warnings otherwise. On the MSYS2 UCRT64 runner none of the three are installed, so cross-language secret scans + AST-aware vuln patterns silently degrade to text-search.
  Concrete next action: add a `scripts/dev/install-security-tools.sh` (mirror of `doctor.sh` shape) that pacman-installs `gitleaks` (or `go install` if not packaged), `pipx install semgrep`, `pacman -S mingw-w64-ucrt-x86_64-flawfinder`. Document in `docs/harness/SETUP.md`. ~1 h.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [tooling] · P3 — `MainThreadDispatcher::PostUiTask` sugar for typed worker→UI hand-off
  Details: `MainThreadDispatcher::PostToMainThread(Task)` takes `std::function<void()>` per `Source_Core/include/MainThreadDispatcher.h:33`. Phase B (PR #163) had to use the pattern "outer lambda captures AppController*, inner lambda references `g_ui` via TU-local `extern`" to reach UI state from a worker callback (`AiAssistantController.cpp` delta + error paths). The shape works but the discoverability is poor — Phase B agent's packet sketched the wrong signature (`function<void(AppController&)>`) on a guess. A typed sugar layer like `PostUiTask([](UiDrawSession& d){ ... })` (or two-arg `(AppController& app, UiDrawSession& d)`) would (a) make worker→UI hand-off self-documenting + (b) centralise the `g_ui` extern shim that AI/MCP/sync currently each replicate.
  Concrete next action: add `MainThreadDispatcher::PostUiTask(std::function<void(UiDrawSession&)>)` as a thin wrapper that resolves `g_ui` once at the dispatch boundary; deprecate raw `PostToMainThread` for worker callbacks. ~1 h including in-tree replacements of the 3 known worker→UI sites (sync, audit, AI).
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [tooling] · P3 — `Source_Core/include/IAiClient.h:14` `virtual ~IAiClient() {}` should be `= default`
  Details: Defaulted destructor preferred for trivial-destruct interfaces; rule-of-three compliance.
  Concrete next action: `virtual ~IAiClient() = default;` + add rule-of-three (copy/move ctor + assign defaults). Surfaced by retrospective code-review sweep on PR #140.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [tooling] · P3 — `Source_Core/src/AiClientFactory.cpp:34,47` fallthrough returns after switch without `default:` will warn `-Wswitch` if `AiProvider` enum grows
  Details: Future-proof against an enum extension going unhandled.
  Concrete next action: add `default:` arm returning a null-client or assertion. Surfaced by retrospective code-review sweep on PR #140.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [tooling] · P3 — `Source_Core/src/OpenAiClient.cpp:18-22` `JoinUrl` does not handle `base` ending `//` or non-leading-slash `path`
  Details: All call sites safe today; defensive note in case of future refactor.
  Concrete next action: comment or `CHECK` invariants at the function head. Surfaced by retrospective code-review sweep on PR #140.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [tooling] · P3 — `tests/support/ScreenshotDiffMain.cpp:32` three-positional CLI with no `--help`
  Details: Discovery friction for a one-off contributor; positional args undocumented.
  Concrete next action: add a `--help` arm printing the three positional names + an example invocation. Surfaced by retrospective code-review sweep on PR #146.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [tooling] · P3 — `CMakePresets.json:154` `RelWithDebInfo` + `--coverage` may strip `gcov` notes via `-fdata-sections`
  Details: Coverage instrumentation can interact with dead-section stripping.
  Concrete next action: verify `*.gcno` existence with an acceptance test in `scripts/dev/test-coverage-gcno.sh`. Surfaced by retrospective code-review sweep on PR #148.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-16 · test-author · [tooling] · P3 — Phase 7 mutation-sanity demo (bootstrap → mutate → revert)
  Details: Per AGENTS.md plan-revision contract + plan-locks packet, each high-risk verification should ship a mutation-sanity recipe: introduce a deliberate one-pixel offset in the production path, observe the gate fail, then revert before commit. Phase 7's two new scenarios + bash gate need the **first golden capture on the user's machine** before mutation-sanity can be meaningful — a freshly-bootstrapped golden is byte-equal to its own capture, so the mutation has to follow the bootstrap. `scripts/dev/test-screenshot-diff.sh` documents the recipe inline in its header.
  Concrete next action: dedicated demo session: (1) `bash scripts/dev/test-screenshot-diff.sh --bootstrap` to capture clean goldens, (2) nudge ImGui dock-spacing or palette padding by 1px in `SmatchetUI.cpp` / `SmatchetTheme.cpp`, (3) rerun the gate, observe diff helper reporting `L∞ > 4`, (4) revert before commit. Estimated cost ~20 min when adjacent to the next dev session that touches dock layout.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-16 · orchestrator · [tooling] · P3 — `gh pr merge --delete-branch` fails when local worktree owns the branch
  Details: After auto-merge of Wave A2 PRs, `gh pr merge 119 --squash --delete-branch` and siblings emitted `failed to delete local branch <branch>: failed to run git: error: cannot delete branch '<branch>' used by worktree at 'C:/Dev/Smatchet/.claude/worktrees/agent-<id>'`. The merge **does** succeed remotely; only the local-branch deletion silently fails. Subsequent `gh pr merge` calls on later PRs sometimes also fail because the local clone still thinks the branch is alive.
  Concrete next action: document the right order in AGENTS.md § Project rules (alongside the existing § Destructive git ops in shared worktrees sub-section) + `agents/git-janitor.md`: worktree-remove first, then merge, then branch-delete. Estimated cost 15 min doc edit.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-16 · orchestrator · [tooling] · P3 — `Source_Core/*.cpp` GLOB picks up new TUs for production targets — only test target needs explicit per-file entry
  Details: Wave A2 agents wrote new pure-helper TUs (`TrackerLabelsPure.cpp`, `TrackerDateTimePure.cpp`, `TrackerFieldPayloadPure.cpp`, `TrackerFieldCatalogPure.cpp`). Production builds (Standalone + DX12) picked them up automatically via the existing `Source_Core/src/*.cpp` GLOB in the root `CMakeLists.txt`. The test target (`tests/CMakeLists.txt`) is **explicit per-file** — needs a per-source `.cpp` entry **and** a per-test `.cpp` entry. Mental-model save: agents otherwise reflexively touch both files.
  Concrete next action: add a one-line note to `agents/test-rig.md` § Workflow: "Production targets auto-pick new `Source_Core/src/*.cpp` via GLOB — only `tests/CMakeLists.txt` needs explicit per-file source list updates."
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-15 · test-author · [tooling] · P3 — `MarkdownPreviewLangTag` covered (bucket A); rendered-output coverage deferred
  Details: Added `tests/Source_Core/MarkdownPreviewLangTag.test.cpp` (5 cases / 40 assertions) over the inlined `MarkdownPreviewRender::IsCppLikeLangTag` classifier — covers the C/C++ canonical spellings, case-insensitivity, non-cpp languages (python/js/rust/…), substring rejection (cppreference/ccache/cxxabi must not match), and whitespace-only / empty tags. This proves the decision predicate; what still needs automation is "given a markdown document containing a ` ```cpp ` fence, the leave-block handler actually iterates `codeBuffer` line-by-line through `DrawColoredCppLine`."
  Concrete next action: Bucket B — scenario `markdown-preview-fence-render` that builds a `MarkdownPreviewRender::Render(fixtureMd)` against a fixed input + screenshot-diff the rendered child region (gated on bucket-C harness). Bucket C — pixel-class count assertion against known keyword RGB. Bucket E — ImGui Test Engine fixture that opens the long-text editor modal with a fixed markdown source, asserts the colorized child renders ≥ N pixels of the active theme's keyword color. Estimated cost 30 min once bucket B/C harness from the parent theme entry lands.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-15 · test-author · [tooling] · P3 — Blame UI raw-callstack `showRaw=true` colored-display verification fully deferred
  Details: Item #3 from `~/.claude/plans/make-the-any-presentation-serene-oasis.md` § Verification — "raw callstack panel with `showRaw=true` is read-only colored; `showRaw=false` still editable" — has no testable seam at the pure-logic layer. The branch is a 2-line `if (State().showRaw)` at `Source_Core/src/BlameAnalysisUi_Window.cpp:316`; one arm calls `DrawColoredCppText(callstackBuf)` inside `BeginChild`, the other calls `InputTextMultiline(... 0)` for editable input. No algorithmic decision to unit-test. Bucket E (ImGui Test Engine) is the right home — open Blame UI with a pre-populated `callstackBuf`, drive the showRaw toggle, snapshot the panel, assert (a) `showRaw=true` panel has zero `InputText`-cursor item by walking the ImGui ID stack, (b) `showRaw=true` paints ≥ N keyword-color pixels in the panel rect, (c) `showRaw=false` panel has an `InputText` item with `ReadOnly=false`. Blocker today: no Blame UI fixture in `tests/ui/`.
  Concrete next action: 2 h (fixture + 3 cases). Filing so the gap accumulates evidence; bucket-A path proven by the parent theme test catches the underlying tokenizer + theme switch, so the residue is only the branch-routing layer.
  Status: parked
  Last-reviewed: 2026-05-18

- 2026-05-15 · test-author · [tooling] · P3 — bucket-B/C for theme syntax-highlight verification deferred — no `theme.*` CLI command + no golden-image screenshot diff
  Details: Manual step "cycle theme via Settings menu and eyeball that keyword/string/comment/number colors change in the Blame Entry tab" is partially covered by `tests/Source_Core/SmatchetThemeSyntaxColors.test.cpp` (bucket A — 7 cases / 28 assertions over the file-static round-trip per theme + pairwise cross-theme keyword inequality). Pixel-level "DrawColoredCppLine actually paints those colors on screen" is not yet automated.
  Concrete next action: Bucket B requires a `theme.apply <ThemeId>` command in `Source_Core/src/Commands/BuiltinCommands.cpp` (~30 LoC: enum-arg parser + dispatch to `SmatchetTheme::ApplyStyle`) plus a `theme-cycle-blame` scenario that runs `theme.apply` × 5 with `blame.open` + `debug.window.screenshot` in between; pass condition "scenario exits 0 across all 5 themes, no warnings". Bucket C extends B with a pixel-class count assertion ("≥ N pixels match the theme's known keyword RGB in the Blame Entry region"). Estimated cost 1 h for bucket B, +1 h for bucket C with golden PPMs.
  Status: parked
  Last-reviewed: 2026-05-18
