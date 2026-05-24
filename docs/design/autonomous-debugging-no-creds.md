# Plan — Fully autonomous debugging without credentials or user input

> **Slug**: `autonomous-debugging-no-creds`
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Grill-with-docs pass**: completed informally against the testing-infrastructure survey + backlog scan that motivated this plan (`docs/backlog/agent-self-improvement/test.md`, `process.md`, `tooling.md`, `bug.md`, `external-blockers.md`, `applied.md`). Each slice has a backlog entry it closes (or names "new" with rationale). No ADR needed: the plan is test-only and follows existing seams (`ITrackerBackendFactory::SetBackendFactory`, `AiClientFactory::SetTestOverride`, `ScenarioRunner` registration, ImGui Test Engine). No production architecture decision.

## Context

Smatchet's debug loop today requires three categories of human input that block end-to-end autonomy:

1. **Credentials** — every tracker backend (Jira / GitHub / Plane / p4) calls live HTTP/RPC; reproducing a backend-touching bug needs real keys.
2. **Interactive verification** — bucket-C screenshot diff + bucket-E ImGui Test Engine are wired but not gated in CI (Windows headless-GL gap). UI bugs require "user opens window and observes."
3. **Reproducer hand-off** — `agents/debug-detective.md` accepts a CLI / scenario / Lua snippet as the *preferred* input but falls back to "user repro steps." Falls back too often.

The deterministic-jira-test-backend plan (just landed on develop) names the shape for closing axis (1) for Jira: extract pure JSON mappers into header-only helpers; reuse `tests/support/FakeTrackerClient.h` as the scripted `ITrackerClient`; add a fixture loader + a `SMATCHET_TEST_JIRA_BACKEND_FIXTURE=<path>` env hook into `AppController::SetBackendFactory`. This plan extends that shape to GitHub + Plane + p4, then closes axes (2) and (3) so the whole loop becomes deterministic.

**End-state**: a bug surfaces in CI (failing test / sanitizer / perf regression / golden-image diff). The merge-watcher auto-act path spawns `debug-detective`. The agent reads the failure metadata, picks the deterministic scenario that exercises the breaking path, runs it via `Smatchet.exe scenario.run --name=<scen> --fixture=<path>` against the in-process fake backend, instruments via `SMATCHET_UI_PERF_SCOPE` / `[temp-debug]` markers, reads `tests/_debug/SmatchetAgentDebug.h` NDJSON output, diagnoses, and hands the fix to the matching subsystem specialist. **Zero user input. Zero live credentials.**

## Approach

Reuse every existing seam — do not invent new architecture. The plan threads through five layers:

1. **Backend fakes** (slices 1-4) — extend the `FakeTrackerClient` + fixture-loader pattern from deterministic-jira-test-backend.md to every backend that today requires credentials: GitHub, Plane, p4, AI HTTP. Each follows the same recipe — extract pure JSON mapper, add `tests/support/Fake<Backend>Driver.h` thin wrapper, add `SMATCHET_TEST_<BACKEND>_FIXTURE=<path>` env hook, add bucket-A doctests covering fixture parsing.
2. **Scenario coverage densification** (slice 5) — every known-but-uncovered bug class gets a CLI scenario that reproduces it deterministically. The backlog already names the gaps (`blame_open_entry_tab`, AI assistant streaming S2/S4/S5, description-grid-cell tooltip render); we close them as one slice with the same `ScenarioRunner` registration shape.
3. **Headless GL on CI + mouse emulation** (slices 6-7) — Mesa-on-CI (or ANGLE-D3D11) unblocks bucket-C/E in GitHub Actions; bucket-E's ImGui Test Engine already emits mouse / keyboard events via its `ImGuiTestContext::ItemClick` / `ItemInput` API, so the "mouse emulation" axis is mostly wiring + scenario density on top of headless-GL.
4. **Sanitizer CI auto-act** (slice 8) — ASAN+UBSAN preset exists in CMakePresets.json but isn't run in CI. Add a CI job; wire failures into `merge-watcher.py:maybe_auto_act` so debug-detective auto-spawns on sanitizer failure with the failing-test name + log path as the reproducer.
5. **Debug-detective contract + log helper** (slices 9-10) — agent prompt reshape: require a deterministic reproducer as input; on missing reproducer, the agent's first task is to add a scenario (slice 5 pattern). Plus `tests/_debug/SmatchetAgentDebug.h` for boundary-crossing NDJSON logs the agent can read without asking the user for paths.

