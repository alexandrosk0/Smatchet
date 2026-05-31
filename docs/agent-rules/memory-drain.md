# Weekly memory drain

The harness's auto-memory (`~/.claude/projects/<slug>/memory/`) is a
**transient inbox**, not a durable store. Facts captured there during sessions
rot: they go stale against the code, duplicate `AGENTS.md`, or stay invisible to
non-Claude-Code harnesses (Codex / Cursor / Aider read the repo, never user-private
memory). The weekly drain converts that inbox into repo-resident truth.

## Cadence

Event-driven, not calendar. The `SessionStart` hook `agents/scripts/core/memory-drain-nudge.sh`
checks the local inbox on every session start and prints a one-line nudge into
context when the inbox holds ≥ 5 live items **or** any item is > 7 days old
(both thresholds are env-overridable — see the hook script header). Act on
the nudge with the `/drain-memory` skill, or run it any time the user says
"drain memory". A remote/cloud routine **cannot** do this — the memory dir is
machine-local, outside the repo; only a local session sees it.

## Per-item triage — three verdicts

For **every** memory file in the inbox, decide one:

| Verdict | Action |
|---|---|
| **Implement** | The item is a durable behavioral rule or project invariant. Migrate it into the agentic structure: a 1-liner-or-less rule into `AGENTS.md` § Project rules, a fuller rule into the matching `docs/agent-rules/<topic>.md`. Then delete the memory file. |
| **Backlog** | The item is real follow-up work (a regression, a missing guard, a tooling gap) but not yet a rule. Add a dated entry to the right `docs/self-improvement/categories/<cat>.md` with a `Source: memory <slug> (drained <date>)` line. Then delete the memory file. |
| **Toss** | The item is already guarded in code, machine-specific (not portable repo truth), or contradicted by the current tree. Delete the memory file. Note the reason in the drain summary. |

## Verify before trusting

Memories are point-in-time. Before implementing or backlogging, **verify the claim
against the current tree** — cited file paths, function names, and "fixed in PR #N"
notes drift. A recent directory restructure that renamed core source paths and
moved config files already orphaned several memory citations. A memory that says
"now wired into tooling" may describe a guard that has since regressed — that flips
the verdict from *toss* to *backlog (regression)*.

## After the sweep

1. Reset `MEMORY.md` to the empty-inbox header (keep the banner + drain-spec link).
2. Record the tally in the `_(inbox empty — last drained ...)_` line:
   `N items → A implemented, B backlogged, C tossed`.
3. Ship the doc/backlog edits as a **PR** (never direct-push to develop) — pure-docs
   diff, skips build/tests. The memory-file deletions are user-private (outside the
   repo) and are not part of the PR.

## History

- 2026-05-30 — bootstrap drain. 6 items → 2 implemented, 1 backlogged, 3 tossed.
  Implemented `feedback-auto-launch-exe` (→ `ship-loops.md`) +
  `project-watcher-janitor-branch-swap` (→ `process-rules.md`). Backlogged
  `project-bats-gh-stub-windows` (→ tooling). Tossed `project-unreal-msvc-toolset`
  (guarded in code, PR #508), `project-python3-stub-windows` (machine-specific),
  and `project-build-vcvars-toolset` (still guarded on develop — `with-msvc-env.sh`
  reads `msvc_toolset_pin: 14.38` from `project.config.json`). **Verify-first
  lesson**: an initial read of `with-msvc-env.sh` showed the pin missing and the
  item was wrongly filed as an infra regression; the read had landed on a
  watcher-swapped sibling branch, not develop. Always `git show origin/develop:<file>`
  to verify, never trust the local working tree mid-session (see the very
  `project-watcher-janitor-branch-swap` rule this drain implemented).
