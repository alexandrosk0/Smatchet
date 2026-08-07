# 01 · --spawn honors --timeout — Post-implementation review (pass 1)

Problems only. Reviewed commit `bc3b1cde` (fix(cli): --spawn honors --timeout via shared
ScenarioWaitMs helper) against the item's intent set. This item took the **trivial bug-fix** lane of
[work-items.md → The loop](../../../agent-rules/work-items.md#the-loop) (confirmed issue #1943), so
`1-specification.md` / `2-design.md` / `3-plan.md` legitimately do not exist — intent was taken from
the commit body, issue #1943 as quoted in `.github/workflows/build-and-test.yml`, and the shipped
`--timeout` contract in `docs/guides/cli.md`. Read-only pass; no builds, no test runs. No praise, no
restatement.

**Spawned sweep:** `Spawned.md` records none in all four sections. Nothing in the diff shows a
deferral/idea/backlog/bug choice made during the work that failed to land there, so the sweep is
clean as of this commit — findings 2 and 3 below are new deferral candidates for the addresser to
enter.

---

## 1. The bucket-E CI comment now states the opposite of the shipped behaviour and steers the next maintainer to the wrong knob

`.github/workflows/build-and-test.yml:1249-1258` documents the `--frames=5400` workaround with:

> `--spawn` derives its whole wall-clock budget from this one number: `CliDispatch.cpp:476` computes
> the deadline as `((frames/60)+30)*1000` ms … `--timeout` is NOT the knob here despite the timeout
> error's hint: the `--spawn` path drops `pa.timeoutMs` (only the in-process path at
> `CliCommandRunner.cpp:122` honors it). Filed separately.

Every load-bearing claim in that paragraph is false as of this commit. `CliDispatch.cpp:476` is now
`ScenarioWaitMs(pa.timeoutMs, frames)`; `--timeout` **is** the knob; and "filed separately" points at
the issue this commit closes. The workflow was not touched by the diff, so the comment shipped stale.

This is not cosmetic. The comment is the sole rationale for `--frames=5400`, and it instructs the
next maintainer facing a bucket-E deadline trip to keep inflating the frame count. Frames is not a
budget dial — it is the scenario's own frame allowance, so inflating it lengthens the run being
measured to buy wall-clock, whereas `--timeout=<ms>` now buys the wall-clock directly and leaves the
suite's frame budget where the author set it. The stale note actively preserves a workaround the fix
retired.

- **Disposition:** fix — rewrite the comment to state that the budget is
  `ScenarioWaitMs(pa.timeoutMs, frames)` and that `--timeout` overrides it, and decide there whether
  bucket-E keeps `--frames=5400` or moves to `--frames=3600 --timeout=120000`. Dropping the "filed
  separately" sentence is mandatory either way — it now points at a closed defect.

---

## 2. `SMATCHET_SPAWN_TIMEOUT_MS` is documented as `--timeout`'s default in three places and honoured in none of the budget paths

`CliArgCoercion.h:47-52` introduces the helper as the definitive rule — "an explicit `--timeout`
wins; otherwise the budget is derived from the frame count". The shipped contract says something
else:

- `Source/Standalone/CliHelpAndAttach.cpp:154` (`cmd --help`): `--timeout=<ms>   Cap async wait;
  0=no cap (default: SMATCHET_SPAWN_TIMEOUT_MS or 0).`
- `docs/guides/cli.md:87`: "Default: `SMATCHET_SPAWN_TIMEOUT_MS` or no cap."
- `docs/guides/cli.md:460`: "`SMATCHET_SPAWN_TIMEOUT_MS` | CLI `--timeout` default | `0` = no cap".

`ParseArgs` (`CliArgs.cpp:287-299`) never consults the env var; `pa.timeoutMs` stays `0`. The only
reader is `CliHelpAndAttach.cpp:304-307`, which uses it for the attach path's **HTTP read timeout**
— not for the wait budget and not as `__timeout_ms`. So an operator who sets
`SMATCHET_SPAWN_TIMEOUT_MS=120000` and runs a 5400-frame `--spawn` scenario still gets the
frames-derived 120 s by coincidence, and at 9000 frames silently gets 180 s instead of the 120 s cap
they configured — the app even reports the var as observed via `config.path`
(`BuiltinCommands_Helpers.cpp:103`).

The gap predates this commit, but this commit is what makes the helper the single authority on the
budget, and the header it ships asserts a rule that contradicts the shipped `--help` text. Leaving
both live is the same defect class the commit set out to close: a flag hint that advertises
behaviour the code does not implement.

- **Disposition:** defer (→ `Spawned.md`, Bugs) — either default `out.timeoutMs` from
  `EnvIntOr("SMATCHET_SPAWN_TIMEOUT_MS", 0)` in `ParseArgs` (one line, and every consumer of
  `pa.timeoutMs` inherits it, including `__timeout_ms`), or delete the claim from `--help` and both
  `cli.md` rows. Do not leave the doc and the header asserting different rules.

---

## 3. Nothing pins the call-site wiring the commit says "cannot diverge again"

The commit body claims the extraction means "the two paths cannot diverge again" and that the
doctests are the guard. The two `TEST_CASE`s in `tests/Core/CliArgCoercion.test.cpp:153-170` exercise
`ScenarioWaitMs` as a pure function only. The bug that shipped as #1943 was never in the arithmetic —
it was at the call site, `CliDispatch.cpp:476`, which passed no timeout at all. Re-inlining that
expression, or regressing the argument to `ScenarioWaitMs(0, frames)`, leaves both new tests green.

A unit-level pin is not reachable today: `tests/CMakeLists.txt:1328-1329` compiles exactly two
Standalone TUs into `SmatchetTests` (`CliArgCoercion.cpp`, `CliExitCodes.cpp`), and `SpawnAndRun`
needs a live child process regardless. So the shared helper is the strongest guard currently
available and the residual risk is genuinely small — but the commit message overstates what the tests
cover, and the only surface that could observe the wiring (a `--spawn --timeout=<small>` run
asserting exit 8 well before the frames-derived deadline) does not exist.

- **Disposition:** defer (→ `Spawned.md`, Deferred) — add a CLI-level `--spawn --timeout` assertion
  to the launch-smoke lane; trigger is the next time bucket-E or the spawn smoke is touched. If the
  addresser declines, the commit body's "cannot diverge again" should be softened to "share one
  helper", so the next reader does not trust a guard that isn't there.

---

## Verdict

```json
{"criteria": {"task_satisfaction": 0.9, "correctness": 0.9, "evidence_quality": 0.7,
  "regression_risk": 0.75, "security": 0.95, "project_invariants": 0.9,
  "scope_discipline": 0.95, "verification_completeness": 0.6},
 "confidence": 0.75, "hard_veto": false, "veto_reason": ""}
```
