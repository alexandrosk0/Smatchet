# A bare `! cmd` or a `[ … ] && [ … ]` chain in the middle of a bats test cannot fail it

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-06
- **Observed on**: the historical-review Batch 26 PR and agent-layer PR alexandrosk0/the-unwilling-agentic-bunch#5. The code-review agent found three inert assertions; bats 1.13 confirms a mid-test `!` that should fail passes. All three are fixed in those PRs:
  - `tests/bats/cr_finding_gate.bats` had two `! printf … | grep` rejection checks;
  - the layer's `safe_admin_merge.bats` had `[ "$status" -eq 0 ] && [ -z "$output" ]`.
- **Status**: open

## What happened

bats runs each test under `set -e`, but bash does not exit on a negated pipeline, or on a failure anywhere in an `&&` / `||` list except its last command. So:

- `! printf … | grep -q …` in the middle of a test never fails it. It only counts as the test's last command.
- In `[ "$status" -eq 0 ] && [ -z "$output" ]`, only the right-hand check is enforced.

Both look like assertions in review, so a test can pass while checking less than it claims. Two of the three cases here were the rejection half of a predicate test, which is the half that guards against a fail-open.

## Concrete next action

Add a rule to the shell-lint / bats lint lane, delta-gated on changed `.bats` files, that flags:

- a line inside a `@test` body that starts with `!` (after indentation) and is not the body's last command, unless it ends in `|| false` or is written `run ! …`;
- a `[ … ] && [ … ]` or `[[ … ]] && [[ … ]]` chain as a statement, as opposed to an `if` condition.

The suggested fix in the message is `run ! …` (bats ≥ 1.5, with `bats_require_minimum_version 1.5.0`), `|| false`, or one check per line.

Then sweep the existing suites in both repos with the rule in report-only mode, and fix or grandfather each hit.

Bats coverage for the rule: a fixture `.bats` with each inert shape is flagged, and the `|| false`, `run !` and one-per-line forms are not.
