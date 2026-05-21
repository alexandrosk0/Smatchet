# Smatchet agentic system — operator manual

What the system does, how to turn it on, and how to drive it day-to-day. No internals.

---

## What it does

Three automation loops on top of GitHub Issues + GitHub PRs:

1. **Triage** — polls your GitHub repo, asks an LLM to suggest actions (comment, label, assignee, state change, derived ticket, or "implement this issue"), drops them in a **proposals** panel for you to approve or reject.
2. **Handoff** — when you approve an *Implement* proposal, spawns a Claude coding session in an isolated worktree, opens a **draft PR**.
3. **React** — watches every open draft PR. CodeRabbit comments get auto-classified (invariant violations rejected with a cited reason; real findings spawn a fix). CI failures get auto-dispatched to the right specialist; transient flakes get re-run.

All three loops are **off by default**. You enable each one separately. The system **never auto-merges**. You always review the diff and click Merge in GitHub yourself.

---

## Prerequisites

- Smatchet built from a standard preset (agentic is on by default for standalone builds).
- `gh` CLI authenticated — `gh auth status` returns OK.
- `claude` CLI on `PATH`.
- GitHub Personal Access Token, scopes: `repo`, `issues` (add `actions:write` if you want manual workflow re-runs).
- `ANTHROPIC_API_KEY` set in the environment **before** you launch Smatchet (the spawned session inherits a clean env; if you set the key after launch it won't be visible — restart Smatchet).

---

## Turn it on

Open Smatchet → **Edit → Preferences → Agentic** tab.

### Triage (top section)

| Setting | Meaning |
|---|---|
| Enable scheduled agentic triage | Master toggle |
| Interval | 60–3600 seconds between polls (default 300) |
| Source | `github` (only option) |
| Query | `OWNER/REPO` of the repo to poll |
| GitHub PAT | Your token |

### Handoff

| Setting | Meaning |
|---|---|
| Auto-start handoff when an ImplementIssue proposal is approved | Off → you click [Start handoff] manually. On → approve = spawn immediately. |

### CodeRabbit react loop

| Setting | Meaning |
|---|---|
| Enable CodeRabbit react loop | Master toggle |
| Poll interval | 60–3600 s (default 1800 = 30 min) |
| Short-circuit-reject invariant-violating suggestions | Auto-reject CodeRabbit comments that violate project rules, with cited reason |
| Iteration budget per PR | Max auto-fixes per PR (default 5) |

### CI react loop

| Setting | Meaning |
|---|---|
| Enable CI react loop | Master toggle |
| Poll interval | 60–3600 s (default 600 = 10 min) |
| Auto-dispatch build-doctor on build failures | On by default |
| Auto-dispatch test-rig on coverage-gate failures | On by default |
| Auto-dispatch debug-detective on ctest failures | **Off** by default — behavioural fixes are riskier; opt-in |
| Re-run workflow on transient fingerprint match | On — auto-re-runs known-flaky CI |
| Transient reruns per PR | Default 2 |
| Iteration budget per PR | Default 5 |

Saving Preferences picks up changes without restarting Smatchet.

---

## Open the panels

Main menu → **Window**:

- **Agent proposals** — pending LLM-suggested actions, one row per proposal.
- **Agent handoffs** — in-flight Claude sessions, one row per spawned handoff.

---

## Review proposals

Each row in Agent proposals shows:

- Source issue (e.g. `owner/repo#123`)
- Proposed action — one of: Comment, Label add, Label remove, Assignee set, State transition, Derived ticket, **Implement issue**
- Rationale (free text from the LLM)
- Buttons: **[Approve]**, **[Reject]**, plus **[Start handoff]** on Implement rows

Approve = the action runs (a comment is posted, a label is set, etc.). Reject = nothing happens, row is dismissed.

For Implement rows, Approve also fires the handoff if you ticked the auto-start checkbox; otherwise click [Start handoff] when ready.

---

## Watch a handoff

Open Agent handoffs. The table shows the proposal id, current state, branch name, when it started, and row buttons:

- **[Cancel]** — abort the run.
- **[Open PR]** — appears once the draft PR is live.
- **[Open worktree]** — opens the local working directory.

Click a row to expand. The detail pane shows a live progress log, the current state, the last error (if any), and — when the harness needs input — a **clarification reply box** with a [Submit clarification] button.

States to know:

- **Pending → Spawning → Running** — normal startup path.
- **AwaitingUser** — the harness asked you a question. Type a reply and submit.
- **PrOpen** — draft PR is live, waiting for review or comments.
- **Iterating** — a comment landed on the PR and the harness is pushing a follow-up commit.
- **Complete** / **Failed** / **Cancelled** — terminal.

If clarification was requested, the same question is also posted as a comment on the originating GitHub issue. Whichever channel you answer first wins; the other becomes a no-op.

---

## Commands (if you'd rather skip the UI)

Run from any terminal where `Smatchet.exe` is on `PATH`.

### Triage

```bash
# Run triage against a whole repo (one-shot, ignores schedule):
Smatchet.exe cmd agent.triage.run --source github --query owner/repo --limit 30

# Run triage against a single issue:
Smatchet.exe cmd agent.triage.run --source github --issue owner/repo#123
```

### Handoff

```bash
Smatchet.exe cmd handoff.start    --proposal-id 42
Smatchet.exe cmd handoff.cancel   --proposal-id 42
Smatchet.exe cmd handoff.list
Smatchet.exe cmd handoff.clarify  --proposal-id 42 --answer "Use the smaller batch size."
Smatchet.exe cmd handoff.dry-run  --proposal-id 42        # preview without spawning
```

### CodeRabbit react

```bash
Smatchet.exe cmd coderabbit-react.start
Smatchet.exe cmd coderabbit-react.stop
Smatchet.exe cmd coderabbit-react.status
Smatchet.exe cmd coderabbit-react.poll-now
```

### CI react

```bash
Smatchet.exe cmd ci-react.start
Smatchet.exe cmd ci-react.stop
Smatchet.exe cmd ci-react.status
Smatchet.exe cmd ci-react.poll-now
Smatchet.exe cmd ci-react.rerun --pr-url <url> --workflow-id <id>    # needs actions:write
```

---

## Common workflows

### Triage tickets daily

1. Preferences → Agentic → paste PAT, paste `owner/repo`, set Interval to 1800.
2. Tick **Enable scheduled agentic triage**.
3. Walk away. Check the Agent proposals panel once a day; batch-approve or reject.

### Auto-implement approved feature requests

1. Do the triage workflow above.
2. Also tick **Auto-start handoff when an ImplementIssue proposal is approved**.
3. Approving an Implement row now spawns Claude and opens a draft PR within minutes.
4. Review the draft PR. Merge if good; comment if revisions needed (the react loop will push follow-up commits).

### Handle CodeRabbit feedback automatically

1. Preferences → CodeRabbit react loop → tick **Enable**.
2. Leave **Short-circuit-reject invariant-violating suggestions** on.
3. Leave iteration budget at 5.
4. Done. Every 30 min, new CodeRabbit comments on your open PRs are classified — invariant violations get auto-rejected with a cited reason; real findings spawn a fix.

### Auto-fix red CI when possible

1. Preferences → CI react loop → tick **Enable**.
2. Leave the build-doctor and test-rig auto-dispatches on.
3. Leave the debug-detective auto-dispatch **off** (default) unless you want every ctest failure to spawn an investigation.
4. Leave transient re-run on.
5. Done. Build failures and coverage-gate failures auto-spawn a specialist; known-flaky failures auto-re-run.

### Observation-only

Untick every **Enable** checkbox. Commands still work for manual one-shot probes; the scheduled worker stays idle.

---

## Troubleshooting

**Watcher isn't firing.** Check, in order: master toggle is on; `gh auth status` is OK; PAT has `repo` + `issues` scope; the `*.status` command returns `watcher_present: true`.

**Handoff keeps asking for clarification.** The harness can't infer something from the proposal. Either:

- Answer in the panel (most reliable),
- Comment on the originating GitHub issue (slower, but works),
- Or cancel and re-approve after editing the proposal's rationale.

**CodeRabbit reject reply doesn't resolve the thread.** Usually the PAT is missing `read:org` scope — reissue the token with `repo` + `read:org`. If the thread stays unresolved, click Resolve manually in GitHub; the auto-path is best-effort.

**Spawned harness can't find `ANTHROPIC_API_KEY`.** It must be set in the environment **before** Smatchet launches. If you set it after, restart Smatchet.

**Worktree directory growing.** Old worktrees stay on disk for inspection. Clean up when you're done:

```bash
git worktree remove -f -f .claude/worktrees/agent-42
git worktree prune
```

---

## What it won't do

- Merge a PR for you. Ever.
- React to PRs in other repos. Same-repo only.
- Watch via webhook. Polling only.
- Run two handoffs in parallel. One at a time.
- Auto-delete old worktrees. Manual cleanup.

For the "why" behind any of this, see the design docs under `docs/design/agentic-*.md` and the dev-detail manual at `docs/agentic/USAGE.md`.
