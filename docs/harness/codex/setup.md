# Codex / OpenAI Agents Adapter

Codex reads the project agent contract natively:

- [`AGENTS.md`](../../../AGENTS.md) at the repo root - project rules, delegation table, and harness adapter pointers.
- [`agents/core/*.md`](../../../agents/core/) + [`agents/project/*.md`](../../../agents/project/) - canonical agent definitions.
- Nearest nested `AGENTS.md` files - subsystem leaf rules, loaded by Codex per the agents.md spec.

Codex also supports project-local lifecycle hooks and custom subagents under
`.codex/`. The project keeps those files gitignored and regenerates them from
tracked templates so Claude Code, Cursor, pi, and Codex continue to share the
same canonical `agents/` prompts.

## Verify After Clone

Run:

```bash
bash agents/scripts/core/setup-harness.sh codex
```

The setup command:

1. Runs the standard required-tool probe.
2. Copies `docs/harness/codex/config.toml.tmpl` to `.codex/config.toml`.
3. Copies `docs/harness/codex/hooks.json.tmpl` to `.codex/hooks.json`.
4. Generates `.codex/agents/*.toml` from `agents/{core,project}/*.md`.
5. Verifies `AGENTS.md`, canonical agent count, generated Codex agent count, duplicate basenames, and nested subsystem `AGENTS.md` coverage.
6. Installs the tracked git hooks by setting `core.hooksPath=scripts/git-hooks`, but only when that local git config is unset, still points at the default `.git/hooks`, or already points there.
7. Prints the remaining payload-dependent Claude-Code-only runtime gaps.

The setup script preserves user-modified `.codex/config.toml` and
`.codex/hooks.json` on re-run. Regenerated `.codex/agents/*.toml` files are
adapter output; edit the canonical markdown prompts instead.

## Trust Step

Project-local Codex hooks load only after the local `.codex/` layer is trusted
by the Codex runtime. After setup, review `.codex/hooks.json` and trust/enable
project hooks through the Codex hook UI or `/hooks` flow for your Codex surface.

## Repo-Owned Parity

These parts are covered after setup:

- Agent/rule discovery: native `AGENTS.md` + `agents/{core,project}/*.md`.
- Project specialist agents: generated `.codex/agents/*.toml` custom agents.
- Subsystem leaf rules: native nearest-`AGENTS.md` behavior.
- SessionStart nudges: memory drain, owed postmortems, due follow-ups, and shipped-active-plan archival checks.
- Stop hooks: committed-diff pre-ship gate and main-repo cleanliness warning.
- Git-level checks: tracked hooks in `scripts/git-hooks/`.
- Pillar 2 staged-file scan: `scripts/git-hooks/pre-commit` runs `scripts/dev/pillar2-scan.sh` for staged first-party C++ files.
- Merged-PR push guard: `scripts/git-hooks/pre-push` blocks accidental pushes to merged/closed PR branches when `gh` can resolve the PR.

## Remaining Runtime Gaps

These Claude Code features are still payload-dependent and intentionally not
wired in Codex yet:

- `PreToolUse` gates such as the HEAD-drift edit guard.
- `PostToolUse` edit lint drains and Bash PR autoregistration.
- `SubagentStop` token telemetry.

Use [`hooks-equivalent.md`](hooks-equivalent.md) for the current coverage matrix
and manual commands.

## Token Telemetry

If Codex's `SubagentStop` event payload is validated for the project logger, wire
it to the harness-agnostic logger documented in
[`agents/_shared/token-tracking/README.md`](../../../agents/_shared/token-tracking/README.md).
Until then, token telemetry is not automatic under Codex.

## Removing

To remove the generated Codex adapter files:

```bash
rm -rf .codex/config.toml .codex/hooks.json .codex/agents
```

To opt out of the git hooks in a specific clone, restore or unset the local hook
path:

```bash
git config --local --unset core.hooksPath
```
