# Agent self-improvement — tooling

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-18 · orchestrator · [tooling] · P3 — Bucket-E coverage for Preferences > Agentic tab (T7 residue)
  Details: T7 (PR pending) ships the scheduled-poll worker + Preferences "Agentic" tab (`SmatchetPreferencesUi.cpp` master toggle / interval / source / query / GitHub PAT / Run-now button). The worker thread itself is unit-test-hostile (std::thread + condition variable + 60..3600 s sleeps); we lean on `scripts/dev/test-agentic-triage-cli.sh` for the synchronous triage path the worker calls. The Preferences UI variants (toggle flip → RestartAgenticPoll, Run-now → LaunchBackgroundTask, last-poll/next-poll readout updates) are not exercised by any bucket — manual click verification today.
  Concrete next action: add `tests/ui/agentic_prefs_tab.test.cpp` (bucket-E) parallel to `tests/ui/agent_proposals_panel.test.cpp` covering: toggle-on-without-PAT (no thread spawned), toggle-on-with-PAT (thread spawned + joined on Stop), Run-now button (dispatches a background task), last-poll readout transition from "never" → time-ago string. Runner: `scripts/dev/test-ui-agentic-prefs.sh`. ~2 h.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · orchestrator · [tooling] · P3 — Lazy-load AI clients to drop spawn-ready timeout (architectural follow-up)
  Details: The cheap fix from the original P1 entry shipped — `--spawn` ready-timeout bumped 15s→30s with `SMATCHET_SPAWN_READY_MS` env override (`Target_Standalone/CliCommandRunner.cpp:670`). Bucket-E gates unblock. Architectural follow-up remains: profile AI-client init paths (`OpenAiClient`, `AnthropicClient`, `OllamaClient`, `AiNdjsonParser`, Lua glue) and lazy-load so MCP server publishes ready in <15s again, then drop the bump.
  Concrete next action: instrument `AppController` ctor + `IAiClient` subclass init with `SMATCHET_UI_PERF_SCOPE` markers; identify which init paths can defer past MCP-ready; refactor. Once mean spawn-ready is <10s on dev machines, revert the timeout to 15s. ~3-4 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · test-author · [tooling] · P3 — Preferences "Test connection" async button deferred from PR #174
  Details: PR #174 (`ai-debug-cli-and-prefs-validation`) planned a "Test connection" button in `SmatchetPreferencesUi.cpp` Assistant tab that would call `IAiClient::ProbeReachability` on a worker thread + post the result back via `MainThreadDispatcher`. Agent deferred at implementation time because the existing Preferences tab uses **per-field autosave** (no single Save button), so the async-result-display pattern would have fought the existing flow. Workaround for user: run `bash scripts/dev/manual-ai-anthropic-probe.sh` or `Smatchet.exe cmd ai.probe --provider anthropic` directly. Cost-to-add: ~30 min if folded into the broader Preferences UI refactor that gives the Assistant tab its own Save button (would also unblock other staged-validation UX). Independent worth alone: lower; CLI command + bash script already provide a clean equivalent.
  Concrete next action: either (a) add a self-contained Assistant-tab Save button + the async test button, or (b) leave the CLI path as the canonical reachability test and remove the button from any future plan docs. Decide at the next AI-feature-touching PR.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [tooling] · P3 — Install gitleaks + semgrep + flawfinder in MSYS2 dev image (security-review fallback is grep)
  Details: Current `security-review` agent attempts gitleaks / semgrep / flawfinder when present, falls back to grep heuristics + cppcheck security warnings otherwise. On the MSYS2 UCRT64 runner none of the three are installed, so cross-language secret scans + AST-aware vuln patterns silently degrade to text-search.
  Concrete next action: add a `scripts/dev/install-security-tools.sh` (mirror of `doctor.sh` shape) that pacman-installs `gitleaks` (or `go install` if not packaged), `pipx install semgrep`, `pacman -S mingw-w64-ucrt-x86_64-flawfinder`. Document in `docs/harness/SETUP.md`. ~1 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [tooling] · P3 — `MainThreadDispatcher::PostUiTask` sugar for typed worker→UI hand-off
  Details: `MainThreadDispatcher::PostToMainThread(Task)` takes `std::function<void()>` per `Source_Core/include/MainThreadDispatcher.h:33`. Phase B (PR #163) had to use the pattern "outer lambda captures AppController*, inner lambda references `g_ui` via TU-local `extern`" to reach UI state from a worker callback (`AiAssistantController.cpp` delta + error paths). The shape works but the discoverability is poor — Phase B agent's packet sketched the wrong signature (`function<void(AppController&)>`) on a guess. A typed sugar layer like `PostUiTask([](UiDrawSession& d){ ... })` (or two-arg `(AppController& app, UiDrawSession& d)`) would (a) make worker→UI hand-off self-documenting + (b) centralise the `g_ui` extern shim that AI/MCP/sync currently each replicate.
  Concrete next action: add `MainThreadDispatcher::PostUiTask(std::function<void(UiDrawSession&)>)` as a thin wrapper that resolves `g_ui` once at the dispatch boundary; deprecate raw `PostToMainThread` for worker callbacks. ~1 h including in-tree replacements of the 3 known worker→UI sites (sync, audit, AI).
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · test-author · [tooling] · P2 — `test-all.sh` baseline drift across worktrees: 8 fails on agent worktree vs 0 fails on main repo
  Details: Phase B (PR #163) agent reported 168 pass / 8 fail on isolated-worktree dispatch. Same `bash scripts/dev/test-all.sh` on main develop reports 173/0. The 8 worktree-only failures are pre-existing infra (lint-hook-split needs `.claude/hooks/` symlinked into the worktree root; 4 ui-test scripts have batched-PATH issues that pass when run individually). Causes false "regression" alarms in every PR that ships from a worktree, eroding signal.
  Concrete next action: either (a) `test-all.sh` auto-detects worktree context via `git rev-parse --git-common-dir` vs `git rev-parse --git-dir` and skips worktree-incompatible scripts with a CLEAR `SKIPPED (worktree)` line, or (b) `bash scripts/setup-harness.sh claude-code` extends to seed `.claude/hooks/` symlinks into newly-cut worktrees. Option (a) is simpler; option (b) is more thorough. ~2 h for either.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · test-author · [tooling] · P2 — Bucket-E tooltip-content-identity helper for production-driven hover tests
  Details: While writing `tests/ui/callstack_tooltip_hover.test.cpp` (PR #156 — regression gate for #147) a production-driven variant 4 had to be dropped. A generic `##Tooltip_NN` window probe cannot distinguish "my cell's tooltip" from concurrent host-process tooltips. Even with `WindowFocus` + `NoDocking` + `ImGuiCond_Always` position pinning, production's `IsItemHovered()` against the cell rect returned false in the spawned-child host because something else in the shared `ImGuiContext` claimed `g.HoveredWindow`. Workaround taken: faithful replica of the production callstack path with a TU-local `tooltipFiredThisFrame` flag (same idiom as `views_columns_reorder.test.cpp`), plus a `NoGroupWrap` regression-shape variant that proves the methodology is sensitive to the wrap's presence. Sanity-checked end-to-end: removing the wrap from the replica fails variants 1+2 deterministically.
  Concrete next action: add a `BucketE::TooltipContentMatches(ctx, sentinel)` helper to `tests/ui/` (or shared `tests/ui/_helpers/`) that walks a tooltip window's `DrawList`'s `CmdBuffer` for a text command containing `sentinel`. Use it to distinguish "my cell's tooltip" from concurrent tooltips by feeding a unique marker through `rawForTooltip`. Once available, retrofit `callstack_tooltip_hover.test.cpp` with a variant 4 that drives real `RenderClippedFieldText` and asserts content match — closing the production-drift gap the replica can't cover.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [tooling] · P2 — `scripts/dev/coverage-delta-gate.sh:69` `tests/support/*.h` counts as a "test change"; gate is trivially dismissable
  Details: A PR can add an empty `tests/support/foo.h` to dismiss the gate. The intent was per-test-file coverage parity.
  Concrete next action: tighten to `tests/Source_Core/*.test.cpp|tests/Lua/*.test.cpp|tests/Plugins/**/*.test.cpp` only. Surfaced by retrospective code-review sweep on PR #148.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [tooling] · P2 — `scripts/dev/coverage.sh:35` `--threshold "${2:-0}"; shift 2` triggers `unbound variable` under `set -euo pipefail` when `$2` missing
  Details: `set -euo pipefail` is the file's default; the `shift 2` is unguarded.
  Concrete next action: `shift $(( $# < 2 ? $# : 2 ))` or guard with `[[ $# -ge 2 ]] && shift 2 || shift 1`. Surfaced by retrospective code-review sweep on PR #148.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [tooling] · P2 — `.github/workflows/coverage.yml:50` cache key hashes `CMakeLists.txt` but not `CMakePresets.json`
  Details: `actions/cache@v4` key hashes `CMakeLists.txt` + `**/CMakeLists.txt`. Coverage-flag changes (which live in `CMakePresets.json`) won't bust the FetchContent cache → stale gcov-instrumented `_deps`.
  Concrete next action: include `CMakePresets.json` in the cache key hash inputs. Surfaced by retrospective code-review sweep on PR #148.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [tooling] · P3 — `Source_Core/include/IAiClient.h:14` `virtual ~IAiClient() {}` should be `= default`
  Details: Defaulted destructor preferred for trivial-destruct interfaces; rule-of-three compliance.
  Concrete next action: `virtual ~IAiClient() = default;` + add rule-of-three (copy/move ctor + assign defaults). Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [tooling] · P3 — `Source_Core/src/AiClientFactory.cpp:34,47` fallthrough returns after switch without `default:` will warn `-Wswitch` if `AiProvider` enum grows
  Details: Future-proof against an enum extension going unhandled.
  Concrete next action: add `default:` arm returning a null-client or assertion. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [tooling] · P3 — `Source_Core/src/OpenAiClient.cpp:18-22` `JoinUrl` does not handle `base` ending `//` or non-leading-slash `path`
  Details: All call sites safe today; defensive note in case of future refactor.
  Concrete next action: comment or `CHECK` invariants at the function head. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [tooling] · P3 — `tests/support/ScreenshotDiffMain.cpp:32` three-positional CLI with no `--help`
  Details: Discovery friction for a one-off contributor; positional args undocumented.
  Concrete next action: add a `--help` arm printing the three positional names + an example invocation. Surfaced by retrospective code-review sweep on PR #146.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [tooling] · P3 — `CMakePresets.json:154` `RelWithDebInfo` + `--coverage` may strip `gcov` notes via `-fdata-sections`
  Details: Coverage instrumentation can interact with dead-section stripping.
  Concrete next action: verify `*.gcno` existence with an acceptance test in `scripts/dev/test-coverage-gcno.sh`. Surfaced by retrospective code-review sweep on PR #148.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-author · [tooling] · P2 — Bucket-E tooltip-content-identity helper for production-driven hover tests
  Details: While writing `tests/ui/callstack_tooltip_hover.test.cpp` (PR #154 — regression gate for #147) a production-driven variant 4 had to be dropped. A generic `##Tooltip_NN` window probe cannot distinguish "my cell's tooltip" from concurrent host-process tooltips. Even with `WindowFocus` + `NoDocking` + `ImGuiCond_Always` position pinning, production's `IsItemHovered()` against the cell rect returned false in the spawned-child host because something else in the shared `ImGuiContext` claimed `g.HoveredWindow`. Workaround taken: faithful replica of the production callstack path with a TU-local `tooltipFiredThisFrame` flag (same idiom as `views_columns_reorder.test.cpp`), plus a `NoGroupWrap` regression-shape variant that proves the methodology is sensitive to the wrap's presence.
  Concrete next action: add a `BucketE::TooltipContentMatches(ctx, sentinel)` helper to `tests/ui/` (or shared `tests/ui/_helpers/`) that walks a tooltip window's `DrawList`'s `CmdBuffer` for a text command containing `sentinel`. Use it to distinguish "my cell's tooltip" from concurrent tooltips by feeding a unique marker through `rawForTooltip`. Once available, retrofit `callstack_tooltip_hover.test.cpp` with a variant 4 that drives real `RenderClippedFieldText` and asserts content match — closing the production-drift gap the replica can't cover.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · build-doctor · [tooling] · P2 — Phase 9 coverage threshold (≥70%) advisory soak → blocking flip
  Details: Phase 9 (`test-phase-9-coverage-gates`) ships `scripts/dev/coverage.sh` + `.github/workflows/coverage.yml` running with `--threshold 0` and `continue-on-error: true` for the first two weeks. Parent plan's § End-state targets calls for ≥70% line coverage on `Source_Core/src/` (excluding ImGui / UI files) as a hard gate. Same advisory→blocking lifecycle as Phase 7's screenshot-diff.
  Concrete next action: after two consecutive green weeks of `coverage.yml` runs, flip (a) `coverage.yml` `continue-on-error: true` → `false`; (b) `coverage.sh` invocation from `--threshold 0` → `--threshold 70`; (c) consider adding `--threshold 90` carve-out for the high-risk units (IssueCreatePipeline, IssueDraft, TrackerFieldValueParser, CallstackParser, LocalCacheManager, TicketSyncService, ConfigManager migrations, MCP dispatch, Lua bindings) per parent plan. Estimated cost 30 min once the soak baseline is collected.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · build-doctor · [tooling] · P2 — Headless CI runners need GL context for spawn-mode UI tests (bucket C + bucket E)
  Details: `.github/workflows/build-and-test.yml` runs `windows-2022` GitHub Actions runners. The runners advertise no display, and `glfwInit` + `glfwCreateWindow` against a hidden window returns a context whose `glReadPixels(GL_FRONT, ...)` reads an undefined / empty buffer (zero or driver-noise). Bucket-C screenshot diff therefore can't gate on cloud CI today — the Phase 7 advisory step is `continue-on-error: true`. Same blocker applies to the existing bucket-E `test-ui-views-columns-reorder.sh` (already excluded from the CI step).
  Concrete next action: wire mesa (`opengl32sw.dll` on Windows runners) OR a headless GL context via ANGLE-D3D11. Either lets Standalone + ImGui Test Engine + screenshot capture run in CI. Estimated cost ~3-5 h to install mesa on the runner image + verify a screenshot-diff round-trip; or ~1 day to switch to ANGLE-D3D11 if mesa proves too lossy for the L∞ ≤ 4 tolerance. Until then, bucket-C + bucket-E gates run on dev machines only.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-author · [tooling] · P2 — Phase 7 pink-clear dock-gap scan (deferred from Phase 7 scenario set)
  Details: AGENTS.md § Debug techniques documents the magenta-clear trick (`glClearColor(1, 0, 1, 1)`) for detecting dock-gap leaks. The Phase 7 `DockGapSentinelScenario` originally planned to flip the clear color during its warm-up frames so any visible pink in the captured PPM = real dock gap. Implementation required a new `UiDrawSession::requestClearColor` flag + a `Target_Standalone/main.cpp` consumer — non-trivial surface for marginal coverage given the L∞ diff against a clean golden already catches dock-shift regressions. `smatchet::test::CountPixels(img, 255, 0, 255, tol)` shipped in `tests/support/GoldenImage.h` to enable the scan once the clear-color toggle lands.
  Concrete next action: add `requestClearColor{R,G,B,A}` fields to `UiDrawSession` + restore-on-clear-after-frame consumer in main.cpp; extend `DockGapSentinelScenario` to set pink-clear during warm-up + bash script to run `CountPixels(img, 255, 0, 255, 8) == 0` as a hard assertion. Estimated cost ~1.5 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-author · [tooling] · P3 — Phase 7 mutation-sanity demo (bootstrap → mutate → revert)
  Details: Per AGENTS.md plan-revision contract + plan-locks packet, each high-risk verification should ship a mutation-sanity recipe: introduce a deliberate one-pixel offset in the production path, observe the gate fail, then revert before commit. Phase 7's two new scenarios + bash gate need the **first golden capture on the user's machine** before mutation-sanity can be meaningful — a freshly-bootstrapped golden is byte-equal to its own capture, so the mutation has to follow the bootstrap. `scripts/dev/test-screenshot-diff.sh` documents the recipe inline in its header.
  Concrete next action: dedicated demo session: (1) `bash scripts/dev/test-screenshot-diff.sh --bootstrap` to capture clean goldens, (2) nudge ImGui dock-spacing or palette padding by 1px in `SmatchetUI.cpp` / `SmatchetTheme.cpp`, (3) rerun the gate, observe diff helper reporting `L∞ > 4`, (4) revert before commit. Estimated cost ~20 min when adjacent to the next dev session that touches dock layout.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-author · [tooling] · P2 — lint-hook deferred-drain verification gaps (4 checks deferred)
  Details: `scripts/dev/test-lint-hook-split.sh` ships 14 assertions across 7 of 11 plan-spec checks. Four deferred for follow-up:
    - Test 4 — issue surfacing with a real cppcheck violation. Requires fault-injection into a real .cpp under `Source_Core/` (write a deliberate `if (x = 1)` and expect drain exit 2). Either author a `tests/fixtures/` first-party-path subtree + carve-out, or run in a real-source mutation harness.
    - Test 5 — chunked drain across > `SMATCHET_LINT_DRAIN_CHUNK` files. Synthesise 11+ distinct .cpp paths into the queue, run drain, assert remainder re-queued.
    - Test 6 — parallel-subagent per-PID isolation. Stage two `.lint-queue.<distinct-pids>` files with overlapping + disjoint paths, run drain, assert both consumed without data loss.
    - Test 10 — lockfile serialises concurrent drains. Spawn two `lint-cpp-drain.sh` invocations in parallel against a shared queue, assert exactly one processes + the other exits 0 without touching state.
  Concrete next action: 1 h for tests 5+6+10 (pure file synthesis); 2 h for test 4 (needs the production-source mutation discipline). Not blocking the deferred-drain ship — pipeline behaviour is exercised by the 7 covered checks plus the live Part 0 spike.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · orchestrator · [tooling] · P3 — `gh pr merge --delete-branch` fails when local worktree owns the branch
  Details: After auto-merge of Wave A2 PRs, `gh pr merge 119 --squash --delete-branch` and siblings emitted `failed to delete local branch <branch>: failed to run git: error: cannot delete branch '<branch>' used by worktree at 'C:/Dev/Smatchet/.claude/worktrees/agent-<id>'`. The merge **does** succeed remotely; only the local-branch deletion silently fails. Subsequent `gh pr merge` calls on later PRs sometimes also fail because the local clone still thinks the branch is alive.
  Concrete next action: document the right order in AGENTS.md § Git workflow + `agents/git-janitor.md`: worktree-remove first, then merge, then branch-delete. Estimated cost 15 min doc edit.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · orchestrator · [tooling] · P3 — `Source_Core/*.cpp` GLOB picks up new TUs for production targets — only test target needs explicit per-file entry
  Details: Wave A2 agents wrote new pure-helper TUs (`TrackerLabelsPure.cpp`, `TrackerDateTimePure.cpp`, `TrackerFieldPayloadPure.cpp`, `TrackerFieldCatalogPure.cpp`). Production builds (Standalone + DX12) picked them up automatically via the existing `Source_Core/src/*.cpp` GLOB in the root `CMakeLists.txt`. The test target (`tests/CMakeLists.txt`) is **explicit per-file** — needs a per-source `.cpp` entry **and** a per-test `.cpp` entry. Mental-model save: agents otherwise reflexively touch both files.
  Concrete next action: add a one-line note to `agents/test-rig.md` § Workflow: "Production targets auto-pick new `Source_Core/src/*.cpp` via GLOB — only `tests/CMakeLists.txt` needs explicit per-file source list updates."
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-15 · test-author · [tooling] · P3 — `MarkdownPreviewLangTag` covered (bucket A); rendered-output coverage deferred
  Details: Added `tests/Source_Core/MarkdownPreviewLangTag.test.cpp` (5 cases / 40 assertions) over the inlined `MarkdownPreviewRender::IsCppLikeLangTag` classifier — covers the C/C++ canonical spellings, case-insensitivity, non-cpp languages (python/js/rust/…), substring rejection (cppreference/ccache/cxxabi must not match), and whitespace-only / empty tags. This proves the decision predicate; what still needs automation is "given a markdown document containing a ` ```cpp ` fence, the leave-block handler actually iterates `codeBuffer` line-by-line through `DrawColoredCppLine`."
  Concrete next action: Bucket B — scenario `markdown-preview-fence-render` that builds a `MarkdownPreviewRender::Render(fixtureMd)` against a fixed input + screenshot-diff the rendered child region (gated on bucket-C harness). Bucket C — pixel-class count assertion against known keyword RGB. Bucket E — ImGui Test Engine fixture that opens the long-text editor modal with a fixed markdown source, asserts the colorized child renders ≥ N pixels of the active theme's keyword color. Estimated cost 30 min once bucket B/C harness from the parent theme entry lands.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-15 · test-author · [tooling] · P3 — Blame UI raw-callstack `showRaw=true` colored-display verification fully deferred
  Details: Item #3 from `~/.claude/plans/make-the-any-presentation-serene-oasis.md` § Verification — "raw callstack panel with `showRaw=true` is read-only colored; `showRaw=false` still editable" — has no testable seam at the pure-logic layer. The branch is a 2-line `if (State().showRaw)` at `Source_Core/src/BlameAnalysisUi_Window.cpp:316`; one arm calls `DrawColoredCppText(callstackBuf)` inside `BeginChild`, the other calls `InputTextMultiline(... 0)` for editable input. No algorithmic decision to unit-test. Bucket E (ImGui Test Engine) is the right home — open Blame UI with a pre-populated `callstackBuf`, drive the showRaw toggle, snapshot the panel, assert (a) `showRaw=true` panel has zero `InputText`-cursor item by walking the ImGui ID stack, (b) `showRaw=true` paints ≥ N keyword-color pixels in the panel rect, (c) `showRaw=false` panel has an `InputText` item with `ReadOnly=false`. Blocker today: no Blame UI fixture in `tests/ui/`.
  Concrete next action: 2 h (fixture + 3 cases). Filing so the gap accumulates evidence; bucket-A path proven by the parent theme test catches the underlying tokenizer + theme switch, so the residue is only the branch-routing layer.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-15 · test-author · [tooling] · P2 — perf-measure scenario `blame_open_entry_tab` does not exist; Pillar 1 regression gate uncovered
  Details: Item #4 from `~/.claude/plans/make-the-any-presentation-serene-oasis.md` § Verification — "Pillar 1 gate — `perf-measure` on `blame_open_entry_tab` scenario before/after — mean frame ≤ 6.94 ms" — references a scenario name that is not registered with `ScenarioRunner` (only `priority-grid-scroll` + `lua-recorder-fuzz` + `ui-test` exist today; see `Source_Core/src/Commands/Scenarios/`).
  Concrete next action: author `blame-open-entry-tab`: (a) a fake-callstack injection API on `AppController` so the scenario can prime `BlameAnalysisUi::State().callstackBuf` without going through the live Jira fetch path, (b) a scenario class (~100 LoC modelled on `PriorityGridScrollScenario.cpp`) that opens Blame UI → runs `blame.process` → switches to Entry tab → ticks N frames so `UiPerfMonitor` accumulates `DrawColoredCppLine` samples, (c) `OnCancel` cleanup that unwinds the injection. Estimated cost 3 h (1 h injection API, 1.5 h scenario, 0.5 h doc + scripts/dev/test-blame-perf.sh runner). Until then, the tokenizer hot-path lacks a regression gate.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-15 · test-author · [tooling] · P3 — bucket-B/C for theme syntax-highlight verification deferred — no `theme.*` CLI command + no golden-image screenshot diff
  Details: Manual step "cycle theme via Settings menu and eyeball that keyword/string/comment/number colors change in the Blame Entry tab" is partially covered by `tests/Source_Core/SmatchetThemeSyntaxColors.test.cpp` (bucket A — 7 cases / 28 assertions over the file-static round-trip per theme + pairwise cross-theme keyword inequality). Pixel-level "DrawColoredCppLine actually paints those colors on screen" is not yet automated.
  Concrete next action: Bucket B requires a `theme.apply <ThemeId>` command in `Source_Core/src/Commands/BuiltinCommands.cpp` (~30 LoC: enum-arg parser + dispatch to `SmatchetTheme::ApplyStyle`) plus a `theme-cycle-blame` scenario that runs `theme.apply` × 5 with `blame.open` + `debug.window.screenshot` in between; pass condition "scenario exits 0 across all 5 themes, no warnings". Bucket C extends B with a pixel-class count assertion ("≥ N pixels match the theme's known keyword RGB in the Blame Entry region"). Estimated cost 1 h for bucket B, +1 h for bucket C with golden PPMs.
  Status: open
  Last-reviewed: 2026-05-17