## Detailed implementation plan

### Slice 1 — Pure GitHub issue mapping + deterministic backend (mirrors deterministic-jira-test-backend slice 1-3)

Per `docs/design/deterministic-jira-test-backend.md`, repeat for GitHub:

- Extract pure helpers from `Source_Core/src/GitHubIssueSearch.cpp` (~22 KB, currently named anonymous-namespace functions) into `Source_Core/{include,src}/GitHubIssueMappingPure.{h,cpp}`. Targets — JSON-to-`CachedTicket` mapper, `BuildGitHubHeaders`, the query builder side that doesn't touch HTTP.
- Add `tests/support/FakeGitHubFixture.h` — header-only, loads `tests/fixtures/github/<scenario>.json` (issue search response shape) and scripts `FakeTrackerClient("GitHub")` instances with the deserialised tickets.
- Wire `SMATCHET_TEST_GITHUB_BACKEND_FIXTURE=<path>` env hook into `AppController::Initialize` (next to the planned Jira hook), routed via `SetBackendFactory`.
- New `tests/Source_Core/GitHubIssueMappingPure.test.cpp` — basic field mapping; PR-vs-issue filter; assignee + state + labels coverage; pagination boundary.

Files: `Source_Core/include/GitHubIssueMappingPure.h` (new), `Source_Core/src/GitHubIssueMappingPure.cpp` (new), `Source_Core/src/GitHubIssueSearch.cpp` (delegate), `tests/support/FakeGitHubFixture.h` (new), `tests/Source_Core/GitHubIssueMappingPure.test.cpp` (new), `tests/CMakeLists.txt` (`SMATCHET_TESTS_SOURCES` append).

Backlog closure: implicit — the deterministic-jira plan's pattern, repeated. No standing backlog entry today (GitHub was a newer backend); this slice prevents the entry from being filed.

### Slice 2 — Pure Plane issue mapping + deterministic backend

Same shape as slice 1 against `Source_Core/src/Plane*` files (the Plane backend lives in `PlaneClient.cpp` per existing factory registration). New files: `PlaneIssueMappingPure.{h,cpp}`, `tests/support/FakePlaneFixture.h`, `tests/Source_Core/PlaneIssueMappingPure.test.cpp`. Env hook: `SMATCHET_TEST_PLANE_BACKEND_FIXTURE=<path>`. Doctest cases mirror slice 1.

### Slice 3 — Deterministic p4 backend layer

p4 is special — it's an opt-in local-VCS layer (`SMATCHET_AGENT_VCS=p4` per `docs/perforce/AGENT_FLOWS.md`), not a tracker. The fake-backend pattern still applies but the seam is different.

- New `tests/support/FakeP4Driver.h` — header-only, intercepts the `p4` CLI calls that `scripts/dev/p4-*.sh` shells out to. Implementation: a stub `p4` binary on `PATH` (mktemp dir prepended) that reads fixture JSON for `streams` / `change -o` / `shelve -d` / `submit` / `verify` / `counter` commands and returns canned responses.
- Re-uses the existing `tests/bats/lock_claim.bats` sandbox-bare-repo + stub-bin pattern (per `tests/bats/lock_claim.bats:25-38`).
- New `tests/bats/p4_dual_vcs_deterministic.bats` — bucket-A bats coverage for the p4 dual-VCS scripts with stub p4. Replaces the existing `scripts/dev/test-p4-dual-vcs.sh` integration test's reliance on a real local p4d.
- Env hook: `SMATCHET_TEST_P4_FIXTURE_DIR=<dir>` instead of a single fixture file (p4 has many command shapes).
- Defers the real-p4d integration test to a single bucket-A test under an explicit `SMATCHET_TEST_REAL_P4D=1` gate that's CI-skipped — same shape as `SMATCHET_WHISPER_LOCAL_BACKEND`.

