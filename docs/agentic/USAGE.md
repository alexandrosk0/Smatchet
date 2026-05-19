# Smatchet agentic system — usage manual

> Audience: Smatchet operator who has the app installed and wants to drive the agentic surface end-to-end. This document is "what + how"; the design rationale lives in the source plans linked below.
>
> Source design docs (read these for "why"):
> - [`docs/design/agentic-triage-flow.md`](../design/agentic-triage-flow.md) — issue triage half (T0-T9).
> - [`docs/design/agentic-coding-handoff.md`](../design/agentic-coding-handoff.md) — coding-harness handoff half (H0-H10).
> - [`docs/design/coderabbit-react-loop.md`](../design/coderabbit-react-loop.md) — PR-feedback + CI react loop (phases 1-9).
>
> Key agent files invoked by the system:
> - [`agents/handoff-implementer.md`](../../agents/handoff-implementer.md) — first delegate inside every spawned harness.
> - [`agents/pr-iterator.md`](../../agents/pr-iterator.md) — handles PR review-comment iterations on handoff PRs.
> - [`agents/coderabbit-triage.md`](../../agents/coderabbit-triage.md) — 18-rule override classifier for CodeRabbit feedback.
> - [`agents/build-doctor.md`](../../agents/build-doctor.md), [`agents/debug-detective.md`](../../agents/debug-detective.md), [`agents/test-rig.md`](../../agents/test-rig.md) — CI failure specialists.

---

## What this is

Smatchet ships three interlocking automation loops on top of GitHub Issues + GitHub PRs. From left to right:

```
  tracker repo (GitHub Issues)
            │
            │ 1. Triage loop
            │    Source_Core/src/AgenticTriageController.cpp polls every N
            │    seconds, sends each updated issue to an LLM, writes proposals
            │    (CommentAdd / LabelAdd / AssigneeSet / ImplementIssue / ...)
            │    into SQLite (agent_proposals).
            ▼
  Agent proposals panel  ─── user clicks [Approve] or [Reject] or [Start handoff]
            │
            │ 2. Handoff loop
            │    When an ImplementIssue proposal is approved (manually or via
            │    cfg.HandoffAutoStartOnApprove), AgenticHandoffController spawns
            │    `claude --print` inside an isolated git worktree on a fresh
            │    branch, watches it write sentinel files (SEED.* → RUN_RESULT.json),
            │    opens a draft PR.
            ▼
  Draft PR opened on GitHub
            │
            │ 3. React loop
            │    OpenPrRegistrar (Tick()) lists every open PR on develop. The
            │    PrCommentWatcher polls each PR's review comments (handles
            │    CodeRabbit), and PrCheckRunWatcher polls each PR's check-runs
            │    (handles CI). Both can dispatch a spawn into a per-PR ad-hoc
            │    worktree to land an iteration commit.
            ▼
  Iteration commits land on the PR branch; user reviews + merges manually.
```

All three loops are **disabled by default**. The user enables them via Preferences → Agentic, or via the corresponding `*.start` command.

The shipped system never auto-merges PRs. The user always reviews the diff and clicks "Merge" in the GitHub UI (or runs `gh pr merge` themselves).

### Who this is for

A Smatchet operator who:

- Has Smatchet built with `SMATCHET_WITH_AGENTIC=ON` (the default for `ninja-iter-msys2` / `ninja-debug-msys2` / `ninja-publish-msys2`; OFF for `*_DX12` Unreal targets).
- Has a GitHub Personal Access Token with `repo` + `issues` scope (and `actions:write` for `ci-react.rerun`).
- Has the `claude` CLI on `PATH` (or an absolute path persisted in `cfg.HandoffHarnessBinPath`).
- Has `gh` authenticated (`gh auth status` returns OK).

If any of those are missing, the corresponding loop short-circuits cleanly — no crashes, no silent skipped work, just a friendly "configure cfg.X first" message.

### Prerequisites checklist

- Smatchet built with `SMATCHET_WITH_AGENTIC=ON` (the default for every standalone preset)
- `gh` CLI authenticated (`gh auth status` returns OK)
- `claude` CLI on `PATH` (or absolute path in `cfg.HandoffHarnessBinPath`)
- GitHub PAT pasted into Preferences → Agentic → "GitHub PAT" (scopes: `repo`, `issues`; plus `actions:write` if you'll use `ci-react.rerun`)
- `ANTHROPIC_API_KEY` set in the parent process's environment (it's the only Smatchet-side secret the spawned harness inherits)

---

## One-time setup

### 1. Build with the agentic surface enabled

The agentic feature compiles only when `SMATCHET_WITH_AGENTIC` is defined. This is **on by default** for every standalone preset:

| Preset | `SMATCHET_WITH_AGENTIC` |
|---|---|
| `ninja-iter-msys2` | ON |
| `ninja-debug-msys2` | ON |
| `ninja-publish-msys2` | ON |
| `ninja-test-msys2` | ON |
| `ninja-ui-test-msys2` | ON |
| any `*_DX12` (Unreal-embedded) | OFF |

Default build invocation:

```bash
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone
```

The resulting exe at `build/ninja-iter-msys2/Smatchet.exe` ships every agentic feature documented below.

### 2. Authenticate `gh`

```bash
gh auth status
# Expected: "Logged in to github.com as <username>"
# Token scopes must include: repo, read:org
```

If you intend to use `ci-react.rerun` (manual transient-rerun without spawn), the token also needs `actions:write` (i.e. `workflow` scope).

### 3. Place the `claude` CLI on `PATH`

The handoff controller resolves the binary in this order:

1. `cfg.HandoffHarnessBinPath` — absolute path persisted in `smatchet_config.json`. Set this if you have multiple Claude Code installs.
2. `PATH` lookup for `claude` (the default for a normal install).

`Source_Core/include/ConfigManager.h:246`:

```cpp
std::string HandoffHarnessBinPath;
std::string HandoffRunnerName = "claude-code-local";
```

The runner name is `claude-code-local` — the only `ICodingHarnessRunner` implementation today. Future Codex / Aider runners would override the name; today no other strings work.

### 4. Set `ANTHROPIC_API_KEY`

The spawned `claude` child inherits a fresh environment with **only** these variables:

```text
PATH, HOME, USER, USERPROFILE, TEMP, TMP, SYSTEMROOT,
GH_TOKEN, GITHUB_TOKEN, ANTHROPIC_API_KEY
```

