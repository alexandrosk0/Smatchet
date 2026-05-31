# Perforce — operations runbook

> **Plan**: [`docs/plans/shipped/git-to-perforce-migration.md`](../plans/shipped/git-to-perforce-migration.md).
> **Sibling docs**: [`SETUP.md`](SETUP.md) — first-time bring-up. [`AGENT_FLOWS.md`](AGENT_FLOWS.md) — which verb to use when.
> **Audience**: operator running the Smatchet Perforce server day-to-day. Most agents never read this — `p4-janitor` ([`agents/core/p4-janitor.md`](../../agents/core/p4-janitor.md)) handles routine maintenance.

The Smatchet `p4d` server is **non-canonical** (git + GitHub remain the ship-line per [`AGENTS.md`](../../AGENTS.md) § Dual-VCS topology). Losing the depot is annoyance, not data loss — every shipped change lives on GitHub. The runbook below is therefore optimised for "keep agentic-WIP primitives healthy", not "prevent catastrophic data loss".

## Checkpoints + journal rotation

Default cadence: **once daily**, off-hours, on the host running `p4d`.

```powershell
# Run on the p4d server host as the account that owns the server root.
# Adjust SERVER_ROOT to match docs/perforce/SETUP.md § Decisions locked.
$env:P4ROOT = "C:\depot-smatchet"
& "C:\Program Files\Perforce\Server\p4d.exe" -r $env:P4ROOT -jc
```

`-jc` (journal-checkpoint) writes `checkpoint.<N>.gz` + `journal.<N-1>.gz` and rotates the live `journal` file. Idempotent; safe to re-run.

Schedule via Windows Scheduled Task (one-shot install — pure ops, not committed to repo):

```powershell
$action  = New-ScheduledTaskAction -Execute "C:\Program Files\Perforce\Server\p4d.exe" `
                                   -Argument "-r C:\depot-smatchet -jc"
$trigger = New-ScheduledTaskTrigger -Daily -At "3am"
Register-ScheduledTask -TaskName "Smatchet p4d checkpoint" -Action $action -Trigger $trigger `
                       -User "SYSTEM" -RunLevel Highest
```

**Offsite copy** (recommended, not blocking — depot is non-canonical):

```powershell
# Append to the scheduled-task action above, OR run as a second task at 3:30am:
robocopy "C:\depot-smatchet" "\\nas\smatchet-p4-backup" checkpoint.* journal.* /MIR /R:3 /W:5
```

## Verify archive integrity

Weekly cron via Scheduled Task or manual on demand:

```bash
p4 verify -q //smatchet/...
```

Output is empty on success. Any `BAD!` line = archive corruption — restore from the most recent good checkpoint (see § Recovery).

`p4-janitor` runs this on its own schedule per [`agents/core/p4-janitor.md`](../../agents/core/p4-janitor.md) § Verification.

## Stream + shelve GC

Smatchet allocates one task stream per parallel subagent. Streams accumulate; the GC script trims them.

```bash
# Dry-run first (shows which streams would be deleted; no mutation):
bash agents/scripts/project/p4-task-stream-gc.sh --older-than-days 14 --dry-run

# Real run:
bash agents/scripts/project/p4-task-stream-gc.sh --older-than-days 14
```

GC refuses to delete a stream that has pending CLs (any client has work in-flight against it). Resolve the pending CL first — either `p4 submit` it (if the work is wanted) or `p4 revert -c <CL>` then `p4 change -d <CL>` (if not).

Shelves are not auto-GC'd today. Inventory + manual prune:

```bash
p4 changes -s shelved -u <user>           # what's shelved
p4 shelve -d -c <CL>                      # delete a single shelf
```

`p4-janitor` shipped an end-of-session shelf-prune flow per its agent doc; this runbook covers the manual fallback.

## Recovery from checkpoint

Catastrophic loss (`db.*` files corrupted, disk failure on server host). Steps assume the most recent `checkpoint.<N>.gz` + every `journal.<M>.gz` for `M >= N` are recoverable.

1. Stop the `p4d` service: `Stop-Service p4d_smatchet` (or `taskkill /F /IM p4d.exe`).
2. Move the broken db files aside: `Move-Item "$P4ROOT\db.*" "$P4ROOT\db.broken\"`.
3. Restore from checkpoint:
   ```powershell
   & "C:\Program Files\Perforce\Server\p4d.exe" -r $P4ROOT -jr "$P4ROOT\checkpoint.<N>.gz"
   ```
4. Replay every journal since the checkpoint:
   ```powershell
   foreach ($j in (Get-ChildItem "$P4ROOT\journal.*.gz" | Sort-Object Name)) {
       & "C:\Program Files\Perforce\Server\p4d.exe" -r $P4ROOT -jr $j.FullName
   }
   ```
5. Restart the service: `Start-Service p4d_smatchet`.
6. Verify: `p4 info` succeeds + `p4 verify -q //smatchet/...` is silent.

If the offsite robocopy mirror has a more recent checkpoint than the local one, restore from there instead — same recipe with the mirror path.

## Common troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `p4 info` hangs or refuses connection | Service stopped or LAN partition | Re-check `p4d_smatchet` service on the server host; verify port 1666 reachable (`Test-NetConnection brick -Port 1666`). |
| `Library shutdown failed` or similar at `p4d` start | Earlier crash left lock files | Delete `$P4ROOT\db.lockfile`, retry. |
| `Submit failed: Out of disk space on server` | Server-root drive full | Free space OR roll log files; never delete `db.*` files manually. |
| Client `p4 reconcile` lists files the user didn't touch | `.p4ignore` missing or out of sync with `.gitignore` | Re-read [`AGENTS.md`](../../AGENTS.md) § Dual-VCS topology § Drift handling. Confirm `P4IGNORE=.p4ignore` is set; re-run `bash agents/scripts/project/p4-reconcile-check.sh` for the diff against git. |
| A file is `*exclusive*` and the owning client is dead | Stale `+l` lock | `p4 lock -r //path/to/file` from the server host as `super` user (irreversible — confirms abandonment). |
| `p4 counter --from --to` rejects valid CAS | Counter doesn't exist (first claim) | Bootstrap path in [`agents/scripts/core/lock-claim-p4.sh`](../../agents/scripts/core/lock-claim-p4.sh) handles this. Manual: `p4 counter <name> 0` then retry CAS. |

## Permissions + super user

The single dev user (`alexk`) is `super` by default for the local dev box. If/when a second contributor joins:

1. Create the user: `p4 user -f <login>`
2. Add to the `smatchet_devs` group: `p4 group smatchet_devs` (add to `Users:` list)
3. Grant write access via `p4 protect`:
   ```
   write   group   smatchet_devs   *   //smatchet/...
   list    group   smatchet_devs   *   //...
   ```

Super stays restricted to the host operator.

## Out of scope here

- **Helix Swarm** (code review): not deployed; PRs on GitHub remain the review surface.
- **Multi-server replication** (edge / replica nodes): single-server is fine for one developer; revisit when a second machine joins.
- **`p4 obliterate`** (history rewrite): never needed for the agentic-WIP use case. If a secret leaks into a CL, rotate the secret + flag the CL in the depot README; don't try to scrub history out of band with git.

## See also

- [`agents/core/p4-janitor.md`](../../agents/core/p4-janitor.md) — automated maintenance agent.
- [`agents/core/git-janitor.md`](../../agents/core/git-janitor.md) — git-side end-of-session cleanup (still load-bearing).
- [`docs/plans/shipped/git-to-perforce-migration.md`](../plans/shipped/git-to-perforce-migration.md) — the plan this runbook closes a gap on.
