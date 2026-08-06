- 2026-08-05 · claude-code · [tooling] · P2 — The gate-poller filters bot review threads out of its user-comment gate, but `required_conversation_resolution` counts them — so the poller can report all-clear on a PR GitHub will never merge

  Observed on PR #1937 (Help > About dialog). After the missing `CR finding gate`
  check-run was resolved (see
  `2026-08-04-required-check-cancelled-while-pending-wedges-poller.md`), the head
  was 43 SUCCESS / 5 SKIPPED / 0 fail, `cr-out-of-band` was set with a
  `cr-disposition:` marker, and the poller printed `User: 0`. GitHub still
  reported `mergeStateStatus=BLOCKED` and refused the merge.

  Cause: branch protection on `develop` sets
  `required_conversation_resolution: {"enabled": true}`, and GitHub counts **every**
  unresolved review thread — including bot-authored ones. Ten unresolved CodeRabbit
  threads were open on the PR. The poller's gate #3 deliberately excludes them:
  `agents/scripts/core/merge-gates.d/10-gate-filter.sh:210-212` selects only threads
  with `.author.__typename != "Bot"` and a login other than `ORCH_USER`. Gate #2 (the
  CodeRabbit gate) passes on `APPROVED` / `COMMENTED + 0 actionable` and is separately
  waivable via `cr-out-of-band` — but that label waives the **poller's** gate. GitHub
  branch protection has never heard of it. So both poller gates read green while the
  thing actually holding the merge was a count neither of them measures.

  Why it matters beyond this PR: the divergence is silent and it fails in the
  expensive direction. The poller's own output is what an operator (or the
  merge-watcher) reads to decide whether to keep waiting, and it says the PR is
  ready. The only signal to the contrary is the opaque `mergeStateStatus=BLOCKED`
  line, which names no cause. On #1937 this cost the full ~90 min budget and a
  manual GraphQL sweep to discover the ten threads and resolve them one by one.
  The `cr-out-of-band` label makes it *worse*, not better: waiving the CR gate is
  precisely the situation in which unresolved CR threads are expected to remain,
  so the label reliably steers into the wedge.

  Proposed fix: project the **unfiltered** unresolved-non-outdated thread count as a
  new field alongside the existing user count, and on a `mergeStateStatus=BLOCKED`
  poll where every other gate passes, emit an actionable BLOCK naming it — e.g.

      BLOCK: mergeStateStatus=BLOCKED with all gates green; 10 unresolved review
             thread(s) (0 user, 10 bot) and branch protection requires conversation
             resolution. Resolve them or the merge will never unblock.

  The thread nodes are already fetched by the same GraphQL query, so this is a jq
  projection change, not an extra API call. Gate the message on the repo actually
  having `required_conversation_resolution` enabled (one `gh api
  repos/{o}/{r}/branches/{base}/protection` read, cached per run) so it does not
  fire spuriously on repos without it.

  Deliberately **not** proposed: making the poller resolve bot threads itself. That
  is a merge-blocking judgement call — auto-resolving CR threads would silently
  discard findings, which is exactly the failure mode `cr-out-of-band` already has a
  `cr-disposition:` attestation to prevent.

  Concrete next action: add the unfiltered-thread-count field to
  `agents/scripts/core/merge-gates.d/10-gate-filter.sh` and the BLOCK branch to
  `agents/scripts/core/merge-gates.sh`, with a `tests/bats/merge_gates.bats` case
  pinning the "all gates green + BLOCKED + N bot threads" path to the actionable
  message. Also worth a line in `docs/agent-rules/merge-gates.md` § CodeRabbit gate:
  `cr-out-of-band` waives the poller's gate only — it does not waive branch
  protection's conversation-resolution requirement.

  Status: open
  Last-reviewed: 2026-08-05