Backlog closure: `docs/backlog/agent-self-improvement/infra.md` § Whisper Phase H residue documents the same pattern (gate the heavy integration test behind an env, default off).

### Slice 4 — AiHttpFixture + per-AI-client tests

Per `docs/backlog/agent-self-improvement/test.md` P2 (open).

- New `tests/support/AiHttpFixture.h` — in-process `httplib::Server` (cpp-httplib is already in the FetchContent set per AGENTS.md § Available libs) that serves canned SSE streams, 401 unauthorised, transient disconnects mid-frame, and `[DONE]` termination shapes per OpenAI's wire format.
- Bind to `127.0.0.1:<dynamic-port>`; export `OPENAI_API_BASE=http://127.0.0.1:<port>` (and equivalent for Anthropic + Ollama) so `OpenAiClient` / `AnthropicClient` / `OllamaClient` hit the fixture without ever crossing the network.
- New `tests/Source_Core/AiClientCancel.test.cpp` — per-client cancel + error-redaction regression tests (`OpenAiClient`, `AnthropicClient`, `OllamaClient`). Verifies (a) cancel-mid-stream stops `OnDelta` callbacks within 100 ms, (b) error bodies don't contain the API key.
- Extend `Source_Core/include/AiSseParser.h`'s pure-logic test coverage in `tests/Source_Core/AiSseParser.test.cpp` to fully cover the state machine: many-small-Feeds, partial-frame-on-Flush, malformed JSON branches, `\r\n\r\n` boundaries, mid-frame cancel (per backlog `bug.md` P2).

Backlog closure: `test.md` P2 entries for AiHttpFixture + per-client cancel + AiSseParser coverage; `bug.md` P2 AiSseParser missing coverage entry.

### Slice 5 — Missing scenarios for known-bug paths

Closes 3 backlog gaps that each name a missing `scenario.run` reproducer:

- `blame_open_entry_tab` — registered in the scenario list (per `test.md` P2). New file: `Source_Core/src/Scenarios/BlameOpenEntryTabScenario.cpp` + registration in `Source_Core/src/AppController.cpp`. Drives the blame tokenizer hot-path Pillar-1 perf regression gate already names.
- `ai_assistant_streaming_happy_path` / `ai_assistant_streaming_401` / `ai_assistant_streaming_transport_down_within_5s` — three scenarios consuming `AiHttpFixture` (slice 4). Drives the AI panel through real SSE flows + asserts the UI state transitions.
- `description_tooltip_markdown_render` — scenario that opens a grid row whose description contains markdown, hovers the cell, asserts the tooltip's `wrapWidth` is honoured (per `tests/bats/...` regression that landed via `be2b1d9` / "wrapWidth grep gate" — defensive scenario).

Each scenario follows the standard `IScenario` contract (`Source_Core/include/Commands/Scenarios/IScenario.h`) and emits `rows[]` so `perf-gatekeeper` can read it (closes `tooling.md` P2 about 8 perf scenarios missing `rows[]`).

### Slice 6 — Mouse / keyboard emulation density via bucket-E

Per the user's "user actions by mouse emulation" requirement. Bucket-E's `ImGuiTestContext` already provides `ItemClick("##Label")`, `ItemInput()`, `KeyPress(ImGuiKey_…)`, `MouseMove()`, `MouseClick()`. Slice 6's content is **densifying the test surface**, not adding the infrastructure (it exists).

- New bucket-E tests in `tests/ui/`: AI Assistant Preferences (6 flows per `test.md` P2 backlog), description grid-cell tooltip render, `--spawn` warmup deterministic gate (per `infra.md` P2), AgentProposalStore SQLite lane (new SQLite-aware bucket-E test sub-rig per `test.md` P2).
- Each test follows the existing `SmatchetRegister<Feature>Tests(ImGuiTestEngine*)` registration shape and aggregates into `SmatchetRegisterAllUiTests()`.
- Bucket-E SQLite lane is the new sub-rig: a fresh tempfile DB per test case so write-set isolation is bucket-A-style, but execution happens inside the UI thread frame loop. Closes the missing-bucket-E-SQLite-lane backlog entry.

