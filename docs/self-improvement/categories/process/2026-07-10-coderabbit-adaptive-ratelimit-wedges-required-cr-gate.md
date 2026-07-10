# Re-triggering `@coderabbitai review` resets the adaptive rate-limit window, wedging the required CR-findings gate

- 2026-07-10 · orchestrator (self-improvement campaign ship session) · [process] · P2 — high-volume campaigns exhaust CodeRabbit's per-developer adaptive limit, and manual re-triggers make it worse, blocking merge on the required `CR findings` status for hours

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

## Proposal

1. **Agent behaviour (cheap, do first):** when the `CR findings` gate is pending
   due to a CodeRabbit rate-limit, do **not** re-trigger — let the rolling window
   age out, then trigger once. Encode in the PR-babysit / ship-loop playbook next
   to the existing draft-PR note.
2. **Pace campaigns:** stagger PR *readiness* (mark ready in small batches) so CR
   review volume stays under the adaptive limit instead of firing N reviews at once.
3. **Gate design:** give the required `CR findings` check a staleness/timeout
   escape — e.g. after N hours pending with zero findings from any other reviewer
   (Bugbot/Copilot) it degrades to advisory — so an external throttle can't block
   merge indefinitely. This is the load-bearing fix.

Est: (1) ~10 min doc; (2) ~15 min playbook; (3) ~1–2h (poller/gate change).
This session resolved #1702 only via an operator-authorized admin merge past the
pending gate.

## Format

- Details: see § Friction. Verified: the rate-limit countdown reset was observed
  in the PR's `coderabbitai[bot]` comments (51s → 38m after a re-trigger); the
  gate context string is `CR findings (0 actionable)` with description
  "awaiting CodeRabbit review on current head".
- Concrete next action: see § Proposal (1)–(3).
- Triggered-follow-up: when=pr-count:base=develop;since=2026-07-10;n=25; action=re-check whether the required CR gate ever degraded gracefully under a throttle, or whether another campaign wedged again; baseline=#1702 blocked ~2h on CR rate-limit despite green CI + Bugbot clear; fired=never
- Status: open
- Last-reviewed: 2026-07-10
