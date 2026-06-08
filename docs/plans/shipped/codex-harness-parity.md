# Plan - Codex harness parity

> **Slug**: `codex-harness-parity`
>
> **Status**: `shipped` - Codex setup is self-verifying for repo-owned parity surfaces and explicit about Claude Code runtime features that Codex cannot receive from this repository.
>
> **Usage**: this plan drives the Codex adapter hardening requested in-session on 2026-06-08.
>
> **Mandatory rules cross-link**: see `AGENTS.md` Project rules / Process rules / Harness adapter.

## Context

The user asked whether all Smatchet agents and Claude Code features are running under Codex. The audit found Codex can read `AGENTS.md` plus `agents/{core,project}/*.md`, but it does not receive Claude Code's generated `.claude/` hooks, skill links, token telemetry, or SessionStart/Stop automation. After that, the user asked to "fix them all" and push autonomously. After this lands, Codex setup should make every repo-owned parity step explicit, install the stable local enforcement surfaces the repo can own, and clearly report the remaining runtime gaps as non-repo-owned.

## Approach

Harden the Codex adapter instead of copying Claude Code templates into `.codex/`. Codex's stable contract remains `AGENTS.md` plus `agents/`, so the fix should improve `setup-harness codex` to verify agent definitions, install or verify repository git hooks, and emit a concise parity report that separates "repo-wired" from "Claude-Code-only".

Document the equivalent local workflows for hooks that Codex cannot receive directly: git hooks for pre-push/pre-commit enforcement, manual end-of-turn commands for interactive sessions, and token telemetry as opt-in only if a future Codex runtime exposes a subagent-stop hook. Keep the design conservative: no fake `.codex` mirror, no product-level claims, and no hook templates that the docs already say are unstable.

## Files to modify

1. `docs/harness/codex/setup.md`: document the strengthened Codex adapter behavior and the runtime gaps that remain outside repo control.
2. `docs/harness/codex/hooks-equivalent.md`: expand Codex equivalents for Claude Code hooks, including git hook coverage and manual checks.
3. `docs/harness/SETUP.md`: update the harness matrix so Codex no longer reads like a pure no-op when setup can verify/install repo-owned pieces.
4. `agents/scripts/core/setup-harness.sh`: improve `setup_codex` so it validates the canonical agent files and installs/verifies stable git-hook wiring without creating a `.codex/` adapter mirror.
5. `agents/scripts/core/setup-harness.ps1`: mirror the Codex setup behavior for Windows users.
6. `scripts/git-hooks/pre-commit`: add a stable Codex-friendly pre-commit hook if the repo does not already ship one, scoped to cheap local checks.
7. `scripts/git-hooks/pre-push`: refresh comments so the tracked hook is described as repo-owned, not Claude-only.
8. `scripts/dev/check-required-tools.sh`: make tool probing reliable from Codex/Git Bash/WSL when Windows-side tools are installed.
9. `agents/scripts/core/test-setup-harness.sh`: assert the strengthened Codex setup path.
10. `agents/scripts/core/test-markdown-links.sh`: reuse robust Python discovery so Git Bash does not trip over inert Windows Store aliases.
11. `agents/core/debug-detective.md`: repair the existing contract phrase expected by the agent-contract selftest.
12. `docs/harness/capability-adapter.md`: document Codex's native agent discovery and search fallback accurately.
13. `docs/harness/cursor/hooks-equivalent.md`: point Cursor at the same tracked repo hook path so hook docs stay consistent across non-Claude harnesses.
14. `AGENTS.md`: clarify that Codex uses native `AGENTS.md` discovery rather than a generated `.codex` mirror.
15. `agents/scripts/core/test-plan-index.sh`: normalize Git path arguments so Git Bash + Windows Python generates the same shipped-plan dates as Linux CI.
16. `docs/plans/codex-harness-parity.md`: track the plan and verification outcomes.

## Existing utilities reused

