# Archived: AiAssistantStreaming401Scenario

**Archived:** 2026-05-26  
**Original file:** `Source_Core/src/Commands/Scenarios/AiAssistantStreaming401Scenario.cpp`  
**Registry key:** `ai-assistant-streaming-401`

## What the scenario did

Drove the AI assistant streaming path with a `Stub401Client` that fired `onError`
immediately with `HttpStatus = 401` and no deltas. It verified the UI state
transition Idle → InFlight → Errored and asserted zero deltas received, one
error received with HTTP 401.

The scenario was introduced in slice 8 of the `autonomous-debugging-no-creds`
plan (`docs/design/archive/autonomous-debugging-no-creds.md` § Slice 8) as a
missing-bug-path scenario.

## Why archived

**Orphan** — the scenario meets all three orphan criteria:

1. No PR citation in the curated perf set (`scripts/dev/perf-run.sh` scenario list).
2. No references in any test file beyond the mechanical stub in
   `tests/Source_Core/SmatchetScenarioRegistry.stubs.cpp` (which existed only
   to satisfy the linker, not to exercise the scenario logic).
3. Not referenced from any current backlog item or plan-doc that would trigger
   regression runs.

The equivalent error-path coverage is provided by `StubAiClient`-based unit
tests in `tests/` which are faster and do not require the full scenario
scaffolding.
