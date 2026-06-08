# Codex / OpenAI Agents Adapter

Codex reads the project agent contract natively:

- [`AGENTS.md`](../../../AGENTS.md) at the repo root - project rules, delegation table, and harness adapter pointers.
- [`agents/core/*.md`](../../../agents/core/) + [`agents/project/*.md`](../../../agents/project/) - individual agent definitions.
- Nearest nested `AGENTS.md` files - subsystem leaf rules, loaded by Codex per the agents.md spec.

No `.codex/` adapter mirror is required or generated. A local `.codex/worktrees/`
directory may exist because Codex uses it for isolated worktrees, but it is not
part of harness setup.

## Verify After Clone

Run:

```bash
bash agents/scripts/core/setup-harness.sh codex
```

Or on Windows PowerShell:

```powershell
pwsh agents/scripts/core/setup-harness.ps1 codex
```

The setup command:

1. Runs the standard required-tool probe.
2. Verifies `AGENTS.md` exists.
3. Counts `agents/{core,project}/*.md` and warns about duplicate basenames.
4. Reports nested subsystem `AGENTS.md` coverage.
5. Installs the tracked git hooks by setting `core.hooksPath=scripts/git-hooks`, but only when that local git config is unset, still points at the default `.git/hooks`, or already points there.
6. Prints the remaining Claude-Code-only runtime gaps.

It does not copy `.claude/` templates, create `.codex/`, or pretend Codex has
Claude Code hook events.

## Repo-Owned Parity

These parts are covered after setup:

- Agent/rule discovery: native `AGENTS.md` + `agents/{core,project}/*.md`.
- Subsystem leaf rules: native nearest-`AGENTS.md` behavior.
- Git-level checks: tracked hooks in `scripts/git-hooks/`.
- Pillar 2 staged-file scan: `scripts/git-hooks/pre-commit` runs `scripts/dev/pillar2-scan.sh` for staged first-party C++ files.
- Merged-PR push guard: `scripts/git-hooks/pre-push` blocks accidental pushes to merged/closed PR branches when `gh` can resolve the PR.

## Runtime Gaps

These Claude Code features are runtime-owned and cannot be fully installed by
this repository for Codex today:

- `SessionStart` nudges and session scratchpad resets.
- `PreToolUse` gates such as the vexp raw-search guard and HEAD-drift edit guard.
- `PostToolUse` edit lint drains and Bash PR autoregistration.
- `Stop` hooks such as deferred lint drain, pre-ship stop gate, and heartbeat.
- `SubagentStop` token telemetry.
- First-class named project specialist agent types inside Codex.

Use [`hooks-equivalent.md`](hooks-equivalent.md) for the repo-owned equivalents
and manual commands.

## Token Telemetry

If a future Codex runtime exposes a subagent-stop-style hook, wire it to the
harness-agnostic logger documented in
[`agents/_shared/token-tracking/README.md`](../../../agents/_shared/token-tracking/README.md).
Until then, token telemetry is not automatic under Codex.

## Removing

There is no `.codex/` adapter output to remove. To opt out of the git hooks in a
specific clone, restore or unset the local hook path:

```bash
git config --local --unset core.hooksPath
```