- `scripts/dev/check-required-tools.sh`: existing tool probe called by setup-harness before any adapter-specific setup.
- `scripts/git-hooks/pre-push`: existing repo-owned pre-push hook path installed via `core.hooksPath`.
- `scripts/dev/pillar2-scan.sh`: existing harness-agnostic Pillar 2 scanner for Codex manual/pre-commit equivalents.
- `agents/scripts/project/test-lint-rules.sh`: existing pre-ship lint gate used by Claude Code Stop hooks and manual Codex verification.
- `agents/scripts/core/test-portable-purity.sh`: existing robust Python probe pattern reused for the markdown-link gate.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no product runtime impact; harness/docs/scripts only.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no product runtime impact; no UI-thread code touched.
- **Pillar 3 (never crash)**: no product runtime impact; scripts must fail clearly rather than silently claiming parity.
- **Pillar 4 (accessibility - keyboard nav / font scaling / WCAG AA)**: no user-facing UI changes.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A - <reason>`)

N/A - planned diff is docs, harness scripts, and git hooks only; no `Source/Core/` or UI-reachable C++ paths.

1. **PR-fast CI** - N/A - no product/perf path touched.
2. **Pillar 2 static scanner** - N/A - no C++ UI path touched.
3. **Dispatcher drain** - N/A - no dispatcher code touched.
4. **Visible-cue bucket-E harness** - N/A - no sync-stall code path added.
5. **Marker inventory** - N/A - no perf markers added.

**Pre-push local check**: N/A - no perf scenario applies.

**Override**: N/A - no intentional perf regression.

## Risks / non-goals

- Risk: setup output could imply Codex has Claude Code's hook event system. Mitigation: the report must explicitly distinguish repo-owned checks from runtime-only features.
- Risk: git hook installation could trample a user's custom `core.hooksPath`. Mitigation: reuse the existing install behavior that only sets `scripts/git-hooks` when unset or already matching.
- Risk: bash-side tool probing can show false negatives on Windows PowerShell-capable machines. Mitigation: docs should describe the PATH split rather than treating it as a Codex adapter failure.
- Non-goal: implementing Codex product/runtime hooks from inside this repository.
- Non-goal: creating a `.codex/` mirror of `.claude/` templates while the Codex hook mechanism is unstable.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A - docs/harness/script-only change.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A - no visual/UI runtime change.
- **Bash-driver scenario / screenshot / sanitizer**: `bash agents/scripts/core/setup-harness.sh codex` should report agent count, hook state, and missing tools cleanly.
- **Build gate**: N/A - no C++ product code touched.
- **Doc validation (blocks plan-doc PRs - keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps - anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test - `grill-with-docs` (keep this bullet)**: stress-test this plan against the harness docs before finalising; record the outcome.
- **Manual residue**: any remaining Claude Code-only runtime features must be listed as non-repo-owned in Codex docs, not hidden as manual residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** - per `AGENTS.md` Process rules / Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Product-level Codex hook events: follow up only if Codex publishes stable hook config semantics.
- First-class named Smatchet specialist agent types inside Codex: out of repo control; repo files remain canonical prompts.
- Automatic token telemetry under Codex: opt-in follow-up when a stable subagent stop event exists.

## Implementation log

- `256669a8` - Add the active Codex harness parity plan and verification scope.
- `7979b346` - Wire Codex setup verification, repo-owned hooks, docs, and Windows-safe probe fixes.
- `dd6314e4` - Archive the plan and regenerate the shipped-plan index.

## Deviations from plan

- Scope widened from Codex setup/docs to shared tool and Python probe hardening after local verification exposed Git Bash / WSL false negatives.
- `agents/scripts/core/test-setup-harness.sh` gained Codex coverage and Windows-safe Python assertion probing so the new behavior is guarded.
- CI exposed a shipped-plan index date drift that local Windows Python masked; `test-plan-index.sh` now passes Git forward-slash paths to `git log --follow`.
- Product-level Codex hook events, first-class named Codex specialist agents, and automatic Codex token telemetry remain out of repo control and are documented as runtime gaps.

## Verification (actual)

- `bash -n agents/scripts/core/setup-harness.sh agents/scripts/core/test-setup-harness.sh agents/scripts/core/test-markdown-links.sh scripts/dev/check-required-tools.sh scripts/git-hooks/pre-commit scripts/git-hooks/pre-push` - passed.
- `bash agents/scripts/core/setup-harness.sh codex` - passed; reported 26 agent files, 5 leaf `AGENTS.md` files, and git-hook wiring.
- `powershell -NoProfile -ExecutionPolicy Bypass -File agents\scripts\core\setup-harness.ps1 codex` - passed.
- `bash agents/scripts/core/test-setup-harness.sh` - passed, 19/19.
- `bash agents/scripts/core/test-plan-index.sh --fix` - passed; regenerated `codex-harness-parity` with date `2026-06-08`.
- `bash scripts/dev/test-docs.sh` - passed, 10/10.
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` - passed; narrowing conversion opt-in skipped as designed.
- `git diff --check` - passed.
- PR #1008 CI / CodeRabbit before plan archive - all checks green, CodeRabbit completed with zero actionable findings.
- Plan stress-test - inline review against `docs/harness/codex/setup.md`, `docs/harness/codex/hooks-equivalent.md`, and `setup-harness` behavior; outcome: keep native `AGENTS.md` discovery plus repo-owned hooks, no `.codex` mirror.
