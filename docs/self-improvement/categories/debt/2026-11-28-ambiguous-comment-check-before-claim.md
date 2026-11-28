- 2026-11-28 · cache-hardening · [debt] · P2 — An `ambiguous` pending comment is checked against the tracker BEFORE it is claimed, so a failed check by one Smatchet can undo another's live claim

Details:
`PendingActionQueueService::ReplayOne` takes a row read as `sending` over first (SQ-5, `TakeOverPendingActionClaim`),
but a row read as `ambiguous` still runs the tracker check (`ResolveAmbiguousComment`) before the `ambiguous` to `sending`
compare-and-set. If that check fails (a transport error), its failure write (`RecordFailure` then `UpdatePendingAction`)
matches on the row id only. With two Smatchet processes on one cache file:

1. both load the row as `ambiguous`;
2. A's check passes, A claims the row and starts sending;
3. B's check fails and B writes `ambiguous` over A's live claim;
4. about 30 s later (B's backoff) B checks again, A's slow send has not landed, B's compare-and-set wins and B sends it
   again.

It needs a transport failure on one side plus a send slower than the 30 s backoff (the tracker timeout goes up to 600 s),
and it predates SQ-5. Fix: claim `ambiguous` rows first as well (`ambiguous` to `sending`), do the tracker check holding
the claim, delete the row if the comment is found, and release it back to `ambiguous` when the check fails. That changes
the order of the offline-first replay for every ambiguous comment, so it needs its own slice and tests (a two-service
race over one `FakeSyncCache`, as in the SQ-5 take-over test). Until then the Sync leaf invariant says "never twice" only
for rows read as `sending`.

Found by the SQ-5 code review (cache-hardening plan, `docs/plans/active/cache-hardening.md`).