In particular, **no `SMATCHET_*` env vars inherit** — Smatchet config and any secrets in `SMATCHET_*` stay in the parent. Set `ANTHROPIC_API_KEY` in your shell profile, in Windows env vars, or in a `.envrc`. (See [`AGENTS.md` § Handoff envelope § Env allow-list](../../AGENTS.md#env-allow-list).)

### 5. Configure Smatchet's Agentic Preferences tab

Open Smatchet → `Edit` → `Preferences…` → `Agentic` tab. The tab is hidden when `SMATCHET_WITH_AGENTIC=OFF`.

**Top section — Scheduled agentic triage:**

| Label (verbatim) | Backing field | Default |
|---|---|---|
| "Enable scheduled agentic triage" | `cfg.AgenticPollEnabled` | false |
| "Interval:" + "seconds (60..3600)" | `cfg.AgenticPollIntervalSec` | 300 |
| "Source:" (combo, disabled — only `github` today) | `cfg.AgenticPollSource` | `"github"` |
| "Query:" + "For github: OWNER/REPO of the repository to poll" | `cfg.AgenticPollQuery` | `""` |
| "GitHub PAT:" + "Bearer token - needs `repo` + `issues` scope" | `cfg.GitHubPat` | `""` |

**Handoff (Implement) section:**

| Label (verbatim) | Backing field | Default |
|---|---|---|
| "Auto-start handoff when an ImplementIssue proposal is approved" | `cfg.HandoffAutoStartOnApprove` | false |

**CodeRabbit react loop section:**

| Label (verbatim) | Backing field | Default |
|---|---|---|
| "Enable CodeRabbit react loop" | `cfg.CoderabbitReact.Enabled` | false |
| "Poll interval (sec):" (slider 60..3600) | `cfg.CoderabbitReact.PollIntervalSec` | 1800 |
| "Bot logins (read-only):" | `cfg.CoderabbitReact.BotLogins` | `["coderabbitai[bot]"]` |
| "Short-circuit-reject invariant-violating suggestions" | `cfg.CoderabbitReact.ShortCircuitRejectEnabled` | true |
| "Iteration budget per PR:" (slider 1..50) | `cfg.CoderabbitReact.IterationBudgetPerPr` | 5 |

**CI react loop section:**

| Label (verbatim) | Backing field | Default |
|---|---|---|
| "Enable CI react loop" | `cfg.CiReact.Enabled` | false |
| "Poll interval (sec):" (slider 60..3600) | `cfg.CiReact.PollIntervalSec` | 600 |
| "Auto-dispatch build-doctor on build failures" | `cfg.CiReact.AutoDispatchBuildDoctor` | true |
| "Auto-dispatch test-rig on coverage-gate failures" | `cfg.CiReact.AutoDispatchTestRig` | true |
| "Auto-dispatch debug-detective on ctest failures" | `cfg.CiReact.AutoDispatchDebugDetective` | false |
| "Re-run workflow on transient fingerprint match" | `cfg.CiReact.TransientRerunEnabled` | true |
| "Transient reruns per PR:" (slider 0..10) | `cfg.CiReact.TransientRerunMaxPerPr` | 2 |
| "Iteration budget per PR:" (slider 1..50) | `cfg.CiReact.IterationBudgetPerPr` | 5 |

All checkbox flips that touch the master `Enabled` toggle call `AppController::RestartAgenticPollAsync()` so the scheduled-poll worker picks up the change without an app restart. Other field edits debounce-save and apply on next tick.

### 6. Pick the menu items

Both panels are toggleable from Smatchet's main menu (Window menu):

- "Agent proposals" — proposals panel
- "Agent handoffs" — in-flight handoffs panel

Source: [`Source_Core/src/SmatchetUI_MainMenu.cpp:346-352`](../../Source_Core/src/SmatchetUI_MainMenu.cpp).

---

## The three loops at a glance

| Loop | Trigger | Owner class | Output |
|---|---|---|---|
| **Triage** | Scheduled poll (interval `cfg.AgenticPollIntervalSec`) **or** `agent.triage.run` command | `AgenticTriageController` (`Source_Core/src/AgenticTriageController.cpp`) | Pending rows in `agent_proposals` SQLite table |
| **Handoff** | Approving an `ImplementIssue` row in the proposals panel (auto if `cfg.HandoffAutoStartOnApprove`, otherwise via `[Start handoff]` button or `handoff.start` command) | `AgenticHandoffController` (`Source_Core/src/AgenticHandoffController.cpp`) + `ClaudeCodeLocalRunner` (`Source_Core/src/ClaudeCodeLocalRunner.cpp`) | Draft PR opened on GitHub; row appears in the handoffs panel; FSM-tracked via `HarnessRunState` |
| **React** | The same scheduled-poll worker ticks `OpenPrRegistrar::Tick()` → `PrCommentWatcher::Tick()` + `PrCheckRunWatcher::Tick()` when either of `cfg.CoderabbitReact.Enabled` / `cfg.CiReact.Enabled` is true | `PrCommentWatcher` + `PrCheckRunWatcher` (`Source_Core/src/PrCommentWatcher.cpp`, `PrCheckRunWatcher.cpp`) | Posted PR replies, resolved review threads, ad-hoc spawns into `.claude/worktrees/coderabbit-pr<N>` |

All three loops share **one scheduled-poll worker thread** owned by `AppController`. Calling `RestartAgenticPollAsync()` recycles it; the watchers themselves expose only `Tick()` — they hold no thread state of their own. This keeps thread count flat regardless of how many surfaces light up.

---

## Triage flow — daily use

### Configure

In Preferences → Agentic (top section):

1. Paste a PAT into "GitHub PAT" (scopes: `repo`, `issues`).
2. Paste `OWNER/REPO` into "Query" (e.g. `alexandrosk0/Smatchet`).
3. Leave "Source" on `github` (only option today).
4. Pick an "Interval" — 300 s (5 min) default, 60 s minimum, 3600 s maximum.
5. Tick "Enable scheduled agentic triage".

Saving Preferences automatically triggers `RestartAgenticPollAsync()`. The next tick fires within `cfg.AgenticPollIntervalSec` seconds. To poll right now, the worker exposes a one-shot path — click the "Run triage now" button in the Agentic Preferences tab (it calls `AppController::RunAgenticTriageOnce` synchronously on a worker thread).

### What happens each tick

```
SchedPoll worker thread:
    │
    ▼
GetGitHubReadClient().ListOpenIssuesForRepo(query, since=cursor)
    │  (cursor stored in agent_poll_cursor SQLite table — wall-clock)
    ▼
for each returned issue:
    AgenticInferenceClient.Run(Ollama POST /api/chat, schema-validated)
    │
    ▼
    AgentProposalStore.Append({sourceIssueKey, action, payload, rationale,
                               status=Pending, createdAt=now})
    │
    ▼  (audit-trail row written via BackendAuditTrail.AppendEvent)
SQLite agent_proposals row visible to the UI
```

A single tick may insert zero or many proposals per issue depending on what the LLM produces. The `AgenticInferenceClient` validates each draft against the JSON schema in `AgenticInferenceClientPure.h` (the `ProposedAction` enum) and drops malformed entries with a `bad_inference` audit-trail row instead of crashing.

### Reviewing pending proposals

Open Smatchet → Window → "Agent proposals".

Each row shows:

- Source issue key (e.g. `alexandrosk0/Smatchet#123`)
- Proposed action — one of the `ProposedAction` enum values below
- Rationale (free text from the LLM)
- `[Approve]`, `[Reject]`, and (for `ImplementIssue` rows only) `[Start handoff]` buttons

The `ProposedAction` enum (canonical source: [`Source_Core/include/AgenticInferenceClientPure.h:23`](../../Source_Core/include/AgenticInferenceClientPure.h)):

```cpp
enum class ProposedAction {
    Unknown,
    CommentAdd,
    LabelAdd,
    LabelRemove,
    AssigneeSet,
    StateTransition,
    DerivedTicketCreate,
    ImplementIssue,
};
```

**Approving** transitions the row from `Pending → Approved` and (for non-`ImplementIssue` actions) applies the action via `GitHubClient`. For `ImplementIssue` rows, approval **also** fires `OnProposalApproved` which (when `cfg.HandoffAutoStartOnApprove=true`) auto-starts the handoff. The default is **false** — you click `[Start handoff]` explicitly to begin work.

**Rejecting** transitions the row to `Rejected` and writes a `BackendAuditTrail` row. No GitHub call fires.

### What each action does on Approve

| Action | Apply effect |
|---|---|
| `CommentAdd` | Posts a plain-text comment via `GitHubClient::AddIssueCommentPlain` |
| `LabelAdd` | `PATCH /repos/{owner}/{repo}/issues/{n}` with the added label |
| `LabelRemove` | `PATCH …` with the removed label |
| `AssigneeSet` | `PATCH …` with the new assignee |
| `StateTransition` | `PATCH …` with the state change (typically open/closed) |
| `DerivedTicketCreate` | Creates a derived ticket in the cross-source tracker (Jira/Plane) named in `payload` |
| `ImplementIssue` | **Triggers handoff** — see § Handoff flow below |

The proposals panel is the single place to see what the LLM thinks should happen. **You always pull the trigger.**

### Triage commands (CLI / MCP / Lua)

Only one triage command ships in the current build:

```bash
# Run triage against an entire OWNER/REPO (batch mode):
Smatchet.exe cmd agent.triage.run --source github --query alexandrosk0/Smatchet --limit 30

# Run triage against a single issue:
Smatchet.exe cmd agent.triage.run --source github --issue alexandrosk0/Smatchet#123
```

Parameters (from [`BuiltinCommands_Agentic.cpp`](../../Source_Core/src/Commands/Builtin/BuiltinCommands_Agentic.cpp)):

| Param | Type | Required | Notes |
|---|---|---|---|
| `--source` | string (enum: `github`) | yes | Only `github` is supported in this slice |
| `--query` | string | one of `--query` / `--issue` | `OWNER/REPO` for batch triage |
| `--issue` | string | one of `--query` / `--issue` | `OWNER/REPO#N` for a single-issue run |
| `--limit` | int | optional (default 30) | Per-batch cap, matches GitHub `per_page` |

The command is `Destructive = false` for reads but `Idempotent = false` because each run inserts a fresh batch.

> **No `agent.triage.poll.start` / `.stop` commands ship today.** The scheduled poll is config-driven only — flip `cfg.AgenticPollEnabled` via Preferences or by hand-editing `smatchet_config.json`, then restart the worker via Preferences (auto) or by calling `AppController::RestartAgenticPollAsync` from an MCP / Lua context.

---

## Handoff flow — driving the harness

### Trigger paths

1. **Auto-start on approve** — `cfg.HandoffAutoStartOnApprove = true` + click `[Approve]` on an `ImplementIssue` proposal row.
2. **Manual button** — `cfg.HandoffAutoStartOnApprove = false` (the shipped default) + click `[Start handoff]` on an approved `ImplementIssue` row.
3. **CLI** — `Smatchet.exe cmd handoff.start --proposal-id <id>`.

The proposal must be of action `ImplementIssue`; other actions never trigger a spawn. The `[Start handoff]` button is rendered **only** on `ImplementIssue` rows.

### What gets created

```
.claude/worktrees/agent-<proposalId>/         ← worktree root (gitignored)
    ├── SEED.md                                ← written by runner before spawn
    ├── SEED.json                              ← canonical seed payload
    ├── .progress.log                          ← live tail surfaced in UI
    └── (during run) CLARIFICATION_NEEDED.json ← if harness blocks on a question
                     USER_RESPONSE.json        ← when you answer
    └── (on success) PR_URL.txt                ← single-line PR URL
                     RUN_RESULT.json           ← terminal signal (last write)
```

Branch name: `agent/<proposalId>/<short-slug>` where `<short-slug>` is the first 32 characters of `kebab-case(issueTitle)`. The runner asserts that the branch is **not** `develop` or `main` before `git worktree add` runs.

Per [`AGENTS.md` § Handoff envelope § Branch naming](../../AGENTS.md#branch-naming) and [`AGENTS.md` § Worktree layout](../../AGENTS.md#worktree-layout).

### The handoffs panel

Open Smatchet → Window → "Agent handoffs". The panel has two halves:

**Top (table):**

| Column | Content |
|---|---|
| proposal-id | SQLite ROWID of the originating proposal |
| state | One of the 9 `HarnessRunState` values (see FSM diagram below) |
| branch | `agent/<id>/<slug>` |
| started | Wall-clock timestamp of `Start()` |
| (per-row actions) | `[Cancel]` (when Cancellable), `[Open PR]` (when `PR_URL.txt` exists), `[Open worktree]` |

**Bottom (detail):** click a row to expand. Shows:

- `.progress.log` tail (read-only multi-line text)
- Current state name
- Last-error string (if any)
- Recent events (compact metadata block — worktree path, branch, started, iteration count)
- When state is `AwaitingUser`: an `InputTextMultiline` reply box + `[Submit clarification]` button

UI strings (from `Source_Core/src/SmatchetAgentHandoffUi.cpp`, localized via `agent.handoffs.*` keys):

- "Cancel", "Open PR", "Submit clarification"
- "Agent question:" / "Your reply:"
- "Last error", "Recent events:"
- "Handoff" (detail heading)

### The 9-state FSM — `CodingHarness::RunState`

Canonical source: [`Source_Core/include/HarnessRunState.h:36`](../../Source_Core/include/HarnessRunState.h).

```
                  ┌───────────┐
   Start() ──▶    │ Pending   │
                  └─────┬─────┘
                        ▼
                  ┌───────────┐
                  │ Spawning  │
                  └─────┬─────┘
                        ▼
                  ┌───────────┐ ◀──── (answer arrives)
              ┌──▶│ Running   │
              │   └──┬────┬───┘
              │      │    │
       (need  │      │    │ (PR opened)
        info) │      ▼    ▼
              │   AwaitingUser     ┌─────────┐
              │      │             │ PrOpen  │ ◀────┐
              │      │             └───┬─────┘     │
              └──────┘                 │           │ (push iteration)
                                       ▼           │
                                  ┌─────────┐      │
                                  │Iterating│──────┘
                                  └────┬────┘
                                       │
                                       ▼
                          ┌────────────┴────────────┐
                          ▼                         ▼
                     ┌─────────┐               ┌──────────┐
                     │Complete │               │  Failed  │
                     └─────────┘               └──────────┘
                          (and: Cancelled — terminal from any non-terminal state)
```

Terminal states (`Complete`, `Failed`, `Cancelled`) reject all outbound transitions, including self-loops. Every state-name string the runner emits via stream-json passes through `IsTransitionAllowed` at the controller boundary — disallowed transitions log `LOG_WARN` and are dropped. This is the FSM integrity boundary; see [`AGENTS.md` § Handoff envelope § Anti-deception note](../../AGENTS.md#anti-deception-note).

### Clarification flow

When the spawned harness writes `CLARIFICATION_NEEDED.json`, the controller transitions the state to `AwaitingUser`. The panel surfaces:

```
Agent question: <question text from JSON>
Your reply:     [text input ............]
                [Submit clarification]
```

Click `[Submit clarification]` (or invoke `handoff.clarify --proposal-id <id> --answer <text>`) — the controller writes `USER_RESPONSE.json` into the worktree, the harness reads it on `claude --resume`, and the state transitions back to `Running`.

If `cfg.HandoffClarificationPostToGithub=true` (the default), the same question is **also** posted to the originating GitHub issue as a bot-marked comment. The scheduled-poll worker scans that issue for non-bot replies; whichever channel the user answers first wins. The other answer becomes a no-op (idempotency lock keyed on proposalId).

### PR-open path

When the harness finishes implementing the change, it either:

1. **Writes `PR_URL.txt`** itself after running `git push` + `gh pr create --draft`. The controller transitions to `PrOpen`.
2. **Doesn't write it** (older / minimal harness). Then `cfg.HandoffAutoCreatePrIfMissing=true` (the default) makes the runner do `git push -u origin <branch>` + `gh pr create --draft --base <cfg.HandoffPrBaseBranch>` (default `develop`) itself and writes the URL back to the worktree.

The PR is **always opened as `--draft`**. You mark it ready-for-review after auditing the diff. The spawned child **never** calls `gh pr ready`, `gh pr merge`, or closes/reopens PRs (per [`AGENTS.md` § Spawned-child PR draft requirement](../../AGENTS.md#spawned-child-pr-draft-requirement)).

### PR-iteration loop

While the PR sits in `PrOpen`, the scheduled-poll worker keeps ticking `PrCommentWatcher::Tick()`. When a new non-bot comment lands (`agents/pr-iterator.md` triage rule), the watcher:

1. Increments `agent_pr_watch.iteration_count` (capped at `cfg.HandoffPrIterationBudget`, default 10).
2. Dispatches `ClaudeCodeLocalRunner::SpawnAdHoc(...)` with the comment body + PR diff.
3. The harness pushes an iteration commit to the same branch.
4. State transitions `PrOpen → Iterating → PrOpen`.

When the budget is exhausted, the watcher posts a "iteration budget exhausted, handing back to user" PR comment and stops dispatching further. The handoff stays in `PrOpen` for manual handoff.

### Handoff commands

Canonical source: [`Source_Core/src/Commands/Builtin/BuiltinCommands_Handoff.cpp`](../../Source_Core/src/Commands/Builtin/BuiltinCommands_Handoff.cpp).

| Command | Destructive | Description |
|---|---|---|
| `handoff.start --proposal-id <id> [--dry-run]` | yes | Begin a handoff. With `--dry-run`, returns computed branch + worktree without spawning. |
| `handoff.cancel --proposal-id <id>` | yes | Raise the cancel atom. Runner honours within seconds; controller transitions to `Cancelled`. |
| `handoff.list` | no | Snapshot every in-flight `ActiveHandoff` as JSON. |
| `handoff.clarify --proposal-id <id> --answer <text>` | yes | Write `USER_RESPONSE.json` into the worktree (non-UI clarification path). |
| `handoff.dry-run --proposal-id <id> [--use-stub-claude]` | no | Developer convenience — returns the would-be branch + worktree. The `--use-stub-claude` flag is reserved for the stub-claude end-to-end smoke test. |

Example invocations:

```bash
# Inspect a proposal's would-be handoff before committing:
Smatchet.exe cmd handoff.dry-run --proposal-id 42

# Start the handoff:
Smatchet.exe cmd handoff.start --proposal-id 42

# Watch the in-flight set:
Smatchet.exe cmd handoff.list

# Cancel if it's gone off the rails:
Smatchet.exe cmd handoff.cancel --proposal-id 42

# Answer a clarification without opening the UI:
Smatchet.exe cmd handoff.clarify --proposal-id 42 --answer "Use the smaller batch size."
```

> **Not shipped in this build:** `handoff.show`, `handoff.gc`. The plan named them; the shipped CLI surface is the five commands above. For garbage-collection, use `git worktree remove -f .claude/worktrees/agent-<id>` directly.

### Sentinel file vocabulary (handoff path)

Files written by either the runner or the spawned harness, living at the worktree root. All gitignored.

| File | Writer | Reader | When | Role |
|---|---|---|---|---|
| `SEED.md` | runner | handoff-implementer | Before spawn | Human-readable handoff brief; weaves into commit message + PR body |
| `SEED.json` | runner | handoff-implementer | Before spawn | Canonical seed payload (proposal payload + issue body + comments + repo pointers + `dispatch_source` discriminator) |
| `.progress.log` | harness (via `scripts/dev/agent-progress.sh`) | UI panel | Continuously during spawn | Live progress lines tailed in the panel |
| `CLARIFICATION_NEEDED.json` | handoff-implementer | runner + UI | When harness blocks | Single question; harness halts until answered |
| `USER_RESPONSE.json` | runner / UI / GitHub-comment poller | handoff-implementer | When user answers | Rewritten per clarification round |
| `RUN_RESULT.json` | handoff-implementer | runner | Last write before exit | `{ ok, errorMessage, prUrl, filesChanged, linesAdded, linesRemoved, toolUseSummary }` |
| `PR_URL.txt` | handoff-implementer | runner + UI | After `gh pr create` | Single line containing the PR URL |
| `CHECK_RUN.json` | classifier (CI react path only) | handoff-implementer | Before spawn, only for `dispatch_source == ci_*` | CI-failure payload — check-run name + conclusion + top-N annotations + last-N log lines |

Write-once semantics: every sentinel except `USER_RESPONSE.json` is written once per spawn. `RUN_RESULT.json` is the **last** write before exit. Canonical contract in [`AGENTS.md` § Handoff envelope § Sentinel files](../../AGENTS.md#sentinel-files).

The `dispatch_source` discriminator on `SEED.json` (added in phase 7 of the react loop) is one of:

```text
proposal_implement    ← classic handoff from an approved ImplementIssue proposal
coderabbit_comment    ← CodeRabbit PR comment classified as actionable
ci_build_failure      ← cmake / link failure on build-and-test
ci_ctest_failure      ← ctest assertion / sanitizer hit
ci_coverage_gate      ← missing paired-test delta
ci_transient_rerun    ← (no spawn — handled by `gh workflow run`)
```

`handoff-implementer` reads `dispatch_source` and routes to the right specialist (`coderabbit-triage`, `build-doctor`, `debug-detective`, `test-rig`). Per [`AGENTS.md` § First-delegate selection](../../AGENTS.md#first-delegate-selection).

---

## React loop — CodeRabbit feedback

### Enable

Two paths:

1. **Preferences UI** → Agentic → "CodeRabbit react loop" → tick "Enable CodeRabbit react loop".
2. **Command**: `Smatchet.exe cmd coderabbit-react.start`.

Both persist `cfg.CoderabbitReact.Enabled = true` and call `RestartAgenticPollAsync()`. The next tick (default every 1800 s = 30 min) picks up the change.

### What gets polled

Every open PR on `develop` (configurable: `cfg.CoderabbitReact.WatchedBaseBranches`, default `["develop"]`) appears as a row in either `agent_pr_watch` (handoff-origin PRs — keyed on `proposal_id`) or `agent_open_pr_watch` (hand-pushed PRs — keyed on `pr_url`). The two tables together drive the watcher; `OpenPrRegistrar::Tick()` keeps them populated by calling `gh pr list --base develop --state open` once per scheduled-poll iteration.

For each row, `PrCommentWatcher::Tick()`:

1. Fetches review-thread comments via `GitHubClient::FetchPrComments(prNumber)`.
2. Advances the per-row `last_seen_comment_id_str` cursor.
3. Filters new comments through the registered `PrCommentClassifier` chain.

### The 18-rule override classifier

`CoderabbitCommentClassifier` (`Source_Core/src/CoderabbitCommentClassifier.cpp`) consumes the 18-rule override table embedded in [`agents/coderabbit-triage.md`](../../agents/coderabbit-triage.md). Each rule encodes a Smatchet invariant that CodeRabbit sometimes suggests violating:

- Use `std::optional` instead of `nullptr` — **reject** (C++14 hard ban)
- Use structured bindings — **reject** (C++14 hard ban)
- Use `if constexpr` — **reject** (C++14 hard ban)
- Add GLFW/OpenGL include to `Source_Core/` header — **reject** (DX12 dual-target violation)
- Use `printf` / `std::cerr` — **reject** (use `LOG_*` macros)
- Raw `new` / `delete` outside documented edge cases — **reject** (RAII)
- (… and 12 more — read the table in `agents/coderabbit-triage.md`.)

For each comment, the classifier returns one of:

| Verdict | Action |
|---|---|
| `RejectShortCircuit` | Post a reply citing the override rule number + resolve the GraphQL review thread via `GitHubClient::ResolveReviewThread`. No spawn. |
| `Dispatch` | Spawn `handoff-implementer` (routed to `coderabbit-triage`) inside `.claude/worktrees/coderabbit-pr<N>` with a fresh iteration branch `coderabbit/pr<N>/iter<n>`. |
| `Skip` | Log + advance cursor. Used for bot comments not on the allow-list or comments the classifier doesn't recognise. |

### Dispatch path details

When the classifier returns `Dispatch`, the watcher invokes `ClaudeCodeLocalRunner::SpawnAdHoc(req)` with:

- `prUrl` = the canonical PR URL
- `headRefName` = the PR's head ref (e.g. `feature/X`)
- `dispatchSource = "coderabbit_comment"`
- `targetAgent = "handoff-implementer"` (always)
- `routedDelegate = "coderabbit-triage"`
- `iteration` = current `agent_open_pr_watch.iteration_count` for that PR

The runner creates `.claude/worktrees/coderabbit-pr<N>` (one worktree per PR, shared between CodeRabbit and CI dispatches) on a fresh branch `coderabbit/pr<N>/iter<n>` off `origin/<headRefName>`. SEED.md opens with `## First delegate: handoff-implementer` + `## Routed via: coderabbit-triage`. The harness picks up the routing, applies the fix, runs the slice-boundary `cmake --build` + `scripts/dev/test-all.sh`, pushes a commit, exits.

### Thread-resolve via GraphQL

After posting a reject reply, the watcher chains:

1. `GitHubClient::LookupReviewThreadIdForComment(commentId)` — GraphQL probe.
2. `GitHubClient::ResolveReviewThread(threadId)` — GraphQL mutation.

This is essential because the sibling merge-gates plan (`docs/design/merge-gates-ci-coderabbit-comments.md`) blocks squash-merge until **zero unresolved non-outdated review threads with a `coderabbitai` comment**. Without the resolve, the merge gate would stay blocked indefinitely after every short-circuit reject.

PAT scopes required: `repo` + `read:org` (per `GitHubClient.cpp`).

### CodeRabbit react commands

Canonical source: [`Source_Core/src/Commands/Builtin/BuiltinCommands_Coderabbit.cpp`](../../Source_Core/src/Commands/Builtin/BuiltinCommands_Coderabbit.cpp).

| Command | Destructive | Description |
|---|---|---|
| `coderabbit-react.start` | yes | Persist `cfg.coderabbit_react.enabled=true` + restart the poll worker |
| `coderabbit-react.stop` | yes | Persist `cfg.coderabbit_react.enabled=false` + restart |
| `coderabbit-react.status` | no | Print runtime state — interval, bot allow-list, iteration budget, last-tick timestamp |
| `coderabbit-react.poll-now [--pr-url <url>]` | yes | Fire one tick right now (synchronous; `--pr-url` is reserved — phase-8 watcher walks every row regardless) |

Example:

```bash
Smatchet.exe cmd coderabbit-react.start
Smatchet.exe cmd coderabbit-react.status
# Returns JSON: {enabled, poll_interval_sec, watched_base_branches, bot_logins,
#                short_circuit_reject_enabled, auto_dispatch_fixes,
#                iteration_budget_per_pr, ad_hoc_worktree_root,
#                last_tick_at_sec, watcher_present, watcher_iteration_budget}
```

### Tunables (config block)

`cfg.CoderabbitReact` ([`Source_Core/include/ConfigManager.h:338`](../../Source_Core/include/ConfigManager.h)):

| Field | Default | Notes |
|---|---|---|
| `Enabled` | false | Master toggle |
| `PollIntervalSec` | 1800 | Clamped 60..3600 |
| `WatchedBaseBranches` | `["develop"]` | OpenPrRegistrar query base |
| `BotLogins` | `["coderabbitai[bot]"]` | Comment-author allow-list |
| `ShortCircuitRejectEnabled` | true | False = log only, no auto-reject |
| `AutoDispatchFixes` | true | False = read-only mode (cursor advances, no spawns) |
| `IterationBudgetPerPr` | 5 | Clamped 1..50 |
| `AdHocWorktreeRoot` | `".claude/worktrees"` | Worktree namespace |

---

## React loop — CI failures

### Enable

Same two paths as CodeRabbit:

1. **Preferences UI** → Agentic → "CI react loop" → tick "Enable CI react loop".
2. **Command**: `Smatchet.exe cmd ci-react.start`.

The same scheduled-poll worker ticks `PrCheckRunWatcher::Tick()` (separate from `PrCommentWatcher::Tick()`) when either loop is enabled.

### What gets watched

For every PR row in `agent_pr_watch` ∪ `agent_open_pr_watch`:

1. `GitHubClient::FetchCheckRuns(headSha)` returns the check-runs for the head commit.
2. The watcher advances `last_seen_check_run_id`.
3. For each failed check-run **whose name is in `cfg.CiReact.WatchedCheckNames` and NOT in `cfg.CiReact.IgnoredCheckNames`**, the watcher fetches annotations (`FetchCheckRunAnnotations`) + log tail (`FetchActionsJobLogs`, capped by `LogTailLines`).
4. The payload is run through `CiFailureClassifier`.

### Classifier categories

Canonical source: [`Source_Core/src/CiFailureClassifier.cpp`](../../Source_Core/src/CiFailureClassifier.cpp).

| Verdict | When | Dispatch |
|---|---|---|
| `ci_build_failure` | check-run name in `WatchedCheckNames` + cmake / link / MSYS2 / lld fingerprint hit in annotations | `build-doctor` (if `AutoDispatchBuildDoctor=true`) |
| `ci_ctest_failure` | check-run name in `WatchedCheckNames` + ctest-assertion / sanitizer-hit fingerprint | `debug-detective` (if `AutoDispatchDebugDetective=true` — default **OFF**) |
| `ci_coverage_gate` | check-run name `"Test-delta gate"` | `test-rig` (if `AutoDispatchTestRig=true`) |
| `ci_transient_rerun` | Transient-flake fingerprint (cpr-timeout, MSYS2-mirror, FetchContent-retry) | `GitHubClient::RerunWorkflowRun(runId)` — **no spawn** |
| `skip` | Name in `IgnoredCheckNames`, advisory check, or unknown name | Log + `Self-improvement` backlog entry |

The hardcoded ignored list ships with `"Coverage (windows-2022 + OpenCppCoverage)"` — advisory until 2026-05-30 per the workflow YAML. After that date, the user manually removes the entry from `IgnoredCheckNames` to enable react-on-coverage-fail. The comment at [`Source_Core/include/ConfigManager.h:402`](../../Source_Core/include/ConfigManager.h) documents this.

### Why `debug-detective` is opt-in

Behavioural regressions auto-spawning `debug-detective` is asymmetrically riskier than build failures. The agent halts at its first pause-loop boundary (per [`AGENTS.md` § Debug-mode pause-loop](../../AGENTS.md)), so practically every behavioural fix is user-gated regardless — but the spawn cost is real and the user should opt in explicitly. Default OFF.

Preferences hint string: "spawn pauses at first investigation round; user reviews".

### CI react commands

Canonical source: [`Source_Core/src/Commands/Builtin/BuiltinCommands_CiReact.cpp`](../../Source_Core/src/Commands/Builtin/BuiltinCommands_CiReact.cpp).

| Command | Destructive | Description |
|---|---|---|
| `ci-react.start` | yes | Enable + restart worker |
| `ci-react.stop` | yes | Disable + restart worker |
| `ci-react.status` | no | Print interval / check-name lists / auto-dispatch toggles / iteration + rerun caps / last-tick |
| `ci-react.poll-now [--pr-url <url>]` | yes | Fire one tick right now |
| `ci-react.rerun --pr-url <url> --workflow-id <id>` | yes | Manually invoke `RerunWorkflowRun(runId)` — same code path the watcher uses for transient flakes. Requires PAT with `actions:write` scope. |

Example:

```bash
Smatchet.exe cmd ci-react.start
Smatchet.exe cmd ci-react.status
# Manually re-run a CI workflow you believe was a transient flake:
Smatchet.exe cmd ci-react.rerun --pr-url https://github.com/alexandrosk0/Smatchet/pull/300 --workflow-id 12345678901
```

### Tunables (config block)

`cfg.CiReact` ([`Source_Core/include/ConfigManager.h:380`](../../Source_Core/include/ConfigManager.h)):

| Field | Default | Notes |
|---|---|---|
| `Enabled` | false | Master toggle |
| `PollIntervalSec` | 600 | Clamped 60..3600 |
| `WatchedBaseBranches` | `["develop"]` | |
| `WatchedCheckNames` | 4 blocking Smatchet checks (see below) | Hand-edit to extend coverage |
| `IgnoredCheckNames` | `["Coverage (windows-2022 + OpenCppCoverage)"]` | Manual flip at 2026-05-30 cutoff |
| `AutoDispatchBuildDoctor` | true | |
| `AutoDispatchTestRig` | true | |
| `AutoDispatchDebugDetective` | false | Opt-in — behavioural fixes are user-gated even when on |
| `TransientRerunEnabled` | true | |
| `TransientRerunMaxPerPr` | 2 | Clamped 0..10 |
| `IterationBudgetPerPr` | 5 | Clamped 1..50 |
| `AnnotationFetchCount` | 20 | Clamped 1..100 |
| `LogTailLines` | 200 | Clamped 10..2000 |

The default `WatchedCheckNames`:

```json
[
    "Windows + MSYS2 UCRT64",
    "Windows + MSYS2 UCRT64 (SMATCHET_WITH_AGENTIC=OFF)",
    "Windows + MSYS2 UCRT64 (SMATCHET_WITH_WHISPER=OFF)",
    "Test-delta gate"
]
```

---

## Workflows (concrete scenarios)

### "I want to triage tickets in my GitHub repo daily"

1. Open Preferences → Agentic.
2. Paste PAT into "GitHub PAT".
3. Paste `OWNER/REPO` into "Query".
4. Set "Interval" to 1800 (30 min) — reasonable cadence for a small backlog.
5. Tick "Enable scheduled agentic triage".
6. Walk away. Check the Agent proposals panel once a day; approve / reject in batches.

### "I want to auto-implement approved feature requests"

1. Do everything from the previous workflow.
2. Also tick "Auto-start handoff when an ImplementIssue proposal is approved" in the Handoff section.
3. Now, approving an `ImplementIssue` proposal in the panel immediately spawns `claude` in `.claude/worktrees/agent-<id>/` and opens a draft PR.
4. Review the draft PR in GitHub when you have time. Merge if good; otherwise comment + let the PR-iteration loop revise.

### "I want CodeRabbit feedback handled automatically on my PRs"

1. Open Preferences → Agentic → CodeRabbit react loop.
2. Tick "Enable CodeRabbit react loop".
3. Leave "Short-circuit-reject invariant-violating suggestions" on (the default).
4. Leave the iteration budget at 5 (per PR).
5. Walk away. Each tick (30 min default), every open PR's new CodeRabbit comments are classified — invariant violations get auto-rejected with a cited rule reply, real findings spawn a fix.
6. Visit Preferences > Audit trail nightly if you want to skim what was rejected.

### "I want red CI to auto-fix when possible"

1. Open Preferences → Agentic → CI react loop.
2. Tick "Enable CI react loop".
3. Leave "Auto-dispatch build-doctor on build failures" on (default).
4. Leave "Auto-dispatch test-rig on coverage-gate failures" on (default).
5. **Leave "Auto-dispatch debug-detective on ctest failures" off** (default) unless you specifically want every ctest red to spawn an investigation.
6. Leave "Re-run workflow on transient fingerprint match" on (default).
7. Walk away. Build failures get `build-doctor` spawned automatically; coverage-gate failures get `test-rig` spawned; ctest failures sit waiting for you to review.

### "I want everything off — observation mode only"

1. Open Preferences → Agentic. Untick every "Enable …" checkbox.
2. The commands still register but every `*.status` call returns `enabled=false`.
3. `*.poll-now` commands still work — useful for manual probes against a fixture without enabling the scheduled loop.

---

## Troubleshooting

### "The watcher isn't firing"

Run through this list:

1. **Master toggle**: `cfg.AgenticPollEnabled` / `cfg.CoderabbitReact.Enabled` / `cfg.CiReact.Enabled` true?
2. **gh auth status**: `gh auth status` returns OK?
3. **PAT scopes**: `repo` + `issues` (+ `actions:write` for `ci-react.rerun`)?
4. **Worker alive**: `*.status` command returns `watcher_present: true`?
5. **Progress log tail**: open the worktree (`.claude/worktrees/agent-<id>/.progress.log`) and look for the most recent line.
6. **Locks**: `bash scripts/dev/locks-show.sh` — any stale lock blocking the current worktree?

### "The spawned harness keeps hitting CLARIFICATION_NEEDED"

The harness is asking for information it can't infer from the seed. Possible causes:

1. The proposal payload was sparse — open the proposals panel and edit the rationale, then re-approve.
2. The issue body itself is ambiguous — comment on the GitHub issue with the clarification first; the dual-channel poll will pick it up.
3. The harness is in a degenerate loop — `handoff.cancel --proposal-id <id>` and re-approve manually with richer context.

When in doubt, answer via the panel (the most reliable path). The GitHub-comment path is best-effort; the panel write is immediate.

### "Lock stays held after I'm done"

The plan-lock workflow only releases on PR merge that contains `lock-slug: <slug>` in the body. If a PR was force-closed or the lock script crashed, manually release:

```bash
bash scripts/dev/lock-release.sh <slug>
```

See [`scripts/dev/lock-release.sh`](../../scripts/dev/lock-release.sh).

### "Worktree leaks"

The handoff path keeps worktrees on disk by default for inspection. Clean up manually:

```bash
# Remove a single worktree (force, force — the second -f handles lock files):
git worktree remove -f -f .claude/worktrees/agent-42

# Or, since worktrees are gitignored, you can just rm -rf the dir + run:
git worktree prune
```

> **`handoff.gc` is NOT shipped in the current CLI.** The plan documents it as a future addition. For now, manual cleanup as above.

### "CodeRabbit reject reply doesn't resolve the thread"

Symptoms: short-circuit reject posts the reply text, but the merge gate stays blocked on the unresolved review thread.

Diagnosis:

1. PAT lacks `repo` + `read:org` scope — the GraphQL `ResolveReviewThread` mutation needs both. Reissue the token.
2. The comment was deleted between the watcher reading it and the resolve mutation firing — log warn at `LookupReviewThreadIdForComment`; nothing to do, the reply is already posted.
3. Thread already resolved — `ResolveReviewThread` returns an idempotency-tolerant error; logged as warn.

Failure is non-fatal by design — the merge gate is the failsafe. Click "Resolve" manually in the GitHub UI when the auto-path degrades.

### "Spawned harness can't find ANTHROPIC_API_KEY"

The env allow-list only inherits `ANTHROPIC_API_KEY` if it's set **in the Smatchet parent process's environment**. If you set it after launching Smatchet, restart Smatchet. The variable must be present in the parent's `environ` at the moment `CreateProcessW` is called.

---

## Reference index

### All commands (15 total)

Compact index — each command is fully documented in its loop's section above. Source: [`Source_Core/src/Commands/Builtin/BuiltinCommands_{Agentic,Handoff,Coderabbit,CiReact}.cpp`](../../Source_Core/src/Commands/Builtin/).

| Family | Commands |
|---|---|
| Triage | `agent.triage.run` |
| Handoff | `handoff.start`, `handoff.cancel`, `handoff.list`, `handoff.clarify`, `handoff.dry-run` |
| CodeRabbit react | `coderabbit-react.start`, `coderabbit-react.stop`, `coderabbit-react.status`, `coderabbit-react.poll-now` |
| CI react | `ci-react.start`, `ci-react.stop`, `ci-react.status`, `ci-react.poll-now`, `ci-react.rerun` |

All `*.start`, `*.stop`, `*.poll-now`, `*.rerun`, `handoff.start`, `handoff.cancel`, `handoff.clarify` are `Destructive = true`; `*.status`, `handoff.list`, `handoff.dry-run` are read-only.

### All config blocks (with defaults)

The full cohesive JSON shape lives at [`Source_Core/include/ConfigManager.h:215`](../../Source_Core/include/ConfigManager.h) (top-level GitHub + triage + handoff fields) and `:338` / `:380` (the nested `CoderabbitReact` / `CiReact` structs, fields listed above per loop's "Tunables" section). All defaults round-trip through `j.value()` on Load — missing keys in older configs hydrate to the safe defaults; no schema-version bump is involved.

### The 18-rule override table

Canonical source: [`agents/coderabbit-triage.md`](../../agents/coderabbit-triage.md) § Override rules. Each rule has a number; `CoderabbitCommentClassifier` consumes the table at startup and cites the matched rule by number in the auto-reject reply.

### Dispatch sources

| `dispatch_source` (string in `SEED.json`) | Routed sub-delegate |
|---|---|
| `proposal_implement` | (handoff-implementer continues per default routing) |
| `coderabbit_comment` | `coderabbit-triage` |
| `ci_build_failure` | `build-doctor` |
| `ci_ctest_failure` | `debug-detective` |
| `ci_coverage_gate` | `test-rig` |
| `ci_transient_rerun` | (no spawn — `gh workflow run` directly) |

Canonical source: [`AGENTS.md` § First-delegate selection](../../AGENTS.md#first-delegate-selection).

### The 9-state handoff FSM

```
Pending → Spawning → Running
                       │     ↑
                       ├──→ AwaitingUser ──(USER_RESPONSE.json)──→ (Running)
                       │
                       ├──→ PrOpen ←──→ Iterating
                       │      │
                       ▼      ▼
                  Complete / Failed / Cancelled  (terminal)
```

Canonical source: [`Source_Core/include/HarnessRunState.h:36`](../../Source_Core/include/HarnessRunState.h). Predicate `IsTransitionAllowed(from, to)` validates every transition at the controller boundary.

### Sentinel file write contracts

| File | Writer | Reader | Write-once? |
|---|---|---|---|
| `SEED.md` | runner | handoff-implementer | yes |
| `SEED.json` | runner | handoff-implementer | yes |
| `CHECK_RUN.json` | classifier | handoff-implementer | yes (per `ci_*` spawn) |
| `CLARIFICATION_NEEDED.json` | harness | runner + UI | yes per round |
| `USER_RESPONSE.json` | runner / UI / poller | harness | rewritten per round |
| `RUN_RESULT.json` | harness | runner | yes (last write before exit) |
| `PR_URL.txt` | harness | runner + UI | yes |
| `.progress.log` | harness | UI panel | append-only |

---

## Out of scope (deferred — see design docs)

- **Bucket-E live-PR end-to-end probe** — current verification is stub-based + synthetic smoke (`scripts/dev/test-coderabbit-react.sh`, `test-ci-react.sh`). Real-PR end-to-end is parked in `docs/backlog/agent-self-improvement/tooling.md`.
- **Cloud Claude runner / Codex / Aider runners** — the `ICodingHarnessRunner` interface accommodates them; only `ClaudeCodeLocalRunner` ships today.
- **Cross-repo PRs** — watchers only follow PRs in the current Smatchet repo.
- **PR merge automation** — covered separately by [`docs/design/merge-gates-ci-coderabbit-comments.md`](../design/merge-gates-ci-coderabbit-comments.md); the spawned harness itself never merges.
- **Webhook-driven react** — polling only (no public HTTPS endpoint).
- **Multi-repo concurrency** + **parallel in-flight handoffs** — serialised today.
- **Auto-GC of worktrees** — `handoff.gc` planned, not shipped; use `git worktree remove -f -f <path>` manually.
- **Webhook receiver, GitHub Enterprise, OAuth, per-user ACLs** — all deferred.

---

## See also

- [`AGENTS.md` § Handoff envelope](../../AGENTS.md#handoff-envelope) + § First-delegate selection (authoritative for sentinel files + dispatch_source)
- [`docs/design/agentic-triage-flow.md`](../design/agentic-triage-flow.md), [`agentic-coding-handoff.md`](../design/agentic-coding-handoff.md), [`coderabbit-react-loop.md`](../design/coderabbit-react-loop.md)
- [`agents/handoff-implementer.md`](../../agents/handoff-implementer.md), [`pr-iterator.md`](../../agents/pr-iterator.md), [`coderabbit-triage.md`](../../agents/coderabbit-triage.md)
