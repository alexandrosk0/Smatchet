# The required CR-findings gate has no pass path when CodeRabbit never reviews (throttled, draft-skipped, or path-excluded)

- 2026-07-10 · orchestrator (self-improvement campaign ship session) · [process] · P2 — the required `CR findings` status pends forever when CR doesn't produce a review; high-volume campaigns exhaust the adaptive rate-limit (and re-triggers reset it), while docs/self-improvement-only PRs are path-excluded outright — both wedge merge

## Friction

Shipping 11 campaign PRs (#1682–#1692) plus follow-ups in one session pushed
CodeRabbit's per-developer review volume to the 95th percentile, where its
**adaptive** limit releases new reviews only gradually. The repo's required
`CR findings (0 actionable)` status check stays `pending` until CodeRabbit posts
a *completed* review on the PR's current head SHA, so the throttle blocked
#1702's merge for ~2h even though every real CI lane was green and Cursor Bugbot
had already reviewed it with zero actionable findings.

Two behaviours compounded it, both verified this session:

- CodeRabbit **skips draft PRs entirely** — the gate can never satisfy while the
  PR is a draft, so a fix-forward opened as draft sits pending until marked ready.
- Each manual `@coderabbitai review` that lands *inside* an active rate-limit
  window **resets the countdown** — observed the "next review available in" value
  jump from `51 seconds` back up to `38 minutes` immediately after a trigger. So
  re-triggering to "unstick" the gate is actively counterproductive.

The required gate has no degrade path when the external reviewer is unavailable,
so an upstream throttle translates directly into an unbounded merge block.

**Stronger variant, observed on the PR logging this very entry (#1718):** CodeRabbit
**path-excludes** `docs/self-improvement/**` (`!docs/self-improvement/**` in
`.coderabbit.yaml`), so for a docs/self-improvement-only PR it posts "Review skipped
due to path filters" and **never** produces a review. The `CR findings (0 actionable)`
gate is then **structurally unsatisfiable** — no amount of waiting or re-triggering
helps, because there is nothing for CR to review. Same class of failure (CR skips a
draft too), and the fix is the same: the gate must treat "CR will not / cannot review
this PR" (path-excluded, draft-skipped, throttled past a deadline) as **0 findings →
pass**, not perpetual pending.

## Proposal

1. **Agent behaviour (cheap, do first):** when the `CR findings` gate is pending
   due to a CodeRabbit rate-limit, do **not** re-trigger — let the rolling window
   age out, then trigger once. Encode in the PR-babysit / ship-loop playbook next
   to the existing draft-PR note.
2. **Pace campaigns:** stagger PR *readiness* (mark ready in small batches) so CR
   review volume stays under the adaptive limit instead of firing N reviews at once.
3. **Gate design (load-bearing):** the required `CR findings` check must have a
   pass path when CR does not produce a review. Two triggers: (a) an explicit
   **"Review skipped due to path filters"** (or draft-skip) comment from CR on the
   head SHA → treat as 0 findings → **pass immediately** (structural, not a wait);
   (b) after N hours pending with zero findings from any other reviewer
   (Bugbot/Copilot) → degrade to advisory. Without (a), any docs/self-improvement-only
   PR — including the ones this very backlog process produces — can never merge
   without an operator admin-merge.

Est: (1) ~10 min doc; (2) ~15 min playbook; (3) ~1–2h (poller/gate change).
This session resolved #1702 only via an operator-authorized admin merge past the
pending gate.

**Update (2026-07-10): partially implemented (the structural half of proposal 3).**
Ported the **selfImpOnly** terminal pass-signal from the client gate
(`merge-gates.sh`) to the SERVER gate (`.github/actions/cr-finding-gate/action.yml`),
the one that actually blocks merge: a diff entirely under `docs/self-improvement/**`
(path-excluded by `.coderabbit.yaml`, sanctioned by
`self-improvement-pr-review-exemption`) passes immediately, no CR wait — exactly
the docs-only-PR class that wedged. It is head-accurate (queries the PR's current
file list) and fail-closed on any `gh` pagination error.

A second, comment-body-based "terminal path-filter skip" pass was tried and
**dropped after CodeRabbit review** (#1724): CR's skip summary comment carries no
reliable head-commit anchor, so a stale skip comment from an earlier docs-only
commit could pass a LATER code commit before CR re-reviewed it (fail-open race).
selfImpOnly covers the recurring case without that hazard.

Still open (deliberately NOT auto-passed — unsafe): the **rate-limit on a CODE
PR** case. Auto-passing it would wave un-reviewed code through; the correct escape
stays the `cr-out-of-band` label + `cr-disposition:` attestation (already
supported). Proposals (1) don't-re-trigger and (2) pace-campaigns remain doc/
playbook follow-ups.

## Format

- Details: see § Friction. Verified: the rate-limit countdown reset was observed
  in the PR's `coderabbitai[bot]` comments (51s → 38m after a re-trigger); the
  gate context string is `CR findings (0 actionable)` with description
  "awaiting CodeRabbit review on current head".
- Concrete next action: see § Proposal (1)–(3).
- Triggered-follow-up: when=pr-count:base=develop;since=2026-07-10;n=25; action=re-check whether the required CR gate ever degraded gracefully under a throttle, or whether another campaign wedged again; baseline=#1702 blocked ~2h on CR rate-limit despite green CI + Bugbot clear; fired=2026-07-11
- Follow-up observation (2026-07-11): no recurrence. The backlog-takeover session merged five PRs
  (#1726, #1700, #1728, #1730, #1738) while CodeRabbit was continuously rate-limited (its
  "review limit reached" comment present on every PR, windows 15–58 min); the
  `CR findings (0 actionable)` check reached SUCCESS on each head within the normal CI window and
  every merge proceeded without an admin-merge or `cr-out-of-band` label. The remaining unsafe
  case (rate-limit wedging a code PR past its window) did not reproduce; proposals (1)/(2) stay
  open as playbook follow-ups.
- Status: open
- Last-reviewed: 2026-07-11
