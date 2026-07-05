# Plan — Coverage-gap Tier 3: registry-wide command error-envelope contract sweep

> **Slug**: `coverage-gap-tier3-command-contract`
>
> **Status**: `active`

## Context

[`TEST_COVERAGE_GAP_MAP.md`](../../../TEST_COVERAGE_GAP_MAP.md) Tier 3: the 24 `Commands/` strict-zone TUs (4.6K LOC of `BuiltinCommands_*` registration + handlers feeding CLI, palette, MCP, and Lua) are compiled into no test target. Their handlers capture `AppController&` at registration, so the doctest rig cannot host them — the architecturally-correct harness runs inside the app. After this lands, every registered command's ParamSpec and the Dispatch guard contract (missing-required-arg / validation-error / confirm-required / unknown-command) are exercised end-to-end by a mutation-free scenario, wired into the auto-enrolled `test-all.sh` sweep.

## Approach

A new `command-contract-sweep` scenario iterates `CommandRegistry::All()` and dispatches each command through the REAL guard path with inputs guaranteed to be rejected before the handler runs (Dispatch guard order: resolve → unknown-command → validation → confirm gate → dry-run → invoke). Probes: (A) omit a required-without-default param → `missing-required-arg`; (B) a JSON array for a scalar param (arrays never coerce) with required params synthesized valid → `validation-error`; (C) destructive commands with fully-synthesized valid args from an unconfirmed automation source (`Mcp`) → `confirm-required` — an `Ok` here means a destructive handler executed unconfirmed, the exact security-audit failure mode the gate exists to prevent; (D) an impossible name → `unknown-command`. Zero-param non-destructive commands are counted `skippedNoProbe` (any dispatch would run their handler), and probe-C synth misses are `inconclusive` — both reported, never silently "covered". The driver script asserts the scenario report's `ok == true` via `scenario.run --spawn`.

## Files to modify

1. `Source/Core/src/Commands/Scenarios/CommandContractSweepScenario.cpp` — NEW scenario (probes above; runs on frame 0; OnFinish returns `{commandsTotal, checked, skippedNoProbe, inconclusive, violations, ok}`).
2. `Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp` — extern + `RegisterFactory("command-contract-sweep", ...)`.
3. `tests/Core/SmatchetScenarioRegistry.stubs.cpp` + `tests/Core/SmatchetScenarioRegistry.test.cpp` — linker stub + snapshot-name pin (the existing lock-step contract).
4. `scripts/dev/test-command-contract.sh` — NEW auto-enrolled driver (skip-if-no-exe; spawns `scenario.run --name=command-contract-sweep --yes --spawn`, asserts `data.ok == true`; stderr kept out of the JSON envelope per the test-whisper-roundtrip.sh pitfall).

## Existing utilities reused

- `ScenarioRunner` + `SmatchetScenarioRegistry` registration/stub/snapshot contract (`tests/Core/SmatchetScenarioRegistry.test.cpp`).
- `CommandRegistry::Dispatch` guard order + `ErrorCodeString` (`Source/Core/src/Commands/CommandRegistry.cpp:283`).
- `scripts/dev/test-all.sh` auto-enrolment glob + the `--spawn` CLI envelope parse pattern from `scripts/dev/test-whisper-roundtrip.sh`.

## Extraction sizing

N/A — additive scenario + driver; nothing is split.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — the sweep runs only when explicitly invoked via `scenario.run`; probes are pre-handler rejections (string compares + map lookups).
- **Pillar 2 (UI never freezes)**: the sweep runs on frame 0 of the scenario on the UI thread by scenario contract; ~60 commands × ≤3 rejected dispatches is well under a frame budget concern for a test-only path (not a steady-state surface).
- **Pillar 3 (never crash)**: strengthened — probe C would catch a destructive handler executing without confirmation; Dispatch's try/catch already contains handler throws.
- **Pillar 4 (accessibility)**: no impact.

## Perf-review-system gates

1. **PR-fast CI**: N/A — no steady-state hot path changes; the scenario is opt-in test infrastructure.
2. **Pillar 2 static scanner**: no new sync-I/O reachable from `ImGui::*`.
3. **Dispatcher drain**: untouched.
4. **Visible-cue bucket-E harness**: no new stall path.
5. **Marker inventory**: no new markers.

**Pre-push local check**: N/A (gate 1).

## Risks / non-goals

- **Risk**: a command whose spec defeats the arg synthesizer makes probe C inconclusive. Mitigation: inconclusive counts are surfaced in the report; a nonzero count is visible in the driver log for follow-up, and the strict violation set stays deterministic.
- **Risk**: probe B's array value unexpectedly coercing for some future ParamType would run a handler. Mitigation: coercion table read and pinned (arrays fail String/Int/Number/Bool; Json params are excluded from probe B).
- **Non-goal**: valid-args happy-path invocation per command (needs per-command fixtures; tracked as a follow-up once the fixture-backend command subset is enumerated).
- **Non-goal**: adding the driver to a blocking CI lane — it auto-enrols in the `test-all.sh` sweep (bats aggregate) and self-skips without an exe; lane promotion is a separate decision.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `SmatchetScenarioRegistry.test.cpp` snapshot pins the new name (run locally: 42 assertions green).
- **Bucket E (ImGui Test Engine)**: N/A — the scenario is itself the in-app harness; `scripts/dev/test-command-contract.sh` is its driver.
- **Bash-driver scenario / screenshot / sanitizer**: the new driver, auto-enrolled by `test-all.sh` (self-skips when the exe is absent, runs wherever a built exe exists — dev boxes + any lane that builds the standalone).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — CI; new TU syntax-checked locally under clang with full warnings.
- **Doc validation**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: guard-order and coercion-table claims verified directly against `CommandRegistry.cpp` before design.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: none — this plan defers nothing that existing docs reference.

- Happy-path per-command invocation against the fixture backend (follow-up; needs a curated read-only command list).
- Gap-map Tier 2 (backend HTTP shells — owned by the B2 migration track) and Tier 5 (bucket-C/E re-blocking — a merge-gates policy change for the user to authorize).

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
Flip § Status to `shipped`, populate the three sections above, `git mv` to `docs/plans/shipped/` in the same PR.
