---
name: drain-memory
description: Drain the harness auto-memory inbox into the repo. Triages every memory file under ~/.claude/projects/<slug>/memory/ (except MEMORY.md) into implement / backlog / toss per docs/agent-rules/memory-drain.md, verifying each claim against the current tree first, then clears drained rows and ships the repo edits as a PR. Use when the user says "drain memory", "triage memory", the SessionStart nudge fires, or the inbox has grown stale. Read-write — edits repo docs + deletes local memory files.
version: 1
---

# drain-memory

One-keystroke driver for the weekly memory drain. The full process spec is
[`docs/agent-rules/memory-drain.md`](../../../docs/agent-rules/memory-drain.md);
this skill is the invocation shortcut.

## When

- User says "drain memory" / "triage memory".
- The SessionStart nudge (`scripts/dev/memory-drain-nudge.sh`) fired this session.
- Inbox holds ≥ 5 live items or any item is > 7 days old.

## Steps

1. **Locate the inbox.** `ls "$HOME/.claude/projects/"*/memory/` — pick the dir
   whose slug names this repo. List every `*.md` except `MEMORY.md`.
2. **If empty** → report "inbox empty" and stop.
3. **Read each item.** For every memory file, decide one verdict — and
   **verify the claim against the current tree before trusting it** (cited paths
   / symbols / "fixed in PR #N" notes drift):

   | Verdict | Action |
   |---|---|
   | **Implement** | Durable rule → migrate into `AGENTS.md` § Project rules (1-liner) or the matching `docs/agent-rules/<topic>.md`. Delete the file. |
   | **Backlog** | Real follow-up (regression / missing guard / tooling gap) → dated entry in `docs/self-improvement/categories/<cat>.md` with a `Source: memory <slug> (drained <date>)` line. Delete the file. |
   | **Toss** | Already code-guarded, machine-specific, or contradicted by the tree → delete the file; note the reason in the summary. |

   A memory that says "now wired into tooling" but whose guard has since
   regressed flips from *toss* to *backlog (regression)* — that is the point of
   verifying first.
4. **Reset `MEMORY.md`** to the empty-inbox header with a dated tally
   (`N items → A implemented, B backlogged, C tossed`).
5. **Ship the repo edits as a PR to develop** (pure-docs diff, skips build/tests;
   never direct-push). The memory-file deletions are user-private (outside the
   repo) and are not part of the PR.
6. **Append a History line** to `docs/agent-rules/memory-drain.md` § History.

## Boundaries

- Verify-before-trust is mandatory — never migrate a stale claim verbatim.
- Never commit the local memory dir into the repo (user-private).
- Pure-docs PR only; do not bundle unrelated changes.
