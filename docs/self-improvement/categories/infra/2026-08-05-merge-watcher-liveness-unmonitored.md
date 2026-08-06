- 2026-08-05 · claude-code · [infra] · P2 — nothing detects a merge-watcher daemon that never starts: the installer has no post-install health check, and the only SessionStart health surface keys on daemon *output*, so daemon *absence* is silent indefinitely

  Surfaced while pruning the watcher registry. The `SmatchetMergeWatcher` scheduled
  task had been failing instantly on every trigger for days — `LastTaskResult: 1` on
  both the 2026-08-03 and the 2026-08-05 11:43 runs — because its action still
  `cmd /c`'d `%LOCALAPPDATA%\Smatchet\merge-watch\run-merge-watcher.bat`, a bridge
  file from the 2026-05-31 `infra.md` entry (now archived to `applied.md`) that no
  longer exists on the host. Two PRs sat registered the whole time. No session, no
  nudge, no log line said anything.

  Two independent holes, either of which alone would have caught it.

  **(1) The installer never verifies the task it just registered.**
  `agents/scripts/core/merge-watcher-install-autostart.ps1` pre-flights its
  *dependencies* carefully — it resolves and fails on missing `gh.exe` / `jq.exe` /
  `bash.exe` at install time rather than at first poll (`:48-82`) — and then
  registers the task and exits. It never runs it. A task whose action is wrong
  (stale path, missing interpreter, bad quoting) installs "successfully" and the
  first evidence is a poll that never happens.

  **(2) The health nudge cannot observe a daemon that never ran.**
  `agents/scripts/core/merge-watcher-stuck-nudge.sh` is the only SessionStart surface
  for watcher health. It reads the registry, then per registered PR opens
  `state_dir / f"{pr}.json"` and inspects `last_state` for
  `STUCK_NEEDS_ATTENTION`. Those per-PR files are written *by the daemon*. A daemon
  that never starts writes none, `last_state` resolves empty, nothing is stuck, and
  the nudge prints nothing. This is silence by construction, not a bug in the
  script — it is scoped to "a running daemon that is stuck", and the script's own
  header is explicit that it is advisory ("Advisory — never blocks. Exit 0 always").

  Confirmed empirically rather than inferred, against the genuinely-dead daemon:

      $ ls -la "$LOCALAPPDATA/Smatchet/merge-watch/"
      -rw-r--r-- 1 alexk 197609 257 Aug  5 14:34 active.json
      # no daemon.log, no per-PR <pr>.json — the daemon had written nothing, ever

      $ bash agents/scripts/core/merge-watcher-stuck-nudge.sh --nudge
      exit=0            # silent, with 2 PRs registered and the task dead since 8/3

  Blast radius is bounded but real: "Register with watcher" is one of the four
  post-ship options, and the ship-loop offers it as a terminal state. A user who
  picks it hands the PR to a component that may not exist, and gets no signal — the
  PR simply sits green and unmerged. Nothing corrupts; throughput silently stops.
  P2 not P1 because the failure is fail-safe (an unmerged PR, never a bad merge) and
  a human notices eventually.

  Proposed fix, cheapest first:

  1. **Post-install verification in the installer.** After `Register-ScheduledTask`,
     `Start-ScheduledTask`, poll `(Get-ScheduledTaskInfo).LastTaskResult` for a few
     seconds, and assert the log file exists and grew. Non-zero → print the action
     string and the log tail and exit non-zero. This alone converts the whole class
     from "silent for days" to "loud at install".
  2. **Teach the nudge about absence.** It already loads the registry; give it the
     complementary check — registry non-empty AND (no per-PR state file for a PR
     registered more than one poll-interval ago, OR `daemon.log` mtime older than a
     few intervals) → nudge "watcher registered N PRs but the daemon looks dead;
     re-run `merge-watcher-install-autostart.ps1`". Cheap, and it closes the
     *recurring* case (a daemon that dies later), which (1) cannot.
  3. Optional: have the daemon touch a heartbeat file each poll so (2) has one
     unambiguous freshness signal instead of inferring from per-PR state.

  Concrete next action: implement (2) in `merge-watcher-stuck-nudge.sh` plus a bats
  case (registry with one PR + no state files + stale/absent log → nudge fires,
  still exit 0), then (1) in the installer. Both are small; (2) is the one that
  generalizes, since (1) only covers the install instant.

  Immediate ops step, carried over from the now-archived 2026-05-31 `infra.md`
  entry: re-run
  `powershell -ExecutionPolicy Bypass -File agents/scripts/core/merge-watcher-install-autostart.ps1`
  on the Windows host (idempotent — unregisters, then re-registers with the canonical
  inline-cmd action), then `Start-ScheduledTask -TaskName SmatchetMergeWatcher` and
  tail the log. The bridge bat is not needed: #644 taught `_resolve_bin` to reject
  the WindowsApps shim as well as `system32\bash.exe`
  (`agents/scripts/core/merge-watcher.py:157`), which is the whole reason the bat
  existed. An agent session cannot register a Scheduled Task, so this step is human.

  Related, distinct — do not merge these:
  - `tooling/2026-06-19-merge-watcher-runs-stale-gate-logic` (archived →
    [`applied.md`](../applied.md)) — a daemon that *is* running enforcing stale gate
    logic; its `maybe_self_resync` fix shipped 2026-06-20. Its residual (1) is the
    same human-ops-restart friction; that entry is about wrong behaviour while alive,
    this one is about no behaviour at all.
  - `infra.md` 2026-05-30 P2 — the daemon's `clone_path` points at the shared
    integration tree `C:\Dev\Smatchet`, so it runs whatever branch that tree has
    checked out. Orthogonal to liveness, but it is the same host state, and both
    argue for the daemon reporting what it actually is at each poll.

  Status: open
  Last-reviewed: 2026-08-05
