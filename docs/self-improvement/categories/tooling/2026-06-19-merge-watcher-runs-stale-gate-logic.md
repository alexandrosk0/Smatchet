- 2026-06-19 · orchestrator · [tooling] · P1 — the `smatchet-merge-watcher` daemon runs `merge-gates.sh` from its own long-lived host checkout, so a daemon started days ago silently enforces STALE gate logic (block-allowlist / CI rules) that has since changed on develop; it merged #1428 past a red block-allowlisted `Intent section` check because its checkout predated the #1391/ADR-0022 allow-list addition
  Details: PR #1428 (merge 092b23480c5f429eb42c1349984976e5e81fffa7, 2026-06-19T17:47:52Z, head 4f6d6624)
    merged to develop with the block-allowlisted `Intent section` doc-validation check terminal `failure`
    (completed 17:46:44Z, ~68s before merge) and NO `intent-out-of-band` override — see postmortems.md
    2026-06-19 PR #1428. Unlike the #1406/#1414/#1415 poller-BYPASS escape, the poller WAS consulted
    (mergeActor:merge-watcher; snapshot row redChecks:[] / GATES_PASSED, merge-snapshots.jsonl line 105) —
    but the watcher daemon (merge-watcher.py, Scheduled Task SmatchetMergeWatcher, running since 2026-06-15)
    executes `MERGE_GATES_SCRIPT = _HERE/"merge-gates.sh"` from its OWN host checkout (integration tree
    C:/Dev/Smatchet parked on feat/tsan-subset-sync-layer, predating #1391). `Intent section` was added to
    MERGE_GATES_BLOCK_ALLOWLIST_RE on 2026-06-18 (#1391/ADR-0022); the daemon's blob predated that, so the
    allow-list regex did not match → the non-required RED was treated as advisory → GATES_PASSED. A
    long-running daemon never re-syncs, so it silently ran 2-day-stale rules; its own snapshot audit, written
    by the stale poller, self-reports clean and cannot detect the drift.
  Concrete next action: SHIPPED in this PR (fix/intent-section-merge-gate) — a gate-logic self-freshness
    guard in merge-gates.sh: before emitting GATES_PASSED it compares its own running file's git blob to
    origin/develop's blob for the same path and refuses (fail-closed) on divergence or unverifiable, gated by
    MERGE_GATES_FRESHNESS ∈ {off(default)|warn|block}; merge-watcher.py sets block; 5 bats cases. RESIDUAL
    (this backlog item tracks): (1) restart the watcher daemon from a develop-current checkout — done as
    stop + Scheduled-Task-disable this session (fail-safe); restart deferred until a develop-current checkout
    is safe to make on the shared integration tree; (2) add a periodic daemon self-resync / restart-on-
    develop-advance (or a startup git-freshness self-check that re-execs after `git pull`), so a long-lived
    watcher cannot drift in the first place — the freshness guard fails CLOSED, but a wedged-stopped daemon
    means zero auto-merge throughput until a human restarts it, so the resync path is the throughput-safe
    complement. Est ~0.5d for (2).
  Update 2026-06-20: (2) SHIPPED — merge-watcher.py now has a gate-logic self-resync (`maybe_self_resync`,
    `MERGE_WATCH_AUTO_RESYNC` default on; cadence `MERGE_WATCH_RESYNC_EVERY_CYCLES` default 30 + a startup
    check in daemon_loop). It fetches origin/develop, compares the on-disk blob of every gate-logic file
    (`detect_gate_logic_drift` over merge-gates.sh / .graphql / merge-watcher*.py — same hash-object-vs-
    origin/develop compare the freshness guard uses), and on a SAFE fast-forward (`_resync_safety`: clean
    tree + on develop + HEAD an ancestor of origin/develop) `git pull --ff-only`s develop so the on-disk
    merge-gates.sh goes fresh → the next poll re-reads it → the fail-closed guard passes again (throughput
    restored). When the daemon's OWN code (merge-watcher.py / -cli.py) drifted it re-execs to reload — POSIX
    only (`os.execv` is a clean same-PID replace); on Windows it logs a restart-needed WARN instead because
    os.execv there is emulated as spawn-new-PID + terminate-self, which would DETACH the daemon from its
    Scheduled Task (orphan + double-instance-at-next-login risk). The UNSAFE-tree case (the #1428 feature-
    branch park / dirty / diverged) is never auto-mutated — warns + waits for a human, the fail-closed guard
    still blocking. A `moved`-guard (HEAD must advance) prevents a re-exec loop on a no-op ff. 8 bats cases
    (`tests/bats/merge_watcher.bats` "self-resync" — real-git drift/safety/ff + mocked orchestration).
    REMAINING: (1) the literal ops restart of the host daemon from a develop-current checkout is still a
    human action on the Windows box (can't be done from a Linux agent session) — but it is now self-healing
    on restart (the startup check resyncs/flags before the first poll). MINOR follow-up: full Windows
    daemon-CODE auto-refresh (without a manual restart) would need a launcher restart-loop + sentinel exit
    code instead of os.execv; deferred (the gate SCRIPTS — what actually decides merges — already self-heal
    on Windows via the ff-pull; only orchestrator-code improvements wait for the next restart).
  Cross-ref: postmortems.md 2026-06-19 PR #1428; merge-gates.sh MERGE_GATES_BLOCK_ALLOWLIST_RE + freshness
    guard (MERGE_GATES_FRESHNESS / MERGE_GATES_FRESH_RUN_BLOB / _DEV_BLOB); merge-watcher.py maybe_self_resync
    / detect_gate_logic_drift / _resync_safety / _ff_pull_develop / _reexec_daemon + daemon_loop startup +
    periodic hooks (MERGE_WATCH_AUTO_RESYNC / MERGE_WATCH_RESYNC_EVERY_CYCLES); merge-watcher.py _poll_run_gates
    env setdefault; Scheduled Task SmatchetMergeWatcher / run-merge-watcher.bat; merge-snapshots.jsonl:105;
    #1391 / docs/adr/0022-intent-gate-promotion.md; docs/agent-rules/merge-gates.md § Env knobs
    (MERGE_GATES_FRESHNESS + watcher-side complement);
    docs/self-improvement/categories/process/2026-06-19-intent-gate-bypassed-via-non-poller-merge.md (the
    sibling poller-bypass escape this is distinct from).
  Status: open (residual (1) = human ops restart on the Windows host; (2) shipped 2026-06-20)
  Last-reviewed: 2026-06-20
