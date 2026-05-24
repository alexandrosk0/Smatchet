# Autonomous debugging without credentials or user input — three coupled decisions

# Status

**Accepted (2026-05-23).** Three decisions in one ADR because they are coupled — they co-deliver the autonomous-debug end-state documented in [`docs/CONTEXT.md`](../CONTEXT.md) § Autonomous debugging and built out across the 11 slices in [`docs/design/autonomous-debugging-no-creds.md`](../design/autonomous-debugging-no-creds.md). Splitting into three ADRs would risk a future reader catching one without the context of the other two.

# Context

Smatchet's debug loop today requires three categories of human input that block end-to-end autonomy:

1. **Live credentials** — every tracker backend (Jira / GitHub / Plane), every AI client (OpenAI / Anthropic / Ollama), and the `P4Blame` C++ feature shell out to real services. Reproducing a backend-touching bug needs real keys.
2. **Interactive verification** — bucket-C screenshot diff + bucket-E ImGui Test Engine are wired but not gated in CI (Windows headless-GL gap). UI bugs require "user opens window and observes."
3. **Reproducer hand-off** — `agents/debug-detective.md` today accepts a CLI / scenario / Lua snippet as the *preferred* input but falls back to "user repro steps." Falls back too often.

The autonomous-debug plan closes all three with 11 slices grouped into three waves (Wave A = backend fakes + registry refactor + CI prerequisites + NDJSON helper; Wave B = scenarios + bucket-E densification + contract update; Wave C = sanitizer auto-act loop closure). Most slice-level decisions are mechanical (extract pure helper, add fixture loader, register scenario factory). Three load-bearing meta-decisions are not mechanical — they shape every slice that depends on them and would surprise a future reader looking at the code without the plan-doc to hand.

