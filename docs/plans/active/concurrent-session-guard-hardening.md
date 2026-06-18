# Plan — Concurrent-session guard hardening (Cluster A)

> **Slug**: `concurrent-session-guard-hardening` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Cluster A of the 2026-06-18 self-improvement sweep: seven open backlog entries (4 filed today) all trace to one architectural flaw in the two PreToolUse concurrent-session guards — `guard-shared-tree.sh` (advisory, bias-to-allow) and `guard-head-drift.sh` (the hard net). Both decide "is this a HEAD/working-tree-mutating git op" by **grepping the raw, pre-expansion `tool_input.command` string**, which produces false-positive DENYs (heredoc bodies, quoted strings, `cd`-relative ops, `$VAR` `-C` paths) and a couple of fail-OPEN holes (quoted `-C` path with spaces; Edit/Write blanket-deny with no worktree exemption).

Confirmed live during planning: a Bash tool call whose text merely *contained* the substrings `git merge`/`git reset`/`git checkout` (inside strings/heredocs) was DENIED by the live shared-tree guard with 3 siblings active — the `:566` P2 reproducing on the author. Repro (`bash file.sh` wrapper to dodge the very bug): FP1 verb-in-echo, FP2 heredoc-body, FP3 `-C "$VAR"`, FP4 `cd <wt> && git merge` all DENY; bare merge DENY (correct), `-C <literal-wt>` ALLOW (correct).

Intended outcome: after this lands, the guards stop false-denying commands that merely *mention* git verbs or target a worktree via `$VAR`/`cd`, the head-drift `-C` matcher no longer fail-OPENs on a spaced quoted path, and Edit/Write to a worktree file is exempt from the integration-tree drift deny — while every true rug-pull / direct-develop-commit stays blocked.

Originating entries: `tooling.md` :566 (P2), :560 (P3), :10 (P3), :70 (P2), :175 (P3), :328 (P3); `process.md` :28 (P3).

## Approach

**One PR, two scripts + two bats suites + one doc note.** Add a shared preprocessing step to the *matching* decision in both guards: a `strip_noncode` helper that removes comments, single/double-quoted spans, and heredoc bodies before the mutating-op grep, so a git verb that only appears as text/argument no longer arms the guard. Because over-stripping can only make a match *less* likely (→ allow), this is philosophically safe for the advisory shared-tree guard; for the hard head-drift guard I apply stripping only to the *false-positive* direction and keep the existing conservative defaults everywhere else.

For the shared-tree guard specifically (advisory, bias-to-allow on uncertainty): treat an unresolvable `-C "$VAR"` path and a `cd <worktree>`-then-op prefix as worktree-targeted (exempt), and rewrite the deny message to name the `git -C <ABSOLUTE-worktree-path>` exemption and stop advertising the inline `SMATCHET_ALLOW_SHARED_SWITCH=1` form that cannot work (the hook reads its own env before the command runs).

For the head-drift guard (hard net): fix the `-C` opt grammar to accept a quoted path containing spaces (`-C "a b"`) — currently `[^[:space:]]+` truncates and the whole invocation fails to match, a direct-commit-to-develop fail-OPEN (`:328`); and add a worktree-target exemption to the Edit/Write branch keyed on `file_path` resolving under a linked worktree (`:70`). The no-jq `json_field` sed-fallback truncation (`:175`) is documented as residual — jq is present in every supported path (the bats suites require it) — rather than risking a fragile sed rewrite.

## Files to modify

1. `docs/harness/claude-code/hooks/guard-shared-tree.sh` — add `strip_noncode`; apply to the mutating-op match (line ~69); `$VAR`/`cd`-worktree exemptions (line ~79); rewrite deny reason (line ~98).
2. `docs/harness/claude-code/hooks/guard-head-drift.sh` — extend `GIT_OPTS_RE` `-C`/`-c` to accept quoted spaced paths (line ~95); add `strip_noncode` to the false-positive direction; Edit/Write `file_path` worktree exemption (line ~147).
3. `tests/bats/guard_shared_tree.bats` — CASES for FP1–FP4 + true-positive regressions (bare op still blocks, `-C <integration>` still blocks).
4. `tests/bats/guard_head_drift.bats` — CASES for spaced quoted `-C` commit (still denied), heredoc/quoted-verb no-false-deny, Edit-to-worktree-file allowed under drift, Edit-to-integration-file still denied under drift.
5. `docs/agent-rules/build.md` (or `agents/core/build-doctor.md`) — short note for `process.md:28`: prefer `scripts/dev/with-msvc-env.sh` over the `.ps1` wrapper (bypass-flag auto-denied), and commit-immediately + on-disk-patch when a concurrent-session shared-tree warning has appeared.

## Existing utilities reused

- `all_git_ops_target_safe_worktree()` — `guard-head-drift.sh:104` — the canonical linked-worktree `-C`-target classifier; the spaced-path fix extends its feeder grammar, not its logic.
- `sr_count_live_siblings()` — `session-registry-lib.sh` — sibling-liveness; unchanged.
- `json_escape` / `json_field` / `deny` helpers — both hooks — reused as-is.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — shell hooks, not UI-thread code.
- **Pillar 2 (UI never blocks)**: no impact — same.
- **Pillar 3 (never crash)**: no impact on the product; guards stay fail-safe (advisory fails OPEN, hard net keeps conservative defaults).
- **Pillar 4 (accessibility)**: N/A — no UI.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else N/A)

N/A — diff touches only `docs/harness/**` shell hooks + `tests/bats/**` + docs; no `Source/Core/` code.

## Risks / non-goals

- **Risk: over-stripping creates a fail-OPEN in the hard guard.** Mitigation — head-drift's stripping is applied only to suppress false-positives; the conservative `all_git_ops_target_safe_worktree` defaults (no `-C` → targets this tree; unresolved → not safe) are unchanged, and bats CASES assert every true positive still denies.
- **Risk: loosening shared-tree `$VAR`/`cd` exemption lets a rug-pull slip.** Accepted — the shared-tree guard is explicitly advisory/bias-to-allow ([script header line 8]); the hard head-drift net protects the victim regardless of who moved HEAD. A `cd`/`-C` resolving to the integration tree still denies.
- **Non-goal**: rewriting the guards onto a real shell parser / expanded-argv model. The harness only passes the raw command text; full parsing is out of scope. Residual edge (unquoted `echo git checkout`) documented, not fixed.
- **Non-goal**: the no-jq sed-fallback truncation (`:175`) — documented residual, jq is always present.

## Verification

- **Bucket A (pure-logic ctest)**: N/A — shell hooks, covered by bats.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver**: `bats tests/bats/guard_shared_tree.bats tests/bats/guard_head_drift.bats` green (existing + new CASES); `shellcheck` clean on both hooks; re-run the planning repro and confirm FP1–FP4 flip to ALLOW while true positives stay DENY.
- **Build gate**: N/A — no C++ touched (`is-pure-docs-diff.sh`-adjacent; shell+docs+test only).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test before finalising; record outcome.
- **Manual residue**: none expected — all verification is bats + shellcheck (deterministic).

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs to deferred items before finalising.

- Cluster B (merge-watcher) — next, separate PR.
- The unquoted-`echo`-arg and no-jq-sed-fallback residuals — left as documented edges (see Risks).

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
