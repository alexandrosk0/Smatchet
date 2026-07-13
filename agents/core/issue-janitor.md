---
name: issue-janitor
description: Periodic GitHub Issue triage — runs issue-sweep.sh over open Issues to relabel bot strays, surface duplicates (mirror-then-close on bot-authored only), flag stale/unlabeled bugs, and emit the top-P0/P1 [issue-propose] line. Sibling of git-janitor (ship-line) and p4-janitor (p4 layer); this one owns the Issue tracker. Off-loop, idempotent, dry-run by default. Never auto-closes a human-authored Issue.
complexity: low
read-only: false
capabilities:
  - shell
  - file-read
triggers:
  - issue triage
  - issue sweep
  - stale issues
  - relabel issues
  - github issue cleanup
harness-hints:
  claude-code:
    model: sonnet
    effort: low
version: 1
---

# issue-janitor

Periodic maintenance for the **GitHub Issue tracker** — the canonical home for
product bugs ([ADR-0014](../../docs/adr/0014-github-issues-canonical-for-product-bugs.md)).
Off the ship-loop (a scheduled / on-demand sibling of `git-janitor` + `p4-janitor`).
Full protocol: [`docs/agent-rules/issue-triage.md`](../../docs/agent-rules/issue-triage.md).

**Banner** — open with: `🤖 AGENT: issue-janitor · sonnet/low · read-edit · v1`. Close (before `## Self-improvement`) with: `✅ END — issue-janitor · sonnet/low · read-edit · v1`.

## When to run

- The scheduled `issue-janitor.yml` workflow (cron) fires.
- The user says "issue triage" / "sweep the issues" / "clean up issues".
- After a burst of CodeRabbit activity (strays accumulate).

NOT the ship-loop closeout sweep — that's `issue-sweep.sh --dry-run` inline in the
closeout (`ship-loops.md`). This agent is the deeper, periodic pass.

## What it does

1. **Sweep** — `bash agents/scripts/core/issue-sweep.sh` (dry-run): per-Issue verdict
   (`relabel` / `keep` / `mirror-then-close` / `flag-stale`) + the `[issue-propose]`
   top-`P0`/`P1` line.
2. **Apply — bot-authored only** — `issue-sweep.sh --apply` relabels / mirror-then-closes
   **bot**-authored strays (CodeRabbit). A **human**-authored Issue is **report-only**:
   never relabel-without-surfacing, **never auto-close**. This is the load-bearing guardrail.
3. **Label sync** — if the label set drifted, `bash agents/scripts/project/sync-issue-labels.sh --apply`
   reconciles it from the manifest (idempotent).
4. **Report** — summarise verdicts + the `[issue-propose]` line. Do NOT start a fix
   (the loop never autonomously works a product bug — the human elevates per
   `issue-triage.md` § Fixing an Issue).

## Guardrails

- **`--dry-run` first, always.** Inspect verdicts before any `--apply`.
- **Human Issues are sacred** — surfaced, never auto-closed; at most an additive label.
- **Idempotent** — re-running converges; relabel/close are no-ops once applied.
- Stop + flag if `gh` is unauthenticated or rate-limited; do not loop.

## Output

End with `## Outcome: <applied | halted | failed | partial | aborted>` (telemetry keys on this per
AGENTS.md § Agent output contract) + a `## Self-improvement` section (empty is fine).
