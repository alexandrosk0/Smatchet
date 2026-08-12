# Plan — Concurrent-session guard hardening (Cluster A)
<!-- plan-date: 2026-06-18 -->

> **Slug**: `concurrent-session-guard-hardening` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — merged via PR #1388 (squash `9346fcf1c31e`).

## Context

Cluster A of the 2026-06-18 self-improvement sweep: seven open backlog entries (4 filed today) all trace to one architectural flaw in the two PreToolUse concurrent-session guards — `guard-shared-tree.sh` (advisory, bias-to-allow) and `guard-head-drift.sh` (the hard net). Both decide "is this a HEAD/working-tree-mutating git op" by **grepping the raw, pre-expansion `tool_input.command` string**, which produces false-positive DENYs (heredoc bodies, quoted strings, `cd`-relative ops, `$VAR` `-C` paths) and a couple of fail-OPEN holes (quoted `-C` path with spaces; Edit/Write blanket-deny with no worktree exemption).

Confirmed live during planning: a Bash tool call whose text merely *contained* the substrings `git merge`/`git reset`/`git checkout` (inside strings/heredocs) was DENIED by the live shared-tree guard with 3 siblings active — the `:566` P2 reproducing on the author. Repro (`bash file.sh` wrapper to dodge the very bug): FP1 verb-in-echo, FP2 heredoc-body, FP3 `-C "$VAR"`, FP4 `cd <wt> && git merge` all DENY; bare merge DENY (correct), `-C <literal-wt>` ALLOW (correct).

Intended outcome: after this lands, the guards stop false-denying commands that merely *mention* git verbs or target a worktree via `$VAR`/`cd`, the head-drift `-C` matcher no longer fail-OPENs on a spaced quoted path, and Edit/Write to a worktree file is exempt from the integration-tree drift deny — while every true rug-pull / direct-develop-commit stays blocked.

Originating entries: `tooling.md` :566 (P2), :560 (P3), :10 (P3), :70 (P2), :175 (P3), :328 (P3); `process.md` :28 (P3).

## Approach

**One PR, two scripts + two bats suites + one doc note.** Stop a git verb that only appears as text/argument from arming the *matching* decision in the **shared-tree guard** (the advisory one — bias-to-allow, where a missed match is safe). Two changes there: (a) a `strip_heredoc()` helper removes heredoc BODIES before the grep (a body line like `git reset` otherwise matches `^git` since grep is line-based); (b) the leading boundary is tightened to **command-position only** — `git` must follow start / `;` / `&` / `|` / `(` / `&&` / `||` (after optional whitespace), never a bare word or a quote — which kills the echo-argument, quoted-string, and comment false-positives without needing to strip quotes/comments. (A narrow whole-quote/comment `strip_noncode` was the first sketch; command-position matching turned out cleaner and is what shipped.)

**The hard head-drift guard's verb-matching grammar is deliberately left UNCHANGED** — loosening it would weaken the last-net guarantee, and the `:566` false-positive is filed against the shared-tree guard, not head-drift. Head-drift gets only its two specific holes fixed (the spaced-`-C` fail-OPEN and the Edit/Write worktree exemption); no stripping is added there.

For the shared-tree guard specifically (advisory, bias-to-allow on uncertainty): exempt only when EVERY mutating-op invocation carries its own `-C <worktree>` (verified per-op at command position, so a `-C` on a later op can't exempt an earlier bare op, and a verb appearing only as an argument isn't counted); an unresolvable `-C "$VAR"` is treated as worktree-targeted (a `-C $VAR` is never the shared tree literally — `:10`). A `cd <worktree>`-based exemption was considered and **rejected as unsound** (shell cwd across `&&`/`||`/`;`/subshell/`cd`-back isn't derivable from the raw string — Cursor #1388), so `cd <wt> && git …` stays blocked exactly as on develop; the canonical cross-worktree form is `git -C <ABSOLUTE-worktree-path>`. The deny message names that form and stops advertising the inline `SMATCHET_ALLOW_SHARED_SWITCH=1` prefix that cannot work (the hook reads its own env before the command runs).

For the head-drift guard (hard net): fix the `-C` opt grammar to accept a quoted path containing spaces (`-C "a b"`) — currently `[^[:space:]]+` truncates and the whole invocation fails to match, a direct-commit-to-develop fail-OPEN (`:328`); and add a worktree-target exemption to the Edit/Write branch keyed on `file_path` resolving under a linked worktree (`:70`). The no-jq `json_field` sed-fallback truncation (`:175`) is documented as residual — jq is present in every supported path (the bats suites require it) — rather than risking a fragile sed rewrite.

## Files to modify

1. `docs/harness/claude-code/hooks/guard-shared-tree.sh` — add `strip_heredoc()`; tighten the mutating-op match to command-position-only + run it on the heredoc-stripped command; `$VAR`/`cd`-worktree exemptions; rewrite deny reason.
2. `docs/harness/claude-code/hooks/guard-head-drift.sh` — extend `GIT_OPTS_RE` `-C`/`-c` to accept quoted spaced paths (the spaced-`-C` fail-OPEN); Edit/Write `file_path` worktree exemption. **No stripping / no matching-grammar change** (preserve the hard-net guarantee).
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
- PR #1388 (squash `9346fcf1c31e`) — `guard-shared-tree.sh`: `strip_heredoc` + command-position-only verb match (`:566`); per-op `-C <worktree>` exemption covering `$VAR`, command-position-anchored (`:10`); rewritten deny message (`:560`). `guard-head-drift.sh`: quoted-spaced-`-C` grammar (`:328`/`:175`); Edit/Write worktree-target exemption under drift (`:70`). `build.md`: `with-msvc-env.sh` + shared-tree edit-clobber note (`process.md:28`). `guard_shared_tree.bats` +ordering/cmd-position CASES, `guard_head_drift.bats` +spaced-`-C` + Edit-exemption CASES + 2 em-dash names ASCII'd.

## Deviations from plan
- **Dropped the `cd`-based worktree exemption entirely** (plan proposed `cd <worktree>`-then-op as exempt). Cursor Bugbot showed it unsound across two review rounds (shell cwd not derivable from the raw string — a later op can re-target the shared tree). The sound model that shipped: exempt only when EVERY command-position mutating op carries its own `-C <worktree>`. Returns to develop's deny-behaviour for `cd && git …` (no regression). Filed as `process.md` `pretooluse-git-guard-cwd-not-derivable-from-raw-command`.
- Used **command-position matching** for the shared-tree verb match instead of the originally-sketched `strip_noncode` (cleaner; kills quoted/echo/comment false-positives without quote-stripping). Head-drift's matching grammar left unchanged (preserve the last-net guarantee), only its two holes fixed.
- The no-jq `json_field` sed-fallback truncation (`:175`) left as a documented residual (jq is present on every supported path; the bats suites require it).

## Verification (actual)
- `guard_shared_tree.bats` 14/14 + `guard_head_drift.bats` all green; `shellcheck` clean (gated codes) on both hooks; `scripts/dev/test-docs.sh` 13/13; shell-lint gate exit 0. Every false-positive (FP1–FP4) confirmed flipped to ALLOW pre-fix→post-fix; every true positive (bare op, `-C`/`cd` at the integration tree, protected-branch worktree, subshell, spaced-`-C` at integration, ordering attacks) still DENIES. CodeRabbit + Cursor findings (plan/impl mismatch; cd/`-C` ordering; leading-cd unsoundness; per-op command-position) all addressed pre-merge.
