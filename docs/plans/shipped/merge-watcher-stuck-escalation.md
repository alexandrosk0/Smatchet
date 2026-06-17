# Plan — smatchet-merge-watcher stuck-PR escalation

> **Slug**: `merge-watcher-stuck-escalation` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — wedge classifier + `STUCK_NEEDS_ATTENTION` escalation + CLI highlight + SessionStart nudge landed; see § Implementation log.

## Context

The `smatchet-merge-watcher` daemon (`agents/scripts/core/merge-watcher.py`)
polls every registered PR via `merge-gates.sh` and merges it on gate-pass. When
a PR gets **wedged** in a non-progressing blocked state, the watcher just logs
`BLOCKED` every cycle forever — no escalation ever reaches a human or session.

Two real cases on 2026-06-11 with [PR #1139](https://github.com/alexandrosk0/Smatchet/pull/1139):

1. **`mergeStateStatus=DIRTY` / `mergeable=CONFLICTING`** — develop advanced under
   the PR (sibling PR #1146 edited the same file). GitHub refuses the squash;
   the watcher's PASS branch hits `merge_failed` and re-polls forever.
2. **`require_conversation_resolution=true` with unresolved CodeRabbit review
   threads** while all CI checks are green — merge-gates returns BLOCKED, the
   watcher logs it, no escalation. CR does **not** auto-resolve its threads on a
   fix push, so the PR can sit green-but-blocked indefinitely.

**Intended outcome — after this lands:** a PR stuck in a non-progressing wedge
(CONFLICTING/DIRTY, BEHIND, CI-failing, or BLOCKED-with-all-CI-green-but-
unresolved-threads) for ≥ N consecutive polls raises a **visible signal** via
three independent surfaces — a SessionStart nudge, a desktop notification, and a
distinct registry state `STUCK_NEEDS_ATTENTION` the CLI `status` highlights —
while **never** false-alarming on a normal in-flight PR (CI still running).

## Approach

Add a **wedge classifier** that runs once per poll cycle, gated to the same
states the existing CR drivers already handle (`BLOCKED`, plus `GATES_PASSED`
where `handle_pass` recorded `merge_failed` — the Case-1 DIRTY path). The
classifier reuses the parsed merge-gates Poll line for cheap transient-vs-wedge
discrimination (CI fail/pending/req-missing counts) and one `gh pr view --json
mergeStateStatus,mergeable,headRefOid` for the authoritative wedge reason. A
per-(PR, head) streak counter — modelled exactly on the existing
`maybe_pass_cr_none_grace` / `_bump_cr_none_grace` cross-cycle counter pattern —
must reach a threshold (`MERGE_WATCH_STUCK_CYCLES`, default 3) before escalating,
so a one-cycle transient never trips it. The streak is head-pinned: a fresh push
(new `headRefOid`) resets it, because a push is exactly the kind of progress that
un-wedges a PR.

The discrimination is the crux: **transient** = CI still pending / a required
check not yet reported → reason `None` → reset streak (do NOT escalate).
**Wedge** = CONFLICTING/DIRTY (conflict), BEHIND (behind base),
ci_fail>0 (CI failing), or BLOCKED+all-CI-green+unresolved-CR-threads
(`UNRESOLVED_THREADS`) / +no-threads (`REVIEW_REQUIRED`). On threshold the entry's
`last_state` flips to `STUCK_NEEDS_ATTENTION`, which is added to `NOTIFY_STATES`
(one toast per entry, suppressed thereafter by the existing
`notify_dispatched_for_state` guard) and surfaced by the always-visible CLI
status highlight + SessionStart nudge (both suppression-independent).

Non-obvious trade-off: the classifier is **fail-closed** — any gh failure or an
unparseable Poll line yields reason `None` (skip), never a spurious escalation.
A missed escalation self-heals next cycle; a false one cries wolf and erodes
trust in the nudge.

## Files to modify

1. `agents/scripts/core/merge-watcher.py` — add `import re`; five new module
   functions (`_stuck_cycles`, `_parse_poll_ci_counts`, `_classify_pr_wedge`,
   `_bump_stuck_streak`, `maybe_escalate_stuck_pr`) modelled on the
   `maybe_pass_cr_none_grace` family ([merge-watcher.py:1258](../../../agents/scripts/core/merge-watcher.py:1258));
   add `STUCK_NEEDS_ATTENTION` to `NOTIFY_STATES`
   ([merge-watcher.py:840](../../../agents/scripts/core/merge-watcher.py:840)); add a
   STUCK message branch to `maybe_notify`
   ([merge-watcher.py:851](../../../agents/scripts/core/merge-watcher.py:851)); wire
   `maybe_escalate_stuck_pr` into `daemon_loop` between the resolve-threads and
   notify steps ([merge-watcher.py:2148](../../../agents/scripts/core/merge-watcher.py:2148)).
2. `agents/scripts/core/merge-watcher-cli.py` — `cmd_status` STUCK highlight:
   a NOTE column reading `stuck_reason`/`stuck_streak` + a footer warning line
   ([merge-watcher-cli.py:272](../../../agents/scripts/core/merge-watcher-cli.py:272)).
3. `agents/scripts/core/merge-watcher-stuck-nudge.sh` — **new** SessionStart
   nudge (advisory, exit 0) reading `state/*.json` for `STUCK_NEEDS_ATTENTION`;
   `--list` / `--nudge` modes; modelled on
   [`postmortem-owed.sh`](../../../agents/scripts/core/postmortem-owed.sh).
4. `docs/harness/claude-code/settings.json.tmpl` — register the nudge in the
   SessionStart hooks array (the deployed `.claude/settings.json` is gitignored;
   `sync-settings-hooks.sh` heals provisioned copies additively on next start).
5. `tests/bats/merge_watcher.bats` — function-level tests for the classifier +
   the streak/threshold behaviour, modelled on the `_parse_gate_carry` /
   `_bump_nudge_state` importlib tests.

## Existing utilities reused

- `maybe_pass_cr_none_grace` + `_bump_cr_none_grace` + `_cr_none_grace_cycles`
  `agents/scripts/core/merge-watcher.py:1205`–`1338` — exact template for the
  head-pinned cross-cycle streak counter (gate → fetch head fail-closed → reset
  on head change → increment → threshold).
- `_fetch_unresolved_cr_threads` `agents/scripts/core/merge-watcher.py:1368` —
  reused to detect `UNRESOLVED_THREADS` vs `REVIEW_REQUIRED`.
- `_looks_like_cr_finding_block` / `_looks_like_cr_none_grace_wait`
  `agents/scripts/core/merge-watcher.py:1057`/`1072` — gate-out so the wedge
  classifier never double-fires on states the triage / grace drivers own.
- `_gh_json` `agents/scripts/core/merge-watcher.py:561` — single gh-view call.
- `maybe_notify` + `NOTIFY_STATES` `agents/scripts/core/merge-watcher.py:840`/`851`
  — existing one-toast-per-state notification surface + suppression.
- `smatchet-notify.sh` — desktop-toast channel (in-app → BurntToast → file-log).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — daemon-side Python
  on a 60s poll cadence; touches no `Source/Core/` render path.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: N/A — no
  UI-thread code; the one extra `gh pr view` per stuck-candidate poll is in the
  background daemon, off the UI thread entirely.
- **Pillar 3 (never crash)**: fail-closed on every gh / parse error (reason
  `None` → skip); no new unguarded indexing; the daemon's existing
  `write_state` OSError guard already wraps the cycle.
- **Pillar 4 (accessibility)**: N/A — terminal/CLI + OS-toast surfaces, no new
  GUI.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

`N/A — diff touches only agents/scripts/core/*, docs/harness/*, tests/bats/*;
no Source/Core/ files.`

## Risks / non-goals

- **Risk: false escalation on a normal in-flight PR.** Mitigated by the
  transient-vs-wedge split (CI pending / req-missing → reason `None` → reset) +
  the N-cycle threshold + head-pinned reset on push.
- **Risk: gh flakiness manufacturing a wedge.** Mitigated fail-closed — any gh
  error yields reason `None` (skip this cycle), never an escalation.
- **Risk: double-firing with the CR triage / grace / resolve drivers.**
  Mitigated by gating out `_looks_like_cr_finding_block`,
  `_looks_like_cr_none_grace_wait`, and a same-cycle `resolve_action` that made
  progress before the classifier runs.
- **Non-goal: auto-*fixing* the wedge** (rebase-onto-develop, auto-resolving
  human-relevant threads). This plan only *escalates*; remediation stays
  human-initiated. Auto-rebase-on-DIRTY is flagged below.
- **Non-goal: changing the merge-gates DIRTY policy.** merge-gates still blocks
  only BLOCKED|BEHIND; Case-1 DIRTY is caught via the `merge_failed` PASS-path,
  not by changing what merge-gates blocks.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — Python daemon, not C++.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver scenario / bats**: `tests/bats/merge_watcher.bats` —
  `_parse_poll_ci_counts` parses fail/pending/req-missing from a real Poll line
  and returns None on garbage; `_classify_pr_wedge` returns the right reason per
  monkeypatched gh (CONFLICT / BEHIND / CI_FAILING / None-transient /
  UNRESOLVED_THREADS); `maybe_escalate_stuck_pr` only flips to
  `STUCK_NEEDS_ATTENTION` after `MERGE_WATCH_STUCK_CYCLES` consecutive cycles and
  resets on head change. Plus a `merge-watcher-stuck-nudge.sh` smoke test
  (empty registry → silent exit 0; one STUCK state file → `--nudge` block).
- **Build gate**: N/A — no C++ compiled.
- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: stress-tested against the
  merge-watcher state-machine + the merge-gates Poll-line grammar; sharpened the
  transient-vs-wedge boundary (CI pending/req-missing = transient, not wedge) and
  confirmed the head-pinned reset matches the existing grace-counter semantics.
  Outcome: design holds; no term drift.
- **Manual residue**: none — all logic is bats-covered; the desktop-toast + the
  three-surface integration are exercised by the existing `maybe_notify` /
  nudge-script tests.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: no deferred symbols introduced into CONTEXT/ADR/
agents docs; nothing to sweep.

- **Auto-rebase on DIRTY** — a future driver could `gh pr update-branch` (or a
  spawned rebase session) on a CONFLICT wedge instead of only escalating.
  Follow-up: tracked as a `tooling.md` backlog candidate if the escalation alone
  proves insufficient in practice. No-action now.
- **M-minutes threshold** (vs N-cycles) — the user named "N polls or M minutes";
  N-cycles is implemented (cycles × poll-interval ≈ minutes). A wall-clock
  variant is redundant given the fixed cadence. No-action.

## Implementation log

- `merge-watcher.py` — added `STUCK_NEEDS_ATTENTION` to `NOTIFY_STATES`; five new
  module functions (`_stuck_cycles`, `_parse_poll_ci_counts`, `_classify_pr_wedge`,
  `_bump_stuck_streak`, `maybe_escalate_stuck_pr`) modelled on the
  `maybe_pass_cr_none_grace` / `_bump_cr_none_grace` family (head-pinned registry
  streak, fail-closed gh, transient-vs-wedge split, N-cycle threshold); wired
  `maybe_escalate_stuck_pr` into `process_registered_pr` between the resolve-threads
  and notify steps so the flipped state fires one toast.
- `merge-watcher-cli.py` — `cmd_status` gained a `NOTE` column (`STUCK[<reason> x<streak>]`)
  + a footer WARNING line listing escalated PRs.
- `agents/scripts/core/merge-watcher-stuck-nudge.sh` — **new** SessionStart nudge
  (`--list` / `--nudge`, advisory exit 0) reading the watcher state via the CLI
  module's own `state_dir()`/`read_registry()`; silent unless a PR's state file is
  `STUCK_NEEDS_ATTENTION`.
- `docs/harness/claude-code/settings.json.tmpl` — registered the nudge in the
  SessionStart hooks array (after `plan-archival-owed`).
- `tests/bats/merge_watcher.bats` — four function-level + nudge-smoke tests.

## Deviations from plan

- **`import re` step was a no-op** — already imported at `merge-watcher.py:61`; plan
  predated that.
- **No dedicated STUCK branch in `maybe_notify`** — the plan listed one, but the
  existing generic `NOTIFY_STATES` path already composes the toast from
  `last_status_line` (which `maybe_escalate_stuck_pr` rewrites to a descriptive
  `STUCK_NEEDS_ATTENTION (<reason>) — …` line). Adding `STUCK_NEEDS_ATTENTION` to
  `NOTIFY_STATES` was sufficient; a special branch would have duplicated the
  generic path. Lower-risk, same surface.
- **All plan line-numbers had drifted** (~140–300 lines; file grew 2148→2486) — the
  named functions/patterns all still existed and were re-anchored by reading
  current context rather than trusting the cited lines.
- **Wedge-classify precedence: `fail>0` tested BEFORE pending/req-missing** — a
  failing required check is a wedge regardless of other still-pending checks (it
  won't self-heal without a push). The plan's prose listed pending/req-missing as
  "transient" without specifying the order; a unit test surfaced the ambiguity.
- **User-facing nudge/CLI strings are pure ASCII** (`--`, not `—`) so the nudge's
  inline-python stdout can't `UnicodeEncodeError` on a cp1252 Windows console.

## Verification (actual)

- `python -m py_compile` both modules — clean.
- `tests/bats/merge_watcher.bats` — full suite green (exit 0); the 4 new cases
  (`_parse_poll_ci_counts`, `_classify_pr_wedge` 7-way, `maybe_escalate_stuck_pr`
  threshold/reset/head-change/gating, nudge silent-vs-block smoke) all PASS.
- `shellcheck merge-watcher-stuck-nudge.sh` — clean.
- Manual end-to-end: seeded a STUCK registry+state → `merge-watch status` shows the
  `STUCK[CONFLICT x4]` NOTE + footer WARNING; `--nudge` emits the block; empty/cleared
  state → silent exit 0.
- `scripts/dev/test-docs.sh` — green (plan archived to `shipped/`, index regenerated).
