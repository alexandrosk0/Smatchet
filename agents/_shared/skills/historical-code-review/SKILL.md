---
name: historical-code-review
version: 1
description: Review an OLD PR/commit's code, but only the lines it introduced that are STILL ALIVE and UNTOUCHED at HEAD — so re-reviewing history never re-flags code a newer PR already changed or fixed. Use to audit historical product code for surviving bugs/debt without noise from superseded lines. Pairs the deterministic survivor-extractor (agents/scripts/core/historical-review-survivors.sh) with a code-review pass over only the surviving hunks.
---

# Historical code review

Re-reviewing a merged PR is normally noisy: a finding may already be fixed by a
later PR, so flagging it again wastes everyone's time. This skill reviews **only
the lines a past change introduced that are still alive and untouched at HEAD**.

## The invariant (why it works)

`develop` **squash-merges**, so each PR is exactly **one commit** on `develop`.
`git blame HEAD` attributes every current line to the commit that **last**
touched it. So a line still blamed to a PR's squash commit = "introduced by that
PR and never touched since." A line a newer PR rewrote is re-attributed to the
newer commit and **drops out for free** — no diff math, no manual exclusion
list, no risk of re-flagging fixed code.

## Procedure

1. **Extract survivors** (deterministic, read-only):
   ```bash
   bash agents/scripts/core/historical-review-survivors.sh --pr <N> --context 3
   # or against any commit on HEAD's history:
   bash agents/scripts/core/historical-review-survivors.sh <commit-ish> --context 3
   ```
   The digest lists, per still-existing file, the surviving line ranges with
   their **current HEAD line numbers**. Line marks:
   - `<space>` = a **surviving line** (still blamed to the target) → **review this.**
   - `~` = a context line (added/changed by a *different* commit) → background only; **do not flag.**

2. **Short-circuit.** If the digest says `FULLY SUPERSEDED`, report
   "nothing to review — every introduced line was since changed/removed" and stop.

3. **Review only the survivors.** Feed the digest to a `code-review`-style pass
   (the project's `code-review` agent, opus/high). Rules:
   - Flag issues **only on `<space>`-marked surviving lines**, cited at their HEAD line number.
   - You MAY open the file at HEAD for fuller context (surrounding code may have
     changed since the PR), but never flag a line outside the surviving set —
     it's either someone else's code or already gone.
   - Apply the project's standard code-review checklist — the `code-review` agent
     already encodes it (language-standard compliance, logging convention, RAII /
     ownership, no empty `catch`, UI-thread non-blocking, no silent UB, and any
     project-specific header/include rules). Defer to the project's contract
     (`AGENTS.md` / `docs/agent-rules/`); don't restate the rules here.
   - Because already-fixed lines are excluded by construction, **assume nothing
     here is "already known/fixed"** — a surviving line is live debt.

4. **Output** a severity-tagged punch list, one finding per line:
   `path:HEADline: <CRITICAL|HIGH|MEDIUM|LOW>: <problem>. <fix>.`
   Lead each PR with `## Historical review — PR #<N> (<short-sha>)` and the
   survivor summary (`N files, M/K introduced lines alive`).

## Routing findings

Per [ADR-0014](../../../../docs/adr/0014-github-issues-canonical-for-product-bugs.md):
user-visible product defect / correctness / safety → **GitHub Issue**;
internal maintainability with no observable defect → **`docs/self-improvement/categories/debt.md`**.
When sweeping many PRs, batch the log (e.g. all of one 5-PR batch into one
`debt.md` commit) and elevate only clear user-visible bugs to Issues.

### Audit mode (work-item era)

When the sweep runs as a **standing-code audit** under the work-item process (Whip-Process
`Procedures/Audit.md` maps here; the itemful loop is `docs/agent-rules/work-items.md`), route into
`docs/work/` instead:

- **Debt already tracked is cited, not re-reported** — check `docs/work/DEFERRED.md` /
  `BACKLOG.md` and open bug issues before flagging; an existing `DEF/BL-NN` covering the finding is
  a citation, not a new entry.
- **Defers** become `docs/work/DEFERRED.md` entries with a Source line —
  `**Source:** <target> audit (YYYY-MM-DD).` — canonical `DEF-NN` assigned at the drain, and every
  `DEF/BL-NN` the audit *resolved* is retired (removed, cross-references repointed), per
  work-items.md § Tracking.
- **User-visible product defects** stay GitHub Issues (ADR-0014) — the audit does not change that
  boundary.
- **No re-litigation of choices** — settled decisions (closed summaries' *Key decisions*, existing
  ledger entries, prior resolutions) are the oracle, not findings; verifying a choice's
  **execution** is exactly the audit's job.

Findings from an audit round are resolved by the single addresser discipline
(`address-review-feedback` skill), and the drain above runs **after the user's sign-off** on the
resolution.

## Notes / limits

- The target commit must be an **ancestor of HEAD** (else "alive at HEAD" is
  meaningless). The script exits 2 otherwise.
- Files **renamed/deleted** since the PR are treated as "touched" → their lines
  don't survive (the script skips paths absent at HEAD). Acceptable: a rename is
  a change.
- `--pr` needs `gh`; a raw commit-ish does not (works offline via `git blame`).
- The extractor never mutates git. The review pass is read-only.
