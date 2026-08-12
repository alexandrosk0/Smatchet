# `AiClientFactory::SetTestOverride` + `AiPrefsTestConnection` extraction
<!-- plan-date: 2026-05-18 -->

## Context

PR #214 shipped three AI Assistant bucket-E TUs. TU#3 V2 (`VerifyOnSave_TestConnection_SetsResult`) + V3 (`VerifyOnSave_CancelOnClose_ShortCircuits`) shipped as deferred placeholders (`LogInfo + IM_CHECK(true)`) because:

1. **No test-injection seam** in `AiClientFactory::MakeAiClient` — every probe instantiates a real `OpenAiClient` / `AnthropicClient` etc. Can't drive a deterministic success path from a test.
2. **`runProbe` is a closure** inside `BeginTabItem("Assistant")` (`Source_Core/src/SmatchetPreferencesUi.cpp:520-680`). Engine-side `ItemClick` on the Assistant tab + the Test-connection button proved unreachable in the existing bucket-E harness — agent #214 reported "BeginTabItem('Assistant') sub-items were not reachable through engine-side ItemClick paths".

This slice lifts both blockers.

**Overlap constraint:** active plan-lock `whisper-dictation-phase-f` (PR #219, OPEN, MERGEABLE+UNSTABLE) holds `Source_Core/src/SmatchetPreferencesUi.cpp` in its write set. This slice **does not** touch that file. Instead:

- Add the seam to `AiClientFactory`
- Extract the `runProbe` body into a new free function `AiPrefsTestConnection::TriggerProbe(d, app, provider)` under `Source_Core/{include,src}/AiPrefsTestConnection.{h,cpp}`
- TU#3 V2/V3 call the free fn directly (bypass UI entirely; no `ItemClick("Assistant")` path)
- A **follow-up 5-line PR** after #219 merges rewires `SmatchetPreferencesUi.cpp`'s `runProbe` lambda to call `AiPrefsTestConnection::TriggerProbe`. Drift window <24h, flagged via comments in both spots.

**Intended outcome:** TU#3 V2 + V3 lift from deferred placeholders to live coverage. Closes the success-completion-branch coverage gap.

## Approach

### Part A — `AiClientFactory::SetTestOverride`

[`Source_Core/include/AiClientFactory.h`](../../Source_Core/include/AiClientFactory.h) adds:

```cpp
namespace AiClientFactory {

// Test seam: when set, MakeAiClient calls this fn instead of the provider-kind
// switch. Lifetime managed by caller — typically a TU-local function + RAII
// scope guard.
using TestOverrideFn = std::unique_ptr<IAiClient> (*)(AiProvider);
void SetTestOverride(TestOverrideFn fn);

} // namespace
```

[`Source_Core/src/AiClientFactory.cpp`](../../Source_Core/src/AiClientFactory.cpp):

```cpp
namespace {
TestOverrideFn s_testOverride = nullptr;
}

void SetTestOverride(TestOverrideFn fn) { s_testOverride = fn; }

std::unique_ptr<IAiClient> MakeAiClient(AiProvider provider) {
    if (s_testOverride) {
        return s_testOverride(provider);
    }
    switch (provider) { /* existing body unchanged */ }
}
```

Function-pointer (not `std::function`) keeps `<functional>` out of the public header + avoids allocation on every `MakeAiClient` call.

### Part B — `AiPrefsTestConnection` extraction

Lift `runProbe` body (`SmatchetPreferencesUi.cpp:520-680`) verbatim into a new free fn:

```cpp
// AiPrefsTestConnection.h
namespace AiPrefsTestConnection {
// Kicks off the async Test-connection probe. Spawns a worker thread that
// drives ProbeReachability → SendStreaming → posts result via MainThreadDispatcher.
// Mutates: g_ui.assistantPrefsTestInFlight, ...Result, ...ResultType, ...Cancel.
// Thread: must be called from UI thread.
void TriggerProbe(UiDrawSession& d, AppController& app, AiProvider provider);
}
```

`SmatchetPreferencesUi.cpp` is **NOT TOUCHED** in this PR. Follow-up PR after #219 lands will replace the inline `runProbe` lambda with `AiPrefsTestConnection::TriggerProbe(d, app, selectedKind)`.

### Part C — TU#3 V2 + V3 lift

[`tests/ui/ai_prefs_autosave_flow.test.cpp`](../../tests/ui/ai_prefs_autosave_flow.test.cpp) — replace the deferred-placeholder bodies for V2 + V3 with real coverage:

**V2 — `VerifyOnSave_TestConnection_SetsResult`:**
- TU-local `StubAiClient` (success): `ProbeReachability` returns `""`, `SendStreaming` emits one delta with `IsFinal=true`.
- Test sets `AiClientFactory::SetTestOverride(MakeStubSuccess)` + `g_ui.cfg.AiProviderKind=0` + dummy AiModelOpenAi + dummy ApiKey.
- Calls `AiPrefsTestConnection::TriggerProbe(g_ui, *app, AiProvider::OpenAi)` directly.
- Polls `g_ui.assistantPrefsTestInFlight` until false (bounded `Yield×240`).
- Asserts `g_ui.assistantPrefsTestResultType == 1 && g_ui.assistantPrefsTestResult == "Verified."`.
- `AiClientFactory::SetTestOverride(nullptr)` cleanup in scope-exit.

**V3 — `VerifyOnSave_CancelOnClose_ShortCircuits`:**
- TU-local `StubAiClient` (gated): blocks `SendStreaming` on a TU-local `std::atomic<bool> release_`; releases when test sets `release_ = true`.
- Test triggers probe → reads `g_ui.assistantPrefsTestCancel` shared_ptr → flips cancel atom → releases stub → polls until `assistantPrefsTestInFlight == false` → asserts `assistantPrefsTestResult.empty()` (dispatcher short-circuited at the cancel guard, lines 651-652 of the soon-to-be-extracted body).

`AppController` access: TU instantiates the global `g_app` per `g_ui` pattern. If `g_app` instantiation has prerequisites (config load, etc.), V3 may need a tighter test scaffolding — Phase 0 of implementation confirms.

### Part D — `IAiClient` stub TU

Stub class lives inline in `ai_prefs_autosave_flow.test.cpp` (no new file). One stub class with a mode enum: `Success`, `Gated`. Constructor accepts a pointer to a TU-local `std::atomic<bool>* release_` for the gated mode.

## Critical files

### New
- `Source_Core/include/AiPrefsTestConnection.h`
- `Source_Core/src/AiPrefsTestConnection.cpp` (body lifted verbatim from `SmatchetPreferencesUi.cpp:520-680`)
- `docs/plans/shipped/ai-client-test-override.md` (this file)

### Modified
- `Source_Core/include/AiClientFactory.h` — append `SetTestOverride` decl + typedef
- `Source_Core/src/AiClientFactory.cpp` — implement override slot + early-out in `MakeAiClient`
- `tests/ui/ai_prefs_autosave_flow.test.cpp` — lift V2 + V3 from placeholders to live coverage; introduce TU-local `StubAiClient`
- `docs/backlog/agent-self-improvement/infra.md` — mark P2 mock-seam entry as **partially applied** (seam shipped + V2/V3 live; UI rewire follow-up flagged)
- `docs/backlog/agent-self-improvement/applied.md` — append archived entry for the seam

### Write set (canonical for lock-claim)

```
Source_Core/include/AiClientFactory.h
Source_Core/src/AiClientFactory.cpp
Source_Core/include/AiPrefsTestConnection.h
Source_Core/src/AiPrefsTestConnection.cpp
tests/ui/ai_prefs_autosave_flow.test.cpp
docs/backlog/agent-self-improvement/infra.md
docs/backlog/agent-self-improvement/applied.md
docs/plans/shipped/ai-client-test-override.md
```

**Explicitly NOT in write set:** `Source_Core/src/SmatchetPreferencesUi.cpp` (held by `whisper-dictation-phase-f`). Follow-up PR after #219 merges adds the inline-lambda → free-fn rewire there.

## Existing utilities reused

- [`Source_Core/src/SmatchetPreferencesUi.cpp:520-680`](../../Source_Core/src/SmatchetPreferencesUi.cpp) — `runProbe` body source for the extraction (verbatim copy with closure-captures rewritten as function parameters).
- [`Source_Core/include/IAiClient.h`](../../Source_Core/include/IAiClient.h) — interface for the stub class.
- [`Source_Core/include/MainThreadDispatcher.h`](../../Source_Core/include/MainThreadDispatcher.h) — used inside the extracted fn unchanged.
- [`tests/ui/views_columns_reorder.test.cpp`](../../tests/ui/views_columns_reorder.test.cpp) — UserData + GuiFunc/TestFunc skeleton; this slice mirrors the same shape.
- [`tests/ui/callstack_tooltip_hover.test.cpp`](../../tests/ui/callstack_tooltip_hover.test.cpp) — atomic flag observation pattern (for V3's gated stub release).

## Verification

```bash
cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone
bash scripts/dev/test-ui-ai-prefs-autosave-flow.sh    # exit 0, Passed=3 Failed=0 — V2 + V3 now live
bash scripts/dev/test-all.sh                          # global gate
cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12   # dual-target tripwire
```

If the existing pre-merge spawn-runner flake reproduces (filed P2 infra), rerun the single TU runner on a fresh port — same workaround as PR #214.

## Risks

- **R1 — `AppController` dependency in extracted fn.** `runProbe` captures `&app` for `app.mainThreadDispatcher`. The extracted fn takes `AppController&` by reference. TU#3 V2/V3 must have a live `AppController` instance. Mitigation: use the existing `g_app` global the rest of the UI uses; Phase 0 verifies it's usable from inside an ImGui Test Engine TestFunc.
- **R2 — Stub V3 timing.** Cancel-on-close path requires the cancel atom to be set BEFORE the dispatcher fires its callback. Gated stub holds the worker thread until release, giving the test a deterministic window. If gating proves fragile (e.g. the dispatcher post happens before stub even gets called), fall back to direct state manipulation: bypass the probe entirely + assert the short-circuit lambda behaviour against synthetic state.
- **R3 — Drift between extracted fn + the to-be-rewired inline lambda.** Window <24h until follow-up PR rewires. Mitigation: top-of-file comment in both `SmatchetPreferencesUi.cpp` (NOT TOUCHED but receives a note in the follow-up) and `AiPrefsTestConnection.cpp` warning "DRIFT — these two MUST stay in lock-step until PR #N rewires the UI side."

## Out of scope

- Wiring the UI side (`SmatchetPreferencesUi.cpp`) to call `AiPrefsTestConnection::TriggerProbe` — held by `whisper-dictation-phase-f`. Follow-up PR after #219.
- Refactoring the `runProbe` body further (e.g. splitting URL sanitization + provider-config picker into separate helpers) — beyond seam scope.
- Adding async test-mode short-circuits to `MainThreadDispatcher`.

## Plan revision contract

After PR merges, append to this file:
- `## Implementation log` — per-commit summary.
- `## Deviations from plan` — what changed.
- `## Verification` — actual runner outputs.
- `## Follow-up PR tracking` — link to the inline-lambda → free-fn rewire PR once it lands.

## Implementation log

- `9193d7c` · wip(plan): ai-client-test-override (this file)
- `6f79c54` · feat(ai): test seam in AiClientFactory + extract runProbe into AiPrefsTestConnection — seam + extraction + V2/V3 lift + UiTestScenario AppController accessor
- `fa0089b` · fix(ai-prefs-test): set showPreferences=true before TriggerProbe — V2/V3 were racing the close-handler clearing state every frame

## Deviations from plan

- **showPreferences=true gotcha.** Plan didn't anticipate that `SmatchetDrawPreferencesPanel`'s close-handler (`SmatchetPreferencesUi.cpp:197-206`) clears `assistantPrefsTest{InFlight,Result,ResultType}` every frame when `showPreferences=false`. V2/V3 originally raced this: probe set Result="Verified.", next frame close-handler cleared it before the assertion ran. Fix in `fa0089b` — `ResetPrefsTestState()` now sets `g_ui.showPreferences = true`; V2 and V3 reset to false at end-of-test.
- **No doctest unit for the seam.** Plan mentioned a possible `tests/Source_Core/AiClientFactoryOverride.test.cpp`. Skipped — bucket-E V2/V3 lift provides equivalent coverage and the seam is one branch in `MakeAiClient`.

## Verification

- Build: `cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone` — PASS.
- TU#3 V1 Autosave_DebouncesThenSaves: PASS (deterministic).
- TU#3 V2 VerifyOnSave_TestConnection_SetsResult: PASS (individual fresh-port runs; mass runs subject to the pre-existing bucket-E spawn-runner flake, P2 infra entry).
- TU#3 V3 VerifyOnSave_CancelOnClose_ShortCircuits: PASS (individual fresh-port runs; same flake).
- `bash scripts/dev/test-ui-ai-prefs-autosave-flow.sh`: PASS on 3rd retry (1st + 2nd attempts hit the spawn flake; 3rd: `Passed: 3 Failed: 0`).

## Follow-up PR tracking

After `whisper-dictation-phase-f` (PR #219) merges:
- 5-line rewire: replace `runProbe` inline lambda in `SmatchetPreferencesUi.cpp:528-688` with `AiPrefsTestConnection::TriggerProbe(d, app, selectedKind)` call. Remove the lambda + add `#include "AiPrefsTestConnection.h"`. Drift-warning comment in `AiPrefsTestConnection.h` updated to "WAS-IN-LOCK-STEP-WITH inline lambda; now canonical".
