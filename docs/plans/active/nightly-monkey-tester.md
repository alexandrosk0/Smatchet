# Plan — Nightly "code monkey" tester

> **Slug**: `nightly-monkey-tester` (matches this file's basename without `.md`).
>
> **Status**: `active` — driving in-flight work. Values: `active` · `shipped` · `blocked` / `deferred`.
>
> **Usage**: Layer 1a (command-registry pre-handler fuzz) is the shipped slice; Layers 1b/2/3/4 are flagged follow-ups (see § Out of scope).

## Context

Smatchet's test suite is deep but entirely **scripted** — every input is one a human thought to write. It has no *stochastic exploratory* tester, so the failure class that only appears on input #47,000 (a coercion overflow, a bounds off-by-one on a UTF-8 boundary, a fuzzy-match crash on adversarial bytes, a destructive command that slips its confirm gate) has no automated hunter. Existing nightlies (`sanitizer-nightly.yml`, `tsan-linux-nightly.yml`, `fuzz-smoke.yml`) sanitize *scripted* runs; none drive the command surface with randomized data.

Intended outcome: after this lands, a **seeded, reproducible monkey** fuzzes `CommandRegistry::Dispatch` nightly under ASan+UBSan and auto-files a `bug` issue (carrying the reproducing seed) when it trips — the command registry gains a stochastic backstop with a one-command replay.

Originating request: maintainer ask for "a code-monkey nightly test." Layered rollout agreed: deterministic core gate first, agent-driven pass deferred.

## Approach

**Layer 1a (this slice)** adds a standalone seeded driver (`tests/monkey/monkey_command_registry.cpp`, target `SmatchetMonkeyCli`) that boots a headless `AppController` with `RegisterBuiltinCommands` — exactly the un-`Initialize`d `BuiltinsFixture` recipe from `tests/Commands/BuiltinCommandsDispatch.test.cpp` — then fires a long seeded sequence of randomized dispatches. Every generated input is **guaranteed to be rejected before the command's handler runs** (missing-required / un-coercible-type / out-of-bounds / over-length / destructive-unconfirmed / unknown-name), so no app-mutating handler executes headless and the oracle is crisp and false-positive-free: **every probe must return `!Ok` with no exception and a well-formed error envelope.** The randomization exercises the parse/coerce/bounds/fuzzy-match/confirm-gate code with adversarial data scripted tests never generate.

The non-obvious trade-off (why *guaranteed-reject* rather than "run handlers"): running an *arbitrary* handler headless can deref `g_ui`/ImGui frame state or hop to the UI thread — state that only exists during the GUI frame loop — producing false-positive crashes, the exact reason `BuiltinCommandsDispatch.test.cpp` stays pre-handler. (Note: `Initialize`→`InitConfig`→`WireCoreServices` *does* wire the core services — `ticketSync_`/`offlineQueue_`/`fieldEdit_`/… — so those are live; the un-wired hazard is `g_ui`, not the services.) Handler-body execution is therefore done as **1b** with a curated allow-list of read-only commands that touch neither `g_ui` nor the UI thread — not by running everything.

Runs on `ubuntu-latest` via the existing `posix-core-check` preset + `-DSMATCHET_SANITIZER=asan` (clang-linux `asan` = `-fsanitize=address,undefined`); no new preset needed.

## Files to modify

1. `Source/Core/include/Commands/ParamValueSynthPure.h` — **new** pure header; `SynthValidValue` & friends extracted from the scenario so the monkey and the sweep share one synthesis rule.
2. `Source/Core/src/Commands/Scenarios/CommandContractSweepScenario.cpp:31` — drop the local anon-namespace copies; `#include` the new header (behavior-preserving; ADL resolves the calls).
3. `tests/monkey/CommandArgSynth.h` — **new**; seeded fuzz generators (raw distribution-free RNG helpers + `ApplicableProbes`/`BuildProbe`) layered on the extracted synth.
4. `tests/monkey/monkey_command_registry.cpp` — **new**; the standalone seeded driver (`int main`), seed logging, deny-set, oracle, per-`ErrorCode` histogram.
5. `tests/CMakeLists.txt:1664` — `SmatchetMonkeyCli` target (mirrors the `SmatchetCommandsTests` link recipe) + `smatchet_apply_sanitizers` + the `monkey_command_registry_smoke` ctest.
6. `CMakeLists.txt:1576` — `smatchet_apply_sanitizers(SmatchetCore_PosixCheck)` (no-op unless a sanitizer is requested) so ASan/UBSan actually instrument the fuzzed core.
7. `.github/workflows/monkey-nightly.yml` — **new**; nightly cron + dispatch, configure/build/run under ASan+UBSan, surface report + seed, auto-file/update the `bug` issue.
8. `.github/workflows/build-and-test.yml:2056` — one extra step in `mobile-posix-core-check` builds + smoke-runs the monkey (fixed seed) so the driver never bit-rots.

## Existing utilities reused

- `SynthValidValue` / `FirstRequiredParam` / `SynthValidRequiredArgs` / `FirstScalarParam` — moved to `Source/Core/include/Commands/ParamValueSynthPure.h:33` (were `CommandContractSweepScenario.cpp` anon-namespace).
- `ParamValueWithinBounds` — `Source/Core/include/Commands/ParamBoundsPure.h:26` (used by the local ASan self-check to prove each bounds probe genuinely fails).
- `BuiltinsFixture` headless-boot pattern — `tests/Commands/BuiltinCommandsDispatch.test.cpp:41` (bare `AppController` + `RegisterBuiltinCommands`, no `Initialize`).
- `CommandRegistry::All()` / `Dispatch()` — `Source/Core/include/Commands/CommandRegistry.h:46`.
- `SmatchetCore_PosixCheck` archive + `SmatchetCommandsTests` link recipe — `tests/CMakeLists.txt:1634`.
- Auto-file/update-`bug`-issue shell block — copied from `.github/workflows/sanitizer-nightly.yml:99`.

## Extraction sizing (this plan EXTRACTS code)

One EXTRACT: the four synth helpers (~55 lines) move from `CommandContractSweepScenario.cpp`'s anon namespace into `ParamValueSynthPure.h`. STAYS: the scenario's probe/violation logic. The source file net-shrinks (~65 lines removed, one `#include` + a 4-line note added); no over-cap file is involved, so no cap math applies — the extraction exists to enable reuse by `tests/monkey`, not to shrink a whale.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — a header-only extraction (identical codegen) plus test-only binaries/workflows; nothing on any frame path.
- **Pillar 2 (UI-thread never blocks)**: no impact — the driver never runs the UI loop; no new sync I/O reachable from `ImGui::*`.
- **Pillar 3 (never crash)**: net-positive — the monkey exists to surface crash/UB before users do; it adds no shipping code path.
- **Pillar 4 (accessibility)**: N/A — no UI surface touched.

## Perf-review-system gates (diff touches `Source/Core/`)

1. **PR-fast CI** — N/A: the two `Source/Core` touches are a pure header extraction + its `#include` swap; no scenario's timed path changes.
2. **Pillar 2 static scanner** — N/A: no new sync-I/O reachable from `ImGui::*` (test-only driver).
3. **Dispatcher drain** — N/A: shipped code does not touch `MainThreadDispatcher::Drain()` (the drain reference is deferred to the 1b test path).
4. **Visible-cue bucket-E harness** — N/A: no new > 100 ms sync stall.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` markers added.

## Risks / non-goals

- **False positives from running handlers** → mitigated by construction: 1a only ever sends guaranteed-pre-handler-reject inputs (proven against the real `ParamValueWithinBounds` in the local ASan self-check). Accepted: 1a does not exercise handler *bodies* — that is 1b.
- **Preset composition** → verified: `posix-core-check -DSMATCHET_SANITIZER=asan` yields ASan+UBSan on clang-linux (`cmake/Sanitizers.cmake:116`). No new preset.
- **Archive/exe sanitizer coherence** → `smatchet_apply_sanitizers` added to the archive and every exe that links it, so an ASan build dir links the runtime consistently (no-op otherwise).
- **Non-goal**: the ImGui UI monkey (Layer 2), parser fuzz drivers (Layer 3), and the AI-agent exploratory pass (Layer 4) — flagged in § Out of scope.

## Verification

- **Bucket A (pure-logic ctest)**: `monkey_command_registry_smoke` (`SmatchetMonkeyCli --seed=1 --steps=500`) registered in `tests/CMakeLists.txt`; runs in the `mobile-posix-core-check` lane. The existing `CommandContractSweep` + `SmatchetCommandsTests` stay green (extraction is behavior-preserving).
- **Sanitizer**: `monkey-nightly.yml` builds `posix-core-check -DSMATCHET_SANITIZER=asan` and runs the driver under ASan+UBSan with a `timeout` (hang = finding). Local proxy for the novel logic: `tests/monkey/CommandArgSynth.h` + `ParamValueSynthPure.h` compiled with `g++ -fsanitize=address,undefined` against a fixture registry — asserts same-seed determinism and that every probe fails the real bounds/required rule (passed: 6000 probes, no sanitizer trips). Full-archive build/run is CI-only here (curl FetchContent is egress-blocked in the dev sandbox).
- **Bucket E**: N/A — no ImGui surface (Layer 2 follow-up).
- **Build gate**: `SmatchetMonkeyCli` builds on the Linux `posix-core-check` lane; no Windows dual-target impact (test-only, Linux-guarded).
- **Doc validation**: `scripts/dev/test-docs.sh` green (this plan indexed via `test-plan-index.sh --fix`).
- **Plan stress-test — `grill-with-docs`**: plan pressure-tested against the command-system domain model; the load-bearing claim (handlers must not run headless) was verified directly against `BuiltinCommandsDispatch.test.cpp` and `AppController::Initialize`, which reshaped 1a into guaranteed-reject-only and pushed handler execution to 1b.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

- **Layer 1b — handler-body execution** *(shipped)*: `--mode=handlers` boots a fully-initialized `AppController` vs the fake Jira backend (`ScriptedTrackerBackendFactory` + `JiraFakeTrackerFixture`, no network) and runs a small **allow-list** of read-only, `g_ui`-free, no-UI-hop commands (`commands.list`/`help`/`search`/`recents`, `config.get`/`path`, `perf.snapshot`/`frame_count`, `debug.thread_dump`), draining the main-thread dispatcher after each. Oracle: no crash + well-formed envelope. Grown only by reading handlers.
- **Layer 2 — ImGui UI monkey** *(shipped)*: `tests/ui/ui_monkey.test.cpp` boots the real app under the ImGui Test Engine and sprays a seeded stream of random keyboard/mouse INPUT events at the live UI (no assertions — crash-only failure). Registration is **opt-in** (`SMATCHET_UI_MONKEY=1`) so it's inert under the required `ui_test.run --all`; the advisory `ui-monkey-nightly.yml` (windows-2022 + llvmpipe, mirrors bucket-E) runs it via `--name="UiMonkey/*"` and auto-files a `bug` issue with the seed. Uses only in-tree-verified engine primitives (no `GatherItems`).
- **Layer 3 — parser fuzz drivers** *(shipped)*: three new `fuzz_*` targets over security-sensitive pure string parsers with clean single-`.cpp` closures — `fuzz_jql_escape` (`tracker_jql::QuoteLiteral`, JQL-injection boundary), `fuzz_ai_error_redact` (`RedactProviderErrorBody`, secret redaction), `fuzz_ai_endpoint_sanitize` (`SanitizeAiEndpointUrl`/`ExtractUrlHost`, config-write SSRF). Each with a seed corpus; auto-discovered by `fuzz-smoke.yml`. Verified locally: drivers+closures compile/link/run clean under `g++ -fsanitize=address,undefined` on adversarial seeds.
- **Layer 4 — AI-agent exploratory pass** *(deferred — needs infra/governance, not shippable autonomously)*: a nightly that drives an LLM agent over the CLI (`Smatchet cmd …`) / MCP (`tools/list` + `tools/call`) surface, lets it explore like a user, and files issues for what looks broken. Deferred deliberately: it requires (1) an agent runtime + a provider **API-key secret** in CI (real per-run cost), (2) maintainer governance sign-off under `AI_POLICY.md` (an autonomous agent acting on the repo), and (3) human triage of non-deterministic findings — none of which should be provisioned without an explicit owner decision. Ready-to-pick-up shape when greenlit: a `workflow_dispatch` + weekly-cron job that builds the CLI, seeds the agent with `CLI_GUIDE.md` + `MCP_GUIDE.md` and a fixed fake-backend fixture, caps turns/tokens, and posts a single summarized issue per run (label `agent-explore`). Layers 1–3 (deterministic monkeys) are the value floor; Layer 4 only earns its keep once they've soaked.

## Implementation log
*(populated post-ship — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip the § Status header to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
