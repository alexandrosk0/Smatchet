# Plan - Codex native parity
<!-- plan-date: 2026-06-08 -->

> **Slug**: `codex-native-parity`
>
> **Status**: `shipped`

## Context

Codex now exposes project-local hooks and custom subagents under `.codex/`, while Smatchet's Codex harness setup still reports those surfaces as unavailable. After this lands, `setup-harness codex` generates a gitignored Codex-native adapter for safe hooks and project specialist agents without changing Claude Code's existing `.claude/` templates or behavior.

## Approach

Add tracked Codex templates under `docs/harness/codex/` and make both Bash and PowerShell setup copy them into `.codex/`. Generate `.codex/agents/*.toml` deterministically from `agents/{core,project}/*.md`, preserving canonical prompts while emitting only Codex-supported fields. Keep payload-sensitive Claude hooks out of Codex until their event payloads are verified; wire only repo-root command hooks that are safe without Claude-specific JSON.

## Files to modify

1. `agents/scripts/core/setup-harness.sh`: generate Codex config/hooks/agents and update parity output.
2. `agents/scripts/core/setup-harness.ps1`: keep Windows setup behavior aligned with Bash setup.
3. `agents/scripts/core/gen-codex-agents.py`: new deterministic generator from canonical agent prompts to Codex TOML.
4. `agents/scripts/core/test-setup-harness.sh`: assert Codex setup outputs native files and does not clobber Claude Code setup.
5. `docs/harness/codex/*`: add Codex templates and update setup/runtime gap documentation.
6. `docs/harness/SETUP.md` and `docs/harness/capability-adapter.md`: update cross-harness setup notes.

## Existing utilities reused

- `copy_template` / `Copy-Template` in setup scripts: preserve user-modified local adapter files.
- `install_git_hooks` / `Install-GitHooks`: keep repo-owned git hooks shared across harnesses.
- `docs/harness/claude-code/hooks/pre-ship-stop-gate.sh`: reused through a Codex wrapper so the same pre-ship logic runs without copying Claude runtime config.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no product UI impact; setup/docs/scripts only.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no product UI impact.
- **Pillar 3 (never crash)**: generated hooks are conservative command hooks and keep payload-sensitive checks manual.
- **Pillar 4 (accessibility - keyboard nav / font scaling / WCAG AA)**: no UI surface changes.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A - <reason>`)

N/A - the diff does not touch `Source/Core/` or runtime UI code.

## Risks / non-goals

- Risk: Codex hook payload compatibility differs from Claude Code. Mitigation: wire only root-resolved command hooks that do not parse event payloads.
- Risk: generated Codex agents drift from canonical agent prompts. Mitigation: setup regenerates from `agents/{core,project}/*.md` and tests count generated files.
- Non-goal: full port of Claude Code's payload-dependent `PreToolUse` / `PostToolUse` hooks.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `python -m py_compile agents/scripts/core/gen-codex-agents.py`.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A - no UI code.
- **Bash-driver scenario / screenshot / sanitizer**: `bash agents/scripts/core/test-setup-harness.sh`.
- **Build gate**: N/A - no C++ build graph change.
- **Doc validation (blocks plan-doc PRs - keep this bullet)**: `bash scripts/dev/test-docs.sh`.
- **Plan stress-test - `grill-with-docs` (keep this bullet)**: passed inline. The only material boundary is Codex-native hooks/agents vs Claude-payload-compatible hooks; the plan keeps payload-dependent blockers out of Codex until event payloads are verified. No glossary or ADR update needed.
- **Manual residue**: none planned; payload-sensitive Codex hook parity is explicitly out of scope and documented.

## Out of scope (flagged, not designed)

- Payload-compatible Codex ports for HEAD-drift edit blocking, deferred C++ edit lint, PR autoregistration, and subagent token telemetry.

## Implementation log

- `36935e00` - generated the Codex-native harness layer and merged PR #1025.

## Deviations from plan

- PowerShell setup also reused the new Python resolver for pi setup, preserving behavior while avoiding Windows Store alias failures.
- CodeRabbit produced status-only evidence after a rate-limit warning; merge used the repo merge-gates grace-window path after a manual `@coderabbitai review` completed with zero actionable findings.

## Verification (actual)

- `C:\Program Files\Git\bin\bash.exe -lc 'bash agents/scripts/core/test-setup-harness.sh'` - passed.
- `C:\Program Files\Git\bin\bash.exe -lc 'bash scripts/dev/test-docs.sh'` - passed.
- `C:\Program Files\Git\bin\bash.exe -lc 'bash agents/scripts/project/test-lint-rules.sh --diff origin/develop'` - passed.
- `powershell -NoProfile -ExecutionPolicy Bypass -File agents\scripts\core\setup-harness.ps1 codex` - passed.
- `python -m py_compile agents/scripts/core/gen-codex-agents.py` - passed.
- `python -m json.tool docs/harness/codex/hooks.json.tmpl` - passed.
