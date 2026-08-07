# 01 · --spawn honors --timeout — Post-implementation review (pass 1)

Problems only. Reviewed commit `bc3b1cde` (fix(cli): --spawn honors --timeout via shared
ScenarioWaitMs helper) against GitHub Issue #1943 — this item is a trivial bug fix
(work-items.md § The loop) with no `1-specification.md` / `2-design.md` / `3-plan.md`; the issue is
the intent artifact. No praise, no restatement.

---

## 1. Issue's suggested regression test wasn't added — only the pure-formula unit test was

Issue #1943's "Suggested fix" asks for two things: mirror the in-process expression at
`CliDispatch.cpp:476` (done — `CliDispatch.cpp:476` now calls `ScenarioWaitMs(pa.timeoutMs, frames)`),
**and** "Add a bats case asserting a spawned scenario with a deliberately small `--timeout` fails
fast rather than running to the frames-derived deadline — otherwise the two paths can diverge again
silently." The diff adds two doctest `TEST_CASE`s for the new `ScenarioWaitMs` pure helper
(`tests/Core/CliArgCoercion.test.cpp:153-170`) but no integration-level test drives the real
`--spawn` path end-to-end. The repo has an established pattern for exactly this kind of coverage —
`scripts/dev/test-spawn-mcp-default-auth.sh`, `scripts/dev/test-ui-spawn-warmup-deterministic-gate.sh`
(both launch a real spawned instance and assert on its observed behavior) — so this isn't a
missing-convention gap, it's an unused one. The unit test proves `ScenarioWaitMs` computes the right
number in isolation; it does not prove the `--spawn` driver actually wires `pa.timeoutMs` into the
real wait loop that gates `WaitForFile` / `SpawnAndRunHandleAsync` (`CliDispatch.cpp:498`) — the exact
class of silent breakage the issue is about, and the one CI already hit once (bucket-E, PR #1937).

- **Disposition:** defer (→ Spawned.md § Backlog) — the shared-helper extraction structurally lowers
  the divergence risk the bats case would guard against (both drivers now call one function, so a
  future edit to either call site can't silently drop the check), so this isn't blocking. But the
  issue explicitly asked for it, the tooling pattern already exists in-repo, and closing this item
  without capturing the ask would let it silently drop — record it as a backlog follow-up rather than
  letting it disappear.

---

## Verdict

```json
{"criteria": {"task_satisfaction": 0.9, "correctness": 0.95, "evidence_quality": 0.85,
  "regression_risk": 0.8, "security": 0.95, "project_invariants": 0.9,
  "scope_discipline": 0.85, "verification_completeness": 0.6},
 "confidence": 0.75, "hard_veto": false, "veto_reason": ""}
```
