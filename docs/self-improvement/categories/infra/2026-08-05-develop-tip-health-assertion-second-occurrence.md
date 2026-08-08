# Develop-tip health assertion — second occurrence, promote it

- **Category**: infra
- **Priority**: P1
- **Date**: 2026-08-05
- **Observed on**: PR #1957 (installer smokes red on develop for 3 merges)

## What happened

`Windows x64 installer smoke` and `Windows-on-ARM ARM64 installer smoke` went RED
on develop at #1957 and stayed red while #1959 and #1960 merged on top. Nothing in
the pipeline asserts the develop tip is green before the next merge, and #1957's own
develop run was **cancelled** by a superseding push, so the failure never even
announced itself on the PR.

This is the *identical class* the 2026-07-10 / #1698 postmortem already named and
proposed a gate for — "required check goes red on develop and nobody notices until it
blocks the next PR". That proposal was filed as a follow-up and never landed. This is
its second occurrence, with a worse variant: the red checks here are **non-required**
post-merge backstops, so even the block-on-any-red inheritance that surfaced #1698
did not fire.

## Proposed action

Land the assertion the #1698 entry proposed, widened to non-required checks:

- A `develop-tip-required-green.sh` (or an extension of
  `agents/scripts/core/postmortem-owed.sh`'s sweep) that queries the develop tip's
  check conclusions — **required and non-required alike**, matching the merge-gate
  allow-list philosophy in `AGENTS.md` § Merge gates — and raises a loud, attributable
  SessionStart nudge naming the PR whose merge introduced each red.
- Treat a **cancelled** post-merge run on develop as unknown-not-green, since that is
  precisely how #1957 hid.

## Why it matters

Post-merge backstop jobs are the *only* coverage for code PR checks structurally cannot
run (here: a ~20-30 min LTO publish build). If nothing reads their result, they are
decorative — the break sat on develop across three merges and was found by an ad-hoc
adversarial review, not by the system.