The grill-with-docs pass (recorded inline on the plan-doc's commit history under wip/plan-autonomous-debugging-no-creds) validated each decision against the existing seams (`ITrackerBackendFactory::SetBackendFactory`, `AiClientFactory::SetTestOverride`, `ScenarioRunner::RegisterFactory`, the existing `tests/support/Fake*.h` shape, `agents/debug-detective.md` v4 contract) and the sibling `deterministic-jira-test-backend.md` plan that anchors the tracker-backend pattern.

# Decision

## (a) Three-pattern backend-fake recipe

Each backend that today requires credentials gets a different fake shape, chosen by the backend's real-world seam rather than unified onto a single mechanism. Documented at `docs/design/autonomous-debugging-no-creds.md` § Approach point 1.

- **Tracker backends (GitHub + Plane)** extend the `FakeTrackerClient` + fixture-loader pattern from `deterministic-jira-test-backend.md`. Per-backend `tests/support/Fake<Backend>Fixture.h` thin wrapper + `SMATCHET_TEST_<BACKEND>_FIXTURE=<path>` env hook routed through `AppController::SetBackendFactory`. This works because `ITrackerClient` is a clean injection point already used by `FakeTrackerClient`; the only per-backend variation is the pure JSON-to-`CachedTicket` mapper extracted from each backend's `*IssueSearch.cpp` anonymous namespace.
- **`P4Blame` C++ feature (annotate + describe-cache)** injects a **runner-seam fake** at the single `P4Blame.cpp:P4RunCommand` spawn-site via a new `BlameAnalysisConfig::P4RunOverride` function-pointer field. `tests/support/FakeP4Runner.h` installs a lambda that returns canned `(exit, stdout, stderr)` triples keyed on argv-prefix. **Explicitly rejected**: a stub `p4` binary on `PATH`. The dev-environment `scripts/dev/p4-*.sh` shell layer (30+ subcommand shapes, opt-in `SMATCHET_AGENT_VCS=p4`, exercised manually) is out of scope; the C++ blame feature has exactly two subcommands and one spawn-site, so the runner-seam approach is dramatically smaller.
- **AI clients (OpenAI / Anthropic / Ollama)** sidestep the cpr layer entirely via the existing `AiClientFactory::SetTestOverride` seam. `tests/support/StubAiClient.h` is an `IAiClient` impl that emits canned SSE-shape deltas via the `OnDelta` callback directly. **Explicitly rejected**: an in-process httplib server (`AiHttpFixture`). The httplib approach depends on assumptions that fail silently on CI (env-var honour at client construction time; loopback reachability under Windows runner firewall hardening; ctest -j port collision) — failure mode is silent hang to the 30-min CI timeout. The cpr layer's correctness is verified at PR time by `code-review`'s grep-for-secret discipline, not by an automated test against a fake server.

The deliberate non-unification is the surprise. A future reader sees three different fake shapes and wonders "why isn't this uniform?" The answer: each backend's *real* seam is different, and forcing a uniform fake shape would either (i) require a unified abstract `IFake*` layer the production code doesn't have, or (ii) pick a single shape (e.g. in-process server for all) that doesn't fit the others' constraints. Per-backend selection follows from per-backend reality.

## (b) `SmatchetAgentDebug.h` NDJSON helper — parallel logging surface alongside `Logger.h`

A new header-only macro `SMATCHET_AGENT_DEBUG_LOG(category, json_obj)` appends one structured NDJSON line per call to `<userData>/agent-debug/<session-id>.ndjson` (production) or `tests/_debug/scratch/<test-name>.ndjson` (doctest / bats). Closed-set category enum (`backend-call`, `ui-event`, `worker-handoff`, `lock-claim`, `scenario-phase`, `cli-command`, `temp-debug`). 5-field schema (`ts`, `category`, `pid`, `tid`, `payload`). Mutex-guarded ofstream; one file per session; `git-janitor` end-of-session prunes files older than 30 days. Full operational contract at `docs/design/autonomous-debugging-no-creds.md` § Slice 7.

**Why a parallel surface, not extending `Logger.h`**: `Logger.h`'s `LOG_*` macros are optimised for **operator-readable text** with severity classification (`LOG_INFO`, `LOG_WARN`, `LOG_ERROR`). The agent's read-path needs **structured, grep-able, per-category-filterable** data with per-line schema discipline so an automated reader can deterministically extract just the `backend-call` entries (or just the `worker-handoff` entries) without a regex over freeform text. Extending `LOG_*` with JSON-mode would either bloat the macro (every call site picks text vs JSON), break the existing log readers, or silently mix the two formats in one file.

**Why closed-set category enum**: open-set categories grow unboundedly over time as agents invent new ones. A closed set forces every new category to be a documented amendment to slice 7, which keeps the agent prompts that grep for categories synchronised with the producers.

**Why per-session file (vs append-across-sessions)**: agents read the NDJSON to diagnose one debug-loop run; cross-session content adds noise. The `<session-id>` = `<unix-epoch>-<pid>` suffix guarantees uniqueness; the spawned-subprocess correlation works via the `SMATCHET_AGENT_DEBUG_SESSION_ID` env var rather than file discovery.

## (c) Reproducer-first contract for `agents/debug-detective.md`

The agent's process gains two new phases before phase 1 (Clarify):

- **Phase 0 — Concreteness check.** The incoming bug description must name (a) the breaking surface, (b) an observable failure, (c) the input shape. If any is missing, the agent emits exactly one `AskUserQuestion`; once concrete, phases 1+ never ask again. This is the *only* user-input point in the loop.
- **Phase 0.5 — Existing-scenario reuse.** Before considering scenario-add, the agent searches `Source_Core/src/Commands/Scenarios/` for a scenario whose failure shape covers this bug-class (= injection point + render path). When found, the existing scenario is *parametrized* (new CLI arg / fixture variant / new sub-case in `OnTick`) rather than forked into a near-duplicate.

Phase 2 (Reproduce) loses its "user repro steps fallback." If no deterministic reproducer is supplied or discoverable and no existing scenario can be parametrized, the agent's first action is to **add a scenario** that reproduces the bug — on the same branch as the fix. Scenario-add = one new `.cpp` under `Source_Core/src/Commands/Scenarios/` + one line in the slice-5 `SmatchetScenarioRegistry.cpp` table.

The contract is verified by a literal-grep check in `scripts/dev/test-agent-contract.sh` ("reproducer-first contract" must appear in `agents/debug-detective.md`); soft-rewrites that drop the phrase fail the check loudly.

**Why hard refusal, not soft preference**: the autonomous-debug loop's promise — "at most one user-input point" — only holds if phase 2 doesn't ask. Soft preferences slide into soft fallbacks under deadline pressure ("the agent didn't have a reproducer but we needed the bug fixed, so we let it pause and ask the user"), and once that happens the entire end-state degrades to "interactive debug with extra steps." The hard refusal makes the failure mode visible: missing reproducer → explicit scenario-add commit on the branch, with the bug-class recorded in the self-improvement template's new `missing-scenario` category so the orchestrator's quarterly pattern-mining loop can spot duplicate scenarios across the lifetime of the codebase.

# Consequences

**Positive**:

- The autonomous-debug loop's promise becomes verifiable. Slice 11's `MERGE_WATCH_AUTO_ACT_ON_SANITIZER=true` env path routes sanitizer-failure CI signals to `debug-detective` without spawning any user-interaction surface; the agent reads the failing-test name + sanitizer stderr URL + slice-7 NDJSON and produces a diagnosis without live credentials.
- Per-backend fake shapes (decision a) avoid the abstract-`IFake*`-layer overhead that a unified-fake approach would impose; each fake's complexity matches its backend's real complexity.
- NDJSON-over-`Logger.h` (decision b) keeps `LOG_*` operator-readable while giving agents a deterministic structured surface. Closed-set category enum + per-session file + 30-day prune bound the disk + cognitive cost.
- Reproducer-first (decision c) creates a positive feedback loop: every new bug-class shipped through the autonomous loop adds a scenario; over time, the scenario library densifies, and phase 0.5's existing-scenario-reuse hit rate climbs. Phase-0-check `AskUserQuestion` count drops as bugs become better-described over time (CI signals are already fully-specified).

**Negative — accepted**:

- Three fake patterns (decision a) means a contributor adding a new backend (e.g. a fourth tracker) has to pick which pattern fits, with the per-backend reasoning to-hand. Mitigation: § Approach point 1 documents the per-backend selection logic with one-line reasoning per pattern.
- NDJSON helper (decision b) adds a second logging surface to maintain. Mitigation: closed-set category enum + slice-9 grep gate on drift-warning headers + slice-7 V7.1-V7.6 verification items keep the helper's contract tight.
- Reproducer-first (decision c) increases the scenario file count (slice 8 adds 5 immediately; slice 10's scenario-add path projects ~1/month). Mitigation: slice 5's `SmatchetScenarioRegistry` table-driven registration; slice 10's bug-class consolidation rule + orphan-scenario sweep in `git-janitor` step 10.5. The scenario library is allowed to grow because every scenario is a deterministic repro that the autonomous loop can run again.

# Considered Options

- **Unify backend fakes onto a single pattern** (rejected for decision a). Would require either an abstract `IFake*` layer the production code lacks, or picking one shape (in-process server / runner-seam fake / `SetTestOverride` stub) that fits the others' constraints. Each backend's real seam is different; forcing uniformity adds production complexity without test-side benefit.
- **Extend `Logger.h` with a JSON-mode** (rejected for decision b). Every call site would need to pick text-vs-JSON; existing log readers break or silently mix formats; the file-vs-stdout-vs-file-per-category routing diverges between `LOG_*` and the agent-read use case.
- **Soft "user repro steps fallback" in debug-detective** (rejected for decision c). The autonomous loop promise — "at most one user-input point" — degrades under deadline pressure. Soft preferences slide into soft fallbacks; the failure mode becomes invisible.
- **Three separate ADRs** (rejected). The three decisions co-exist as a single coherent autonomy-strategy; splitting risks a future reader catching one without the context of the other two.