### Slice 7 — Headless GL on CI (Mesa / ANGLE-D3D11)

Per `docs/backlog/agent-self-improvement/tooling.md` P2 (open ~3-5 h estimate).

- Add `mesa` (or `swiftshader` / `ANGLE-D3D11`) install step to `.github/workflows/build-and-test.yml`'s `windows-msys2-ucrt64` job.
- Wire a new job `bucket-c-screenshot-diff` that runs `scripts/dev/test-screenshot-diff.sh` against the now-available headless GL context. Continue-on-error stays `false` (advisory window over).
- Wire a new job `bucket-e-ui-tests` that runs the `ninja-ui-test-msys2` preset + the ImGui-Test-Engine-driven scenario via `scenario.run --name=UiTestScenario`. Continue-on-error initially `true` for a 1-week soak.
- The two new jobs gate on slice 6's denser bucket-E coverage; if slice 6 doesn't land in the same wave, the jobs surface only the existing 12 tests' status — still useful.

Backlog closure: `tooling.md` P2 headless-GL entry.

### Slice 8 — Sanitizer CI gates + auto-spawn debug-detective on fail

- Add `sanitizer-asan-ubsan` job to `.github/workflows/build-and-test.yml` using the existing `ninja-debug-msys2-asan` preset. Runs full `ctest` under ASAN+UBSAN. Failure surfaces line + file + sanitizer report in the job summary.
- Extend `scripts/dev/merge-watcher.py:_looks_like_cr_finding_block` to also recognise sanitizer-failure CI lines as auto-act triggers (currently only CR findings trigger auto-act). New env knob `MERGE_WATCH_AUTO_ACT_ON_SANITIZER=true` (default off, same opt-in pattern as `MERGE_WATCH_AUTO_ACT`).
- Extend `AUTO_ACT_PROMPT` (slice 9 of merge-watcher.py per PR #437) with a sanitizer-failure branch: when the trigger is sanitizer-failure (not CR-finding), the spawned session invokes `debug-detective` directly with the failing-test name + the sanitizer stderr URL as the reproducer. Skips the `coderabbit-triage` step (no CR findings to triage).
- Add `tsan`-mode as a separate opt-in job (data-race detection is noisier; gate behind `tsan-out-of-band` label per the existing `tests-out-of-band` / `perf-out-of-band` pattern).

Backlog closure: `process.md` doesn't yet have a sanitizer-CI entry; slice 8 closes the implicit gap.

### Slice 9 — SmatchetAgentDebug.h NDJSON helper

Currently referenced in `docs/agent-rules/delegation.md` § Debug-mode pause-loop but the file doesn't exist. This slice creates it.

- New `tests/_debug/SmatchetAgentDebug.h` — header-only. Provides `SMATCHET_AGENT_DEBUG_LOG(category, json_obj)` macro that appends one NDJSON line per call to `<userData>/agent-debug/<session-id>.ndjson` (or `tests/_debug/scratch/<test-name>.ndjson` under doctest / bats). Category is a short string for log-grep filtering (`backend-call`, `ui-event`, `worker-handoff`, `lock-claim`, etc.).
- Header is `#include`-able from any TU; safe to leave in production code as a no-op when `SMATCHET_AGENT_DEBUG=OFF` (default in iter / publish presets, ON in debug / asan / ui-test presets).
- New `Source_Core/include/Logger.h` macro `LOG_AGENT_DEBUG(category, msg)` that bridges: routes to NDJSON when debug-on, to `LOG_DEBUG` otherwise.
- New section in `docs/agent-rules/delegation.md` § Debug-mode pause-loop documenting the helper's contract (path, format, life-cycle).
- Bucket-A doctest in `tests/Source_Core/SmatchetAgentDebug.test.cpp` — opens a tempfile output target, calls the macro 5 times with different categories, parses + asserts each line is valid JSON with the expected category.

Backlog closure: no explicit backlog entry today; slice 9 closes the implicit gap that `delegation.md` references a non-existent helper.

### Slice 10 — debug-detective reproducer-first contract update

Per user decision: reproducer-first contract (no fallback to "user repro steps").

- Update `agents/debug-detective.md` § Process step 2 (Reproduce) — replace the "user repro steps fallback" with a hard refusal: if no deterministic reproducer (CLI command / `scenario.run --name=<x>` / Lua snippet / failing-doctest name) is supplied or discoverable, the agent's first task is to **add a scenario** that reproduces the bug (slice 5 pattern + new `Source_Core/src/Scenarios/<NewBugRepro>Scenario.cpp`).
- Update `agents/debug-detective.md` § Self-improvement template to add a new optional category: "missing-scenario" — when the agent had to add a scenario before debugging, it records the scenario name so the orchestrator can learn the patterns.
- Update `docs/agent-rules/delegation.md` § Debug-mode pause-loop to name the reproducer-first contract: phase 2 (Hypothesise) now lands AFTER phase 1.5 (Reproduce-or-Scenario-add).
- Extend `scripts/dev/test-agent-contract.sh` with a new check (the same shape as check 7b for architect's Design sections) that verifies `agents/debug-detective.md` contains the literal phrase "reproducer-first contract" so future doc rewrites don't silently soften the contract.

Backlog closure: `process.md` doesn't have an explicit entry but the spirit lines up with multiple "manual residue / user-as-verifier" entries already filed.

## Files to modify

**New (production):**
- `Source_Core/include/GitHubIssueMappingPure.h`
- `Source_Core/src/GitHubIssueMappingPure.cpp`
- `Source_Core/include/PlaneIssueMappingPure.h`
- `Source_Core/src/PlaneIssueMappingPure.cpp`
- `Source_Core/src/Scenarios/BlameOpenEntryTabScenario.cpp`
- `Source_Core/src/Scenarios/AiAssistantStreamingHappyPathScenario.cpp`
- `Source_Core/src/Scenarios/AiAssistantStreaming401Scenario.cpp`
- `Source_Core/src/Scenarios/AiAssistantStreamingTransportDownScenario.cpp`
- `Source_Core/src/Scenarios/DescriptionTooltipMarkdownRenderScenario.cpp`
- `tests/_debug/SmatchetAgentDebug.h`

**New (test):**
- `tests/support/FakeGitHubFixture.h`
- `tests/support/FakePlaneFixture.h`
- `tests/support/FakeP4Driver.h`
- `tests/support/AiHttpFixture.h`
- `tests/Source_Core/GitHubIssueMappingPure.test.cpp`
- `tests/Source_Core/PlaneIssueMappingPure.test.cpp`
- `tests/Source_Core/AiClientCancel.test.cpp`
- `tests/Source_Core/SmatchetAgentDebug.test.cpp`
- `tests/bats/p4_dual_vcs_deterministic.bats`
- `tests/ui/AiAssistantPreferences{Docking,EnterSend,ValidationBanner,SaveDiscard,TestConnection,VerifyOnSave}_test.cpp` (6 files)
- `tests/ui/DescriptionTooltipMarkdownRender_test.cpp`
- `tests/ui/SpawnWarmupDeterministicGate_test.cpp`
- `tests/ui/AgentProposalStoreSqlite_test.cpp`

**Modified (production):**
- `Source_Core/src/GitHubIssueSearch.cpp` (delegate to pure helper)
- `Source_Core/src/PlaneIssueSearch.cpp` (delegate to pure helper)
- `Source_Core/src/AppController.cpp` (env-hook fixture loaders; new scenario registration; AiClientFactory test-override probe)
- `Source_Core/include/Logger.h` (LOG_AGENT_DEBUG macro)
- `agents/debug-detective.md` (reproducer-first contract; Self-improvement template)

**Modified (test / scripts):**
- `tests/CMakeLists.txt` (`SMATCHET_TESTS_SOURCES` append + new ui-test sources)
- `scripts/dev/merge-watcher.py` (`_looks_like_cr_finding_block` extended; `AUTO_ACT_PROMPT` sanitizer branch; `MERGE_WATCH_AUTO_ACT_ON_SANITIZER` env knob)
- `scripts/dev/test-agent-contract.sh` (new check for reproducer-first contract phrase)
- `.github/workflows/build-and-test.yml` (Mesa/ANGLE install; new `bucket-c-screenshot-diff` job; new `bucket-e-ui-tests` job; new `sanitizer-asan-ubsan` job; opt-in `sanitizer-tsan` job)
- `docs/agent-rules/delegation.md` (§ Debug-mode pause-loop reproducer-first contract; SmatchetAgentDebug.h cross-link)
- `tests/fixtures/github/`, `tests/fixtures/plane/`, `tests/fixtures/p4/`, `tests/fixtures/ai/` (new fixture dirs)

## Existing utilities reused

- `tests/support/FakeTrackerClient.h` (canonical scripted `ITrackerClient`)
- `tests/support/FakeTicketSyncDeps.h`, `tests/support/FakeOfflineQueueDeps.h`
- `Source_Core/include/ITrackerBackendFactory.h` + `AppController::SetBackendFactory` (production seam already wired)
- `Source_Core/include/AiClientFactory.h::SetTestOverride` (shipped via `feat/ai-client-test-override`)
- `Source_Core/include/Commands/Scenarios/IScenario.h` (scenario interface)
- `Source_Core/src/Commands/Builtin/BuiltinCommands_Scenario.cpp` (`scenario.list` / `scenario.run` / `scenario.cancel`)
- `cpp-httplib` (already in FetchContent set per AGENTS.md § Available libs) — for `AiHttpFixture` in-process server
- `tests/bats/lock_claim.bats:25-38` stub-bin sandbox pattern (for `FakeP4Driver` stub `p4` binary)
- `ImGuiTestContext::ItemClick/ItemInput/KeyPress/MouseMove/MouseClick` (mouse emulation already in ImGui Test Engine)
- `CMakePresets.json:ninja-debug-msys2-asan` / `-tsan` / `-msan` (sanitizer presets already configured)

## UX Pillar callouts

- **Pillar 1 (Performance ≤ 6.94 ms)** — slice 5's `blame_open_entry_tab` scenario directly exercises the Pillar-1 perf regression gate already named in `docs/design/pillar-1-2-perf-review-system.md` § File-level table for the blame tokenizer hot-path. Other scenarios add `rows[]` emission so `perf-gatekeeper` can read them (closes the 8-of-15-scenarios-missing-`rows[]` gap).
- **Pillar 2 (UI never freezes >100 ms)** — slice 4's `AiClientCancel.test.cpp` asserts cancel propagation < 100 ms on every AI client. Slice 6's bucket-E coverage densifies the visible-cue regression coverage.
- **Pillar 3 (Never crash)** — slice 8's sanitizer CI gates catch ASAN / UBSAN failures *automatically*; this is the strongest single Pillar-3 strengthening currently feasible.
- **Pillar 4 (Accessibility)** — out of scope; pillar 4 is backlogged per AGENTS.md § UX Pillars.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

Per `docs/design/pillar-1-2-perf-review-system.md`. This plan does touch `Source_Core/` (slices 1, 2, 5, 9), so per-slice perf-gate notes:

- **Slice 1 / 2** (pure helper extraction from `GitHubIssueSearch.cpp` / `PlaneIssueSearch.cpp`) — pure code move, zero perf delta expected. Perf-gate: `perf-pr-fast.yml` runs the scenario subset named by the diff-→-scenario map; for an extraction-only diff the map produces no matches → gate runs no scenarios, exits SKIP. Recommended pre-push local check: none required.
- **Slice 5** (new scenarios) — adds scenarios but doesn't change product code beyond `Scenarios/*Scenario.cpp` registration. Perf-gate: the new scenarios become eligible for `perf-pr-fast.yml` selection on future PRs that touch their target paths; this PR itself triggers no measurement.
- **Slice 9** (`SmatchetAgentDebug.h` macro in `Logger.h`) — header-only macro; when `SMATCHET_AGENT_DEBUG=OFF` (default in iter / publish) the macro expands to no-op so zero per-frame overhead. Perf-gate: `perf-pr-fast.yml` scenario subset includes a baseline render scenario; verify the macro's no-op compile-out doesn't regress. Recommended pre-push: `bash scripts/dev/perf-run.sh PriorityGridScrollScenario && python scripts/dev/perf-compare.py`.
- **Slices 3, 4, 6, 7, 8, 10** — test-only or CI-only diffs; no `Source_Core/` impact → perf-gate N/A per the plan-template rule.

## Risks / non-goals

- **Fixture drift** — fixtures generated from real Jira / GitHub / Plane responses today will go stale as those APIs evolve. Mitigation: every fixture file ships with a `// captured-against: <api-version> <date>` header; a quarterly `scripts/dev/refresh-tracker-fixtures.sh` cron task pulls fresh captures and runs the diff against the existing pure mappers. Stale-fixture as a backlog signal, not a blocker.
- **Mesa-on-CI flakiness** — software GL rasterisation on GitHub Actions Windows runners may produce subtle font-rendering differences vs the developer's GPU. Slice 7's bucket-C job starts in continue-on-error mode; bootstrap goldens captured by the CI runner once Mesa is wired, not the developer's machine.
- **ASAN noise** — third-party libraries (cpr, ImGui, SQLite) may surface known false positives. Slice 8 ships an LSAN suppression file (`tests/_debug/lsan-suppressions.txt`) populated as-needed; suppressions are reviewed in `code-review` to avoid silencing real bugs.
- **Reproducer-first contract may force scenario-add bloat** — slice 10's "agent's first task is to add a scenario" rule could spawn many low-value scenarios. Mitigation: the scenario-add must follow `Source_Core/src/Scenarios/` registration shape and be reviewed by `code-review` like any other code change; orphan-scenario sweep added to `git-janitor` end-of-session.
- **p4 stub limitation** — `FakeP4Driver`'s stub binary doesn't cover every `p4` command shape. Mitigation: explicit allow-list of commands the dual-VCS scripts actually call (`p4 streams`, `p4 change`, `p4 shelve`, `p4 submit`, `p4 verify`, `p4 counter`, `p4 describe`); commands outside the allow-list error loudly so a new dual-VCS script can't silently work against the stub and break against real p4d.

**Non-goals:**
- Replacing real-p4d / real-Jira / real-GitHub integration testing entirely. The fakes are for *autonomous debug* — final-mile integration smoke against the real services stays as opt-in tests gated by `SMATCHET_TEST_REAL_<BACKEND>=1`.
- Reducing the existing manual-verification PRs that legitimately require visual / acceptance judgement (Pillar-4 work, golden-image bootstrap, theme polish). The visual-validation exception in `AGENTS.md` § Autonomous ship-loop default still applies.
- Replacing `coderabbit-triage` with debug-detective for code-review tasks. The two agents stay distinct; slice 8 only routes sanitizer-failure auto-act to debug-detective (not CR-finding auto-act, which keeps the coderabbit-triage routing from PR #437).

## Verification

Per AGENTS.md § Verification automation, every item classified into a bucket (A CLI / B scenario / C screenshot / D sanitizer / E ImGui Test Engine) — no manual residue.

| # | Verification item | Bucket |
|---|---|---|
| V1 | `bash scripts/dev/test-all.sh` exits 0 after every slice | A |
| V2 | `ctest --output-on-failure` exits 0 with the new bucket-A tests included | A |
| V3 | New `GitHubIssueMappingPure.test.cpp` / `PlaneIssueMappingPure.test.cpp` / `AiClientCancel.test.cpp` / `SmatchetAgentDebug.test.cpp` doctests all PASS | A |
| V4 | `tests/bats/p4_dual_vcs_deterministic.bats` all cases PASS via the stub-p4 driver | A |
| V5 | `SMATCHET_TEST_GITHUB_BACKEND_FIXTURE=<fixture> ./Smatchet.exe scenario.run --name=UiTestScenario` loads the fixture; agent-debug NDJSON contains a `backend-call` entry per fixture row | B |
| V6 | `SMATCHET_TEST_PLANE_BACKEND_FIXTURE=<fixture> ./Smatchet.exe scenario.run --name=UiTestScenario` same as V5 for Plane | B |
| V7 | `./Smatchet.exe scenario.run --name=blame_open_entry_tab --frames=600` emits a `rows[]` JSON output; `perf-compare.py` accepts it without baseline-missing error | B |
| V8 | `./Smatchet.exe scenario.run --name=ai_assistant_streaming_happy_path` against `AiHttpFixture` completes without hitting the network; AI panel shows the final concatenated message | B |
| V9 | Bucket-C screenshot diff job in `.github/workflows/build-and-test.yml` runs against Mesa-on-CI; first run bootstraps `tests/golden/<scenario>.png` from the CI capture; subsequent runs diff-PASS | C |
| V10 | Bucket-E `ninja-ui-test-msys2` job runs 12 existing tests + 9 new tests (slice 6); all PASS | E |
| V11 | `sanitizer-asan-ubsan` CI job runs full `ctest` under ASAN+UBSAN; passes with the LSAN suppressions in place | D |
| V12 | `MERGE_WATCH_AUTO_ACT=true MERGE_WATCH_AUTO_ACT_ON_SANITIZER=true` watcher poll on a PR with a deliberate ASAN failure spawns `debug-detective` (not `coderabbit-triage`); spawned session reads the failing-test name from the CI log | A (bats-coverable via merge_watcher.bats) |
| V13 | `scripts/dev/test-agent-contract.sh` new check verifies `agents/debug-detective.md` contains "reproducer-first contract" literal | A |
| V14 | `agents/debug-detective.md` when invoked on a bug with no existing scenario, the agent's first emit is a new `Source_Core/src/Scenarios/<NewBugRepro>Scenario.cpp` file (verified by a `tests/Source_Core/_meta/debug_detective_reproducer_first_smoke.test.cpp` bucket-A meta-test that loads the agent prompt and asserts the literal "reproducer-first" appears in step 2) | A |

**Bucket-E SQLite lane** verification: slice 6's `AgentProposalStoreSqlite_test.cpp` is the first test in the new sub-rig; its passing is the lane's gate.

**Manual residue**: **none**. Every verification item is bucketed; the plan does not require any "user opens window and observes" step. The single exception is the *bootstrap* of bucket-C goldens on first Mesa-CI run, which is a one-time `cp build/<preset>/*.png tests/golden/` + manual `git add` step — covered by the golden-image-approval contract in AGENTS.md.

## Out of scope (flagged, not designed)

- **Real-p4d integration test** — slice 3 covers stub-p4 only. The real-p4d test stays as `scripts/dev/test-p4-dual-vcs.sh` gated by env knob; CI-skipped per existing pattern.
- **VCR-style replay over the real cpr / httplib HTTP layer** — slice 4 uses an in-process httplib server, not a recording proxy. A recording layer is more powerful (real responses captured from production traffic) but vastly more complex; defer.
- **Pillar 4 (accessibility)** — backlogged per AGENTS.md § UX Pillars.
- **C4 prong 4** (replacing the spawned-Claude session entirely with a deterministic CLI fixer) — out of scope; C4 prongs 1+2+3 (shipped via #428, #431, #437) plus this plan's slice 8 sanitizer routing close the C4 design space.
- **Replacing `code-review` with debug-detective on sanitizer failures** — the two agents stay distinct; slice 8 only adds the sanitizer-failure trigger for debug-detective, not for code-review.
- **Cross-platform CI (Linux / macOS)** — the plan's CI changes target Windows + MSYS2 UCRT64 (the canonical PR-gating job). Linux / macOS coverage is a separate plan.

## Implementation log

<!-- populated when the slices ship; one bullet per merged PR per AGENTS.md § Plan revision after implementation -->

## Deviations from plan

<!-- populated when the slices ship -->

## Verification (actual)

<!-- populated when the slices ship; mirror the V1-V14 list in § Verification with PASS / FAIL / not-run per item -->
