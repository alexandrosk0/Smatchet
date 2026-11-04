# Most hook `timeout` values in the Claude Code settings template are milliseconds, but Claude Code reads seconds

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: the layer's `guard-plan-lock.sh` timeout fix in `agent-layer/docs/harness/claude-code/settings.json.tmpl`, where its `3000` became `30`.
- **Status**: open

## What happened

Claude Code reads a command hook's `"timeout"` in seconds. Most hooks in `agent-layer/docs/harness/claude-code/settings.json.tmpl`, the template `setup-harness.sh claude-code` renders into `.claude/settings.json`, carry millisecond-sized values instead: 2000, 3000, 5000, 10000, 15000, 30000, 60000 and 120000. So the effective limits are 1000 times what each author meant:

- the `PostToolUse` `Edit|Write` `lint-cpp.sh` hook (`60000`) can hold one tool call for about 16.7 hours;
- `autoregister-pr.sh` and `lint-portable-purity.sh` (`30000`) can each hold one for about 8.3 hours;
- the `Stop` hook `lint-cpp-drain.sh` (`120000`) can hold the end of a turn for about 33 hours.

A hook that hangs (a stuck `git` or `gh` call, a network wait, a lock) therefore stalls the session instead of timing out. Only the `guard-plan-lock.sh` entry has been corrected to seconds so far.

## Concrete next action

In the layer, convert every remaining `"timeout"` in `docs/harness/claude-code/settings.json.tmpl` to seconds (each value / 1000: 3000 → 3, 10000 → 10, 60000 → 60, 120000 → 120, and so on). Then check each hook's real worst-case runtime against its new limit, since a millisecond-sized budget that was always far too long may hide a hook that needs more than value / 1000 seconds (for example `lint-cpp-drain.sh` over a large drain, or `pre-ship-stop-gate.sh`), and raise any that do, with the reason in the commit message. Pin the unit with a check: a layer test that fails on any hook `timeout` above a seconds-sized ceiling (for example 600), so a millisecond value cannot come back. Bump the host pin and re-run `setup-harness.sh claude-code` so `.claude/settings.json` picks the new values up.
