- 2026-08-16 · orchestrator · [infra] · P2 — a `send_later` check-in that fires while the remote container is suspended is silently lost, so an autonomous ship-loop can park a finished PR indefinitely with no alarm and no retry
  Details: The autonomous backlog loop drives each PR to merge via self-scheduled
    `send_later` check-ins. On PR #2028 the 04:23Z check-in — whose whole job was
    to post the `@coderabbitai review` trigger once the rolling-hour quota
    reopened — never ran: the trigger record shows `last_fired_at
    2026-08-16T04:24:03Z` with `ended_reason: run_once_fired`, so the scheduler
    considered it delivered, but the session was suspended and no work happened.
    The PR then sat **11.5 hours** at head `8c1f1646` with all CI green and no
    review requested. Nothing surfaced it: the fire-and-forget check-in is
    one-shot, so a lost firing is indistinguishable from a firing that ran and
    found nothing actionable (the loop deliberately re-arms *silently* in that
    case, which is correct behaviour and exactly what makes the failure
    invisible).
    Compounding: the CR gate was green the whole time for an unrelated reason
    (sibling entry 2026-08-16-cr-gate-greens-on-manual-review-required-skip), so
    every surface signal said "ready to merge". The two failures point the same
    way — toward an unreviewed merge — which is what makes the pair worth a gate
    rather than a note.
  Concrete next action: make loss detectable rather than trying to make delivery
    reliable (the scheduler is not ours to fix). Cheapest shape: have the check-in
    prompt stamp a heartbeat — e.g. append `<pr> <head> <iso8601>` to a
    session-local file on every firing — and have the SessionStart nudge compare
    the newest heartbeat against any OPEN PR authored by this session whose head
    is older than ~2h, raising `WARN: PR #<n> has had no check-in for <N>h` so a
    resumed session immediately re-arms instead of assuming the loop is alive.
    A cheaper stopgap that needs no new state: on SessionStart, list this
    account's open PRs on `claude/*` branches and re-poll each one's gates —
    a resumed session should never assume an in-flight PR is being watched.
    Related: infra/2026-08-05-merge-watcher-liveness-unmonitored covers the same
    "the watcher itself is unwatched" shape for the merge-watcher process.
  Status: open
  Last-reviewed: 2026-08-16
