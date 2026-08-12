# Plan — `smatchet-merge-watcher` (out-of-band CI / CodeRabbit poll daemon)
<!-- plan-date: 2026-05-21 -->

> **Slug**: `smatchet-merge-watcher`
>
> **Status**: PHASE-1-READY (2026-05-21 evening grill locked all 5 open design decisions). Lands the P1 backlog entry [`docs/self-improvement/categories/tooling.md`](../../self-improvement/categories/tooling.md) — "Long-running CI / CodeRabbit polls block the interactive session; should run out-of-band" (`644f822` 2026-05-21). Per-user registry at `%LOCALAPPDATA%/Smatchet/merge-watch/`; foreground daemon default; 3-attempt triage budget; Smatchet toast + Windows native BurntToast notifications; explicit owner transfer on `register`. See § Open design decisions — LOCKED for the full rationale.
>
> **Mandatory rules cross-link**: see [`AGENTS.md`](../../AGENTS.md) § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The 2026-05-21 session spawned 6+ background polls (ad-hoc Python invocations from Bash wrapping `scripts/dev/merge-gates.sh`) to drive 5 PRs through the merge-gates contract. Each poll ran 30-40 minutes; each notification consumed a conversation turn (read poll log → reason about merge/triage/abort → commit + push if needed). Four problems surfaced:

1. **Context-budget burn** — orchestrator stayed loaded across idle waits; multi-PR fan-out (5 live polls during the github-tracker + code-color cascade) amplified the cost.
2. **Interruption pattern** — exploratory user work ("double-check this plan", backlog edits) was repeatedly yanked back to merge-bookkeeping by background notifications.
3. **TIMEOUT escalations** — when CR is silent (PR #350 saw 40 polls = 40 min with no review), the user faced the same "force-merge or wait?" decision repeatedly across PRs.
4. **Lossy on session crash** — bg polls die if the parent Claude Code session closes. "Can I close this session?" required "no, polls are mid-flight."

**Intended outcome**: after this lands, the orchestrator runs `merge-watch register <pr>` and walks away. A separate `smatchet-merge-watcher` host process polls every registered PR per the canonical merge-gates contract (now strengthened by PR #360's three-bucket CR logic), invokes the CR-triage classifier when findings appear, auto-cascades stacked PRs on PASS, and notifies the user via Smatchet's existing in-app toast surface when human input is needed. Session can close at will; watcher persists.

## Approach

Build the watcher as a Python daemon outside Smatchet's C++ surface — it talks to GitHub via `gh api`, reads `scripts/dev/merge-gates.sh` for gate semantics, and writes per-PR state to JSON files the orchestrator can `cat`. Five sequential phases (~12 h total), each independently shippable + reviewable:

1. **Registry + CLI + foreground daemon** (~3 h) — `merge-watch register` / `unregister` / `status` / `list` + foreground `daemon` mode that polls every registered PR per the configured interval and emits structured stdout. No triage, no auto-merge yet — just observation.
2. **Cascade detection + auto-merge on PASS** (~2 h) — on PASS, REST-squash-merge + `gh pr list --search "base:<merged>"` to enrol stacked children.
3. **CR-triage classifier** (~3 h) — Python port of `agents/coderabbit-triage.md`'s 18-rule override + Smatchet-invariant table. When poll returns `COMMENTED + N actionable > 0` or `CR_BLOCKED`, classifier reads the review body via `gh api`, applies valid fixes, commits + pushes. Loop continues; CR re-reviews on push.
4. **Smatchet notification surface** (~2 h) — in-app toast via existing `SmatchetToastManager` + a thin shell-side bridge (`scripts/dev/smatchet-notify.sh` POSTs to a local-only HTTP endpoint Smatchet exposes when running). On `CR_FINDINGS_REPEAT` (3+ failed fix attempts), `CI_FAIL`, `CONFLICT`, `USER_COMMENT`, `TIMEOUT` → toast surfaces.
5. **Bats coverage + integration test** (~2 h) — bats around the registry CRUD + state-transition logic; 1 integration test walks a fake PR through PASS → cascade → merge using `gh api`-mocked fixtures (same pattern as `tests/bats/merge_gates.bats`).
6. **Autostart wrappers** (~30 min) — `scripts/dev/merge-watcher-install-autostart.ps1` registers a Windows Scheduled Task that runs `merge-watcher.py daemon` at user login with restart-on-crash (3 attempts, 5 min apart) + log redirect to `%LOCALAPPDATA%\Smatchet\merge-watch\daemon.log`. Idempotent re-install (unregisters first). Counterpart `-uninstall-autostart.ps1` removes the task; preserves user data (registry + log). Optional `-PollInterval`, `-PythonExe`, `-TaskName` params for tuning. One-shot user UX: `powershell -ExecutionPolicy Bypass -File scripts/dev/merge-watcher-install-autostart.ps1`.

**Non-obvious trade-off**: phase 3's CR-triage classifier is a Python port of `agents/coderabbit-triage.md` rather than a `claude --headless` subprocess spawning the agent. Cold-start cost (2-5s per spawn) + per-invocation API charge would dominate the loop's wall-clock at scale; Python port adds duplication risk (agent.md + python script must stay in sync) but mitigates it by making `agents/coderabbit-triage.md` declare the python script as canonical implementation. See § Out of scope for the alternative subprocess shape if duplication-drift becomes a real concern.

## Files to modify

**New files (~8)**:

1. [`scripts/dev/merge-watcher.py`](../../scripts/dev/) — main daemon loop. ~180 LOC (was ~150; per-user registry adds `clone_path` handling + multi-clone cascade serialization).
2. [`scripts/dev/merge-watcher-cli.py`](../../scripts/dev/) — `register` / `unregister` / `status` / `list` subcommands. ~80 LOC.
3. [`scripts/dev/coderabbit-triage.py`](../../scripts/dev/) — Phase 3 classifier (CR-finding parser + Smatchet-invariant rejection table + delegated subsystem fixer). ~150 LOC.
4. [`scripts/dev/smatchet-notify.sh`](../../scripts/dev/) — Phase 4 shell bridge to Smatchet's local-only HTTP toast endpoint. ~30 LOC.
5. [`scripts/dev/smatchet-notify-windows.ps1`](../../scripts/dev/) — Phase 4 Windows native toast via BurntToast for "Smatchet not running" fallback. ~25 LOC.
5b. [`scripts/dev/merge-watcher-install-autostart.ps1`](../../scripts/dev/) — Phase 6 Windows Scheduled Task installer (run-at-login + restart-on-crash + log redirect). ~80 LOC.
5c. [`scripts/dev/merge-watcher-uninstall-autostart.ps1`](../../scripts/dev/) — Phase 6 counterpart (preserves user data). ~30 LOC.
6. [`tests/bats/merge_watcher.bats`](../../tests/bats/) — Phase 5 registry CRUD + state-transition coverage. ~120 LOC.
7. [`tests/fixtures/watcher_registry_active.json`](../../tests/fixtures/) — Phase 5 sample registry fixture (carries `clone_path` per entry).
8. [`tests/fixtures/watcher_pr_state_*.json`](../../tests/fixtures/) — Phase 5 per-PR state fixtures (one per state-transition test case).

**Runtime state (per-user, outside repo)**:

```
%LOCALAPPDATA%/Smatchet/merge-watch/   # per-user; created on first `register`
├── active.json                         # registry (list of {pr, clone_path, registered_at, triage_attempts})
├── active.json.lockfile                # file-lock for multi-daemon serialization
├── daemon.pid                          # PID + start-time for `status --daemon`
├── state/<pr>.json                     # per-PR poll state (last-poll, last-CR-review-SHA, fix-attempt counter)
└── locks/cascade-<branch>.lock         # per-branch lock during cascade-into-stacked-children
```

**Modified (~3)**:

8. [`AGENTS.md`](../../AGENTS.md) § Autonomous ship-loop § Post-ship § option 3 — reword to "Register with watcher" per the v2 re-grill (already named in `docs/plans/shipped/agentic-ripout-doc-cleanup-v2.md` § AGENTS.md edits as "REWORD, not strip"). When this plan ships, the v2 plan's locked decision flips to "applied".
9. [`AGENTS.md`](../../AGENTS.md) § Merge gates § Scope boundary — extend the caller list from "orchestrator + git-janitor in the user's main session" to also include `smatchet-merge-watcher`. Per v2 re-grill § Merge gates § Surgical edit line 194.
10. [`Source_Core/include/SmatchetToastManager.h`](../../Source_Core/include/SmatchetToastManager.h) + [`SmatchetToastManager.cpp`](../../Source_Core/src/SmatchetToastManager.cpp) — Phase 4 only: expose a local-only HTTP endpoint (already-running cpp-httplib instance) that accepts POST `/merge-watch/notify` with a JSON payload `{pr, state, message}`. Toast appears in the running Smatchet UI.

## Existing utilities reused

- `scripts/dev/merge-gates.sh::poll_merge_gates` — gate-check engine; daemon's per-PR poll calls this script directly. PR #360 just hardened the CR logic (non-empty review + STALE three-bucket) — watcher inherits the fix.
- `scripts/dev/merge-gates.graphql` — same query; no fork.
- `gh api graphql` / `gh api -X PUT /pulls/$pr/merge` / `gh pr list --search` — REST + GraphQL contracts; no new GitHub-side surface.
- `agents/coderabbit-triage.md` (KEPT per v2 re-grill) — Phase 3 classifier is its Python port; agent.md retitled to declare the python as canonical implementation.
- `Source_Core/include/SmatchetToastManager.h` — Phase 4 toast surface, already used by `BackendAuditTrail` + `AssistantStreamingNotifier`. Reuse the existing append-toast API; just add the HTTP entry-point.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: **no impact** — watcher is a separate host process; Smatchet's UI thread runs unmodified. Phase 4's HTTP endpoint receives 1 POST per state-change (typically < 10 / hour); request handling drains via existing `MainThreadDispatcher` post-back, not on UI thread.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: **no impact** — same separation as Pillar 1. Phase 4's HTTP server runs on a worker thread (cpp-httplib's default); UI thread sees only the dispatcher-posted toast append, which is sub-ms.
- **Pillar 3 (never crash)**: **mitigated** — watcher daemon can crash (subprocess, OOM, network outage) without affecting Smatchet. Daemon writes per-PR state every poll, so resume on restart is automatic. Phase 4's HTTP endpoint must bind to localhost-only (`127.0.0.1`) + reject non-localhost connects — bug here = unprotected RCE-via-toast surface. Sanitizer build covers the endpoint code per AGENTS.md § Pillar 3 mandatory pre-merge sanitizer.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: **no impact** — toast surface already accessible per existing `SmatchetToastManager` conformance. Watcher CLI is keyboard-only by definition.

## Perf-review-system gates

Per [`docs/plans/shipped/pillar-1-2-perf-review-system.md`](pillar-1-2-perf-review-system.md):

1. **PR-fast CI** — **N/A for Phases 1-3 + 5** (no `Source_Core/` touch); **fires for Phase 4** — `notifyHttpEndpoint` exercises the existing toast pipeline. Scenario: `app_startup_with_toast_replay` if extant, else the closest match in [`agents/perf-gatekeeper.md`](../../agents/perf-gatekeeper.md) § Curated diff → scenario map; orchestrator picks the named scenario at PR-open time + records the choice in `## Verification (actual)`.
2. **Pillar 2 static scanner** — **fires for Phase 4** — new HTTP endpoint must not call any sync I/O reachable from `ImGui::*`. The endpoint accepts the POST on a cpp-httplib worker thread, parses JSON, queues `MainThreadDispatcher::PostToMainThread([&]{ toastManager.Append(...); })`. No sync I/O reaches the UI thread; annotate the cpp-httplib accept-loop function with `/* PILLAR2_WORKER_ONLY */ // est-latency: <50ms` per the convention.
3. **Dispatcher drain** — **fires for Phase 4** — toast append posts back through `MainThreadDispatcher::Drain()`. Existing drain-cap budget applies; toast notifications are < 1/sec sustained.
4. **Visible-cue bucket-E harness** — **N/A** — Phase 4 adds an INVISIBLE-when-empty surface; no new sync-stall code path.
5. **Marker inventory** — **N/A** — no new `SMATCHET_UI_PERF_SCOPE` markers (HTTP endpoint runs on worker; UI-thread toast append already inside `SMATCHET_UI_PERF_SCOPE("Toast::AppendOne")` per existing instrumentation).

**Pre-push local check**: for Phase 4 only, run [`docs/guides/perf-workflow.md`](../guides/perf-workflow.md) § Gate-check vs baseline (Step 7) against the chosen scenario before opening the PR.

**Override**: none anticipated; no perf regression expected from a localhost HTTP POST.

## Open design decisions — LOCKED 2026-05-21 evening grill

Five decisions locked via `grill-with-docs` pass before Phase 1 starts. Each affects the file surface above + the Phase 1 implementation shape.

1. **Registry location — `%LOCALAPPDATA%/Smatchet/merge-watch/active.json` (per-user)**. Single registry per machine, watches PRs across all Smatchet clones. Implication: each registry entry carries a `clone_path` field so the daemon knows where to `git pull --rebase` during cascade. Survives clone-deletion → orphaned-clone PRs surface in `merge-watch status` for explicit unregister. File-lock (`.lockfile` next to `active.json`) serializes multiple daemon-process accesses — though the foreground-default rule should make multi-daemon a rare misconfiguration anyway.
2. **Triage failure budget — 1 attempt** (post once, then surface). Tunable via `MERGE_WATCH_TRIAGE_BUDGET` env var (default 1; was 3 — lowered per the option-C fix logged in `docs/backlog/agent-self-improvement/process.md` since triage retries don't fix code, they only re-classify; the loop's actual value is the user notification). After budget exhausted: notify the user with the last attempt's CR-feedback delta + cumulative-attempt log plus a one-click `…/pull/<n>/files` URL so the user lands on the diff view that shows CR's inline markers. Stop polling that PR until user `unregister`s + re-`register`s OR explicitly invokes `merge-watch retry-triage <pr>` (Phase 3 CLI extension — out of Phase 1 scope). **Per-poll counter-reset (2026-05-28 sub-bug a, `docs/plans/shipped/merge-watcher-triage-recovery.md`)**: when a poll's `status_line` shows CR is no longer block-shaped (matches CR-clean tokens like `0 actionable` / `STALE_RESOLVED` / `STALE_CLEAN` / `APPROVED`), `handle_blocked_cr_triage` zeroes `triage_attempts` in the registry (preserving `triage_for_head_sha` for diagnostic continuity) and surfaces `triage_reset_on_cr_clear` in the state dict. Prevents the per-PR-lifetime counter latching the registry at `TRIAGE_BUDGET_EXHAUSTED` once a CR-finding round clears.
3. **Notification channel — Smatchet toast + Windows native (P0); webhooks deferred**. Two-prong: (a) Smatchet toast via existing `SmatchetToastManager` + Phase 4's localhost HTTP endpoint (only fires when Smatchet is running); (b) Windows native via BurntToast PowerShell module (or `New-BurntToastNotification` shell call) so user sees notifications even when Smatchet is closed. Webhook (Slack / Discord / Teams) deferred to a P1 follow-up once user-need surfaces.
4. **Daemon foreground default + `--background` opt-in**. `merge-watch daemon` runs in foreground (user sees structured stdout per poll cycle); `--background` opt-in detaches via Python `subprocess.Popen` with `creationflags=CREATE_NEW_PROCESS_GROUP` (Windows). State file (`%LOCALAPPDATA%/Smatchet/merge-watch/daemon.pid`) records PID for `merge-watch status --daemon` checks. Foreground catches failures early during dogfooding; opt-in detach matches the production shape once user trusts the loop.
5. **Explicit owner transfer on register**. `merge-watch register <pr>` prints "watcher now owns this PR; use `unregister` to take back control". Orchestrator NEVER auto-merges a registered PR (checks registry before any merge-gates poll). User can `merge-watch unregister <pr>` to take back; daemon stops polling on next cycle. Clean ownership boundary; no race between orchestrator + watcher + user.
6. **Phase 5 — option A auto-act (opt-in)**. `MERGE_WATCH_AUTO_ACT=true` enables Claude-headless auto-spawn on CR-finding terminal states (`TRIAGE_BUDGET_EXHAUSTED`, `COMMENTED (N actionable — block)`, `STALE_WITH_FINDINGS`, `CHANGES_REQUESTED`). Closes the watcher's "observe + notify but never fix" loop. Off by default — auto-spawning a Claude session against a checked-in clone has real token cost + runaway-loop risk (Claude's fix produces new CR findings → another auto-act → repeat). Safeguards:
   - **Single attempt per (PR, head_sha) pair** — dedup key persisted on the registry entry's `auto_act_for_head_sha` field. A push to the PR (which advances head_sha) unlocks one more attempt — bounded by the next item.
   - **Per-PR-lifetime budget** — `MERGE_WATCH_AUTO_ACT_BUDGET` (default 2). Once attempts ≥ budget the auto-act stops firing on this PR even on new pushes; the user `merge-watch unregister`s to take back.
   - **Refuses if `claude` is not on PATH** — no silent no-op; the daemon logs `skipped: claude binary not on PATH` so misconfigured setups are visible.
   - **Refuses if the clone has uncommitted tracked-modified files** — concurrent agent / user edits would race. Daemon logs the file count + skips.
   - **Detached background subprocess** — the daemon's poll loop is never blocked by Claude's session runtime. Per-(PR, sha-prefix) log file under the state dir (`<pr>-auto-act-<sha8>.log`) captures stdout+stderr for after-the-fact inspection.

   Spawn shape: `claude -p "<AUTO_ACT_PROMPT>"` with CWD set to the registered clone path. The prompt is deliberately spare (no project rules pasted; the session reads AGENTS.md + CLAUDE.md from the clone on its own); it instructs the session to `gh pr checkout`, address inline CR findings, and commit + push with a `fix(merge-watcher auto-act):` prefix.

7. **`resolveReviewThread` after auto-act push (opt-in, sub-bug b, 2026-05-28)**. `MERGE_WATCH_RESOLVE_CR_THREADS=true` enables `maybe_resolve_stuck_cr_threads` in the poll loop (between triage and notify). After an auto-act spawn pushes a fix commit, CR's per-line review threads can stay `isResolved:false` on the prior head even when CR's StatusContext on the new head flips to SUCCESS; the merge gate's `cr_open > 0` check then keeps the PR BLOCKED indefinitely. The helper fetches CR-authored, non-outdated, unresolved review threads via `gh api graphql` (canonical query `merge-watcher.py:_CR_THREADS_QUERY`) and calls `mutation resolveReviewThread(input:{threadId:$id})` per thread (canonical mutation `merge-watcher.py:_RESOLVE_MUTATION`). Gate conditions (all required):
   - `MERGE_WATCH_RESOLVE_CR_THREADS=true` env set.
   - Registry has `auto_act_for_head_sha` recorded (a prior auto-act fired).
   - Current `headRefOid` differs from `auto_act_for_head_sha` (the push landed).
   - Status line is NOT CR-block-shaped (`_looks_like_cr_finding_block` returns False — defensive, so genuine new findings are not auto-resolved).
   - `last_resolved_for_head_sha` ≠ current head (one resolve pass per head; same dedup pattern as `auto_act_for_head_sha`).

   Persisted on registry per entry: `last_resolved_threads_count`, `last_resolved_at_unix`, `last_resolved_for_head_sha`. Off by default for the first ship; flip default after one production cycle. Failure mode is bounded: a thread the fix did not actually address will be re-opened by CR on its next review pass. Plan + bats coverage: `docs/plans/shipped/merge-watcher-triage-recovery.md`; tests in `tests/bats/merge_watcher.bats` (9 cases — env-gate, head-advanced fire, head-unchanged skip, same-head dedup, zero-thread noop, etc.).

**Env knob summary** (canonical reference for `MERGE_WATCH_*` listed alongside their decision sites):

| Env var | Default | Decision | Purpose |
|---|---|---|---|
| `MERGE_WATCH_POLL_INTERVAL` | `60` (s) | item 4 | Seconds between daemon poll cycles. |
| `MERGE_WATCH_TRIAGE_BUDGET` | `1` | item 2 | Max CR-triage classifier posts per (PR, head_sha). |
| `MERGE_WATCH_AUTO_ACT` | `false` | item 6 | Spawn `claude -p` to address CR findings. |
| `MERGE_WATCH_AUTO_ACT_BUDGET` | `2` | item 6 | Max auto-act spawns per PR lifetime. |
| `MERGE_WATCH_AUTO_ACT_ON_SANITIZER` | `false` | item 6 (sanitizer variant) | Spawn auto-act on sanitizer CI failure (uses `debug-detective` path, not `coderabbit-triage`). |
| `MERGE_WATCH_RESOLVE_CR_THREADS` | `false` | item 7 | Resolve stuck CR review threads after auto-act push. |

## Risks / non-goals

**Risks**:

- **Phase 3 duplication drift** — `agents/coderabbit-triage.md` rules + `coderabbit-triage.py` rules must stay in sync. Mitigation: agent.md declares the python as canonical implementation (one-way reference); a doctest-style bash check at end-of-CI greps both files for a shared "rules version" marker and fails if they disagree.
- **Phase 4 HTTP endpoint security** — local-bound only is the contract; CVE in Smatchet HTTP layer = local RCE via toast injection. Mitigation: `127.0.0.1`-bind hard-coded, no env override; payload schema-validated; toast text HTML-escaped before render; sanitizer build mandatory.
- **Watcher daemon crash leaves PRs orphan** — registry survives via JSON on disk; per-PR state file last-poll timestamp lets a restarted daemon resume. Mitigation: `merge-watch daemon` on restart reads registry + state files + resumes per-PR loop where each left off.
- **CR rate-limiting** — if multiple PRs go ready simultaneously (per AGENTS.md § Post-ship turn-end protocol option 3 — "Register with watcher" implies `gh pr ready` is the first step the watcher runs after `merge-watch register`), CR may rate-limit + skip some. Already observed this session (#350 + #357). Mitigation: watcher staggers `gh pr ready` calls by 30s when batch-registering.
- **Cascade race** — if two parents merge near-simultaneously, both try to pull develop into the same child branch. Mitigation: per-branch lock-file in `.claude/.merge-watch/locks/`.

**Non-goals**:

- **Cross-repo support** — Smatchet only. Per-repo watcher daemon would be a future expansion.
- **GitHub Enterprise** — `api.github.com` only; per-repo `cfg.GitHubBaseUrl` lookup deferred.
- **Multi-user / multi-account** — single `ORCH_USER`. Multi-account daemons need a separate registry per account; not P0.
- **Persistent storage beyond JSON files** — no SQLite. State volume is small (< 100 PRs at peak); JSON survives daemon restarts adequately.
- **Reverting bad merges** — watcher merges; never unmerges. Reverts are a human-initiated `gh pr` operation.

## Daemon environment prerequisites

The watcher runs as a Windows Scheduled Task (`SmatchetMergeWatcher`), which spawns the daemon with a **minimal inherited env** — only the user's persistent `PATH`, not the augmented one a Git Bash session enjoys. Three tools MUST be discoverable to the daemon for polls to succeed (added by [PR #391](https://github.com/alexandrosk0/Smatchet/pull/391) after the daemon crashed on its first real poll cycle):

| Tool | Used by | Install | Standard location |
|---|---|---|---|
| `gh` | daemon directly + `merge-gates.sh` (graphql call) | `winget install GitHub.cli` | `C:\Program Files\GitHub CLI\gh.exe` |
| `jq` | `merge-gates.sh` (parsing graphql response) | `winget install jqlang.jq` | `%LOCALAPPDATA%\Microsoft\WinGet\Links\jq.exe` |
| `bash` | spawning `merge-gates.sh` | Git for Windows (`git-scm.com/download/win`) | `C:\Program Files\Git\bin\bash.exe` |

**Crucial Windows gotcha — `bash` resolution**: Windows ships `C:\Windows\System32\bash.exe` as a WSL launcher. Bare `bash` on PATH often resolves to WSL bash first, which **cannot run `merge-gates.sh`** (WSL has its own `/bin/bash` that may be misconfigured; symptom: `execvpe(/bin/bash) failed: No such file or directory`). The daemon's `_resolve_bin("bash", …)` explicitly probes Git for Windows' install dir first AND rejects `System32\bash.exe` even if `shutil.which()` returns it — both belt-and-braces because System32 is so often early on PATH.

`scripts/dev/merge-watcher-install-autostart.ps1` checks all three at install time and refuses to register the task if any are missing, with the exact `winget` command to fix it. The daemon itself also probes standard install paths (`Get-Command` + `winget Links` dir + Git's `bin`) via `_resolve_bin()` and `_resolve_orch_user()` at startup as a defence-in-depth — but failing at install time is the loud-and-early posture we want.

`ORCH_USER` is auto-resolved at daemon startup via `gh api user --jq .login` and cached for the daemon's lifetime, so no per-PR cost.

If the daemon polls and you see per-PR state files at `%LOCALAPPDATA%\Smatchet\merge-watch\state\*.json` with empty `last_status_line` + `triage_action: "skipped: BLOCKED but not CR-finding"`, that's the symptom — re-run the install script and read the prerequisite-check output.

## Verification

Per [`AGENTS.md`](../../AGENTS.md) § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: **N/A** for phases 1-5; the watcher is a Python script, not C++. Phase 3's classifier may have a Python doctest equivalent — `pytest scripts/dev/test_coderabbit_triage.py` covering the 18-rule override table + Smatchet-invariant rejection cases.
- **Bucket E (ImGui Test Engine)**: **N/A for phases 1-3 + 5**. Phase 4 only — `tests/ui/merge_watcher_toast_arrives.test.cpp` mirrors `tests/ui/views_columns_reorder.test.cpp`'s shape; posts a synthesised `{pr:999, state:CI_FAIL, message:"..."}` to the local endpoint, asserts toast appears in `SmatchetToastManager` within 1s, asserts toast text matches the message.
- **Bash-driver scenario / screenshot / sanitizer**: **bats** for phases 1-3 + 5 via `tests/bats/merge_watcher.bats`. Sanitizer build mandatory for Phase 4 (Pillar 3 requirement; HTTP endpoint is C++).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — only Phase 4 touches `Source_Core/`; other phases skip this gate (Python + bash only).
- **Manual residue**: zero expected. The watcher CLI is a user-driven interactive surface, but `merge-watch register` / `unregister` / `status` / `list` are all bats-coverable (CRUD + state-transition assertions); `daemon` mode is exercised by the bats integration test against `gh api`-mocked fixtures.

## Out of scope (flagged, not designed)

- **Subprocess-based CR triage (`claude --headless`)** — alternative shape if Phase 3's Python-port duplication-drift becomes a real concern. Cost: 2-5s spawn latency + per-invocation API charge. Switch path documented for future implementer — not built now.
- **MCP-tool-based CR triage (against user's running session)** — third alternative. Requires Smatchet running + open Claude Code session. Most-coupled option; deferred until a concrete user-need surfaces.
- **In-app watcher panel (`SmatchetMergeWatcherUi.cpp`)** — visual surface in Smatchet showing watched PRs + state, mirroring `SmatchetMcpServerUi.cpp`'s shape. Deferred — phase 4's toast handles surface; full panel waits for "I have N watched PRs and want to see them all at a glance" user need.
- **Auto-revert on post-merge regression** — if a merged PR breaks `develop`'s CI, watcher could auto-revert. Out of scope — too dangerous to default-on; needs a separate plan.
- **`merge-watch unregister --reason=<text>`** — audit-trail every unregister with a reason string for later analysis of why users abandoned watcher-owned PRs. Deferred — not P0.

## Implementation log

Shipped 2026-05-21 → 2026-05-28 across 6 phases + 7 follow-up fix/extension PRs. All 7 plan-listed deliverables exist on `develop`; Phase 4b shipped as a dedicated server TU rather than a `SmatchetToastManager` extension (see § Deviations).

- **Phase 1 — Registry CRUD + foreground daemon** (#363, 2026-05-21). `scripts/dev/merge-watcher.py` + `merge-watcher-cli.py`; per-user registry at `%LOCALAPPDATA%/Smatchet/merge-watch/active.json` with file-lock serialization; foreground-default daemon writes per-PR state every poll cycle.
- **Phase 2 — PASS-branch auto-merge + cascade** (#364, 2026-05-21). On `GATES_PASSED`, watcher flips draft→ready, REST-squash-merges via `gh api -X PUT`, detects stacked children via `gh pr list --search "base:<merged-branch>"`, pulls develop into each. Per-branch lock-files in `.claude/.merge-watch/locks/` prevent cascade races.
- **Phase 3 — CR-triage classifier** (#365 + #382, 2026-05-21). `scripts/dev/coderabbit-triage.py` (373 LoC) implements the 18-rule Smatchet-invariant rejection table from `agents/coderabbit-triage.md`. `MERGE_WATCH_TRIAGE_BUDGET` env tunable (default 1 — lowered from 3 per the option-C rationale in § Decisions locked item 2).
- **Phase 4a — notify dispatch** (#366, 2026-05-21). `scripts/dev/smatchet-notify.sh` + `smatchet-notify-windows.ps1` give the two-prong channel: localhost HTTP POST to Smatchet (when running) + BurntToast fallback. Re-notify suppression via `notify_dispatched_for_state` registry field.
- **Phase 4b — Smatchet in-app HTTP notify endpoint** (#367, 2026-05-21). `Source_Core/include/SmatchetMergeWatchNotifyServer.h` + `Source_Core/src/SmatchetMergeWatchNotifyServer.cpp` (161 LoC). cpp-httplib server on `127.0.0.1:7679` accepts POST `/merge-watch/notify` and posts toast appends through `MainThreadDispatcher`.
- **Phase 4c — Windows Scheduled Task autostart** (#370, 2026-05-21). `merge-watcher-install-autostart.ps1` (run-at-login + restart-on-crash + log redirect) + `merge-watcher-uninstall-autostart.ps1` (preserves user data).
- **Phase 5 — bats integration coverage** (#368, 2026-05-21). `tests/bats/merge_watcher.bats` (997 LoC at archive time) covers registry CRUD, file-lock concurrency, state-file lifecycle, per-PR JSON shape, Phase 4a notify suppression, Phase 4b HTTP probe, Phase 3 classifier rule-set sample.

**Follow-up fix / extension PRs (post-Phase-5)**:

- #392, #393 (2026-05-22) — auto `gh pr ready` before merge; resolve `bash` to Git-Bash (not WSL) on Windows hosts; UnicodeEncodeError defence (`sys.stdout.reconfigure(encoding='utf-8')`) for the `→` glyph that crashed the daemon under cp1252.
- #407 (2026-05-23) — option-C fast-notify on CR findings + inline-files URL surfaced in the notification body.
- #418 (2026-05-23) — per-HEAD reset of `triage_attempts` (the precursor fix to the 2026-05-28 P1 entry below).
- #428 (2026-05-23) — C4 prong 1: flip draft→ready BEFORE the gates poll (not just before merge) so CR `auto_review.drafts:false` doesn't skip review.
- #431 (2026-05-23) — C4 prong 2: require non-empty CR review for the `NONE + status-SUCCESS` pass path (closes the draft-PR-bypass-via-placeholder-StatusContext gap).
- #487 (2026-05-28) — triage-budget reset on CR-clear + `resolveReviewThread` mutation after auto-act push (opt-in `MERGE_WATCH_RESOLVE_CR_THREADS=true`). Plan: `docs/plans/shipped/merge-watcher-triage-recovery.md`.
- #522 (2026-05-28) — **CR-NONE grace driver.** `maybe_pass_cr_none_grace` counts consecutive `NONE+status-SUCCESS-waiting-for-inline` / `NONE+pending` grace-wait cycles per HEAD in the registry (`cr_none_grace_polls` / `cr_none_grace_head`, mirroring `triage_attempts`) and, once `MERGE_WATCH_CR_NONE_GRACE_CYCLES` (default 10, floored 1) real cycles elapse, re-polls once with `MERGE_GATES_CR_GRACE_POLLS=0` so the single poll's grace passes → `GATES_PASSED` → `handle_pass`. Closes the wedge where a skipped/absent CodeRabbit review (the common case for trivial diffs — CR fires its SUCCESS StatusContext but posts no inline review) never merges and never notifies: merge-gates' in-process grace (`p >= CR_GRACE_POLLS` at `merge-gates.sh` lines 485/501) is unreachable because `poll_one` drives merge-gates with `MERGE_GATES_MAX_POLLS=1` (so `p` is always 0 and resets every cycle). Watcher-side only — no merge-gates change (the standalone orchestrator's default `MAX_POLLS=60` already reaches grace). Fail-closed on HEAD-fetch failure; resets on a new push or when CR leaves the NONE-wait shape. `poll_one` gained an `extra_gates_env` param; detector `_looks_like_cr_none_grace_wait`; 7 bats tests. Surfaced live on #514 (trivial shell-only diff).

## Deviations from plan

- **`tests/fixtures/watcher_registry_active.json` + `watcher_pr_state_*.json` — NOT created.** Plan § Files-to-modify items 7 + 8 named these fixtures; Phase 5 (#368) ended up using inline JSON via bats heredocs / `cat > "$LOCALAPPDATA/…/active.json" <<JSON …` patterns. Rationale: per-test bespoke shapes were easier to keep readable than juggling six top-level fixture files plus the inline state-overrides every test needs anyway. No observable coverage gap.
- **Phase 4b shape — NEW server TU instead of `SmatchetToastManager` extension.** Plan § Files-to-modify item 10 named `SmatchetToastManager.h/.cpp` as the surface to extend. Phase 4b (#367) added a new `SmatchetMergeWatchNotifyServer.h/.cpp` pair instead — the HTTP server lifecycle (bind / accept loop / shutdown) is a different concern from the toast-render lifecycle, and tangling them into one TU would have grown `SmatchetToastManager.cpp` past the 67 KB file-size cap. The server TU calls `MainThreadDispatcher::PostToMainThread([&]{ toastManager.Append(...); })` on receipt, so the dispatcher-drain integration the plan specified is preserved.
- **Triage budget default 3 → 1.** Plan § Decisions locked item 2 originally locked 3; updated in-line via the option-C fix logged in `docs/backlog/agent-self-improvement/process.md` (2026-05-22, applied via #407). Triage retries don't fix code, they only re-classify; the loop's actual value is the user notification, which should fire on the next poll, not three polls later. The plan-doc decision text has since been amended in-place to reflect the new default + cross-link to the sub-bug-a fix (2026-05-28, #487).
- **Auto-act safeguards (decision 6) shipped in two waves.** Plan's decision-6 list was implemented as Phase 5+, not as part of original Phase 5. `MERGE_WATCH_AUTO_ACT`, dedup, budget, claude-on-PATH check, uncommitted-clone guard, detached subprocess, per-`(PR, sha-prefix)` log file — all landed across #407 / #418 / a follow-up `chore(merge-watcher): option C` series.
- **C4 (draft-PR bypass) closure is two prongs, both implemented post-plan.** Plan didn't anticipate the `auto_review.drafts:false` CR config behaviour. Closed via #428 (flip-ready before gates poll) + #431 (require non-empty CR review for `NONE + status-SUCCESS`). Both prongs cross-linked from `docs/agent-rules/merge-gates.md`.
- **Triage-recovery work (sub-bug a + sub-bug b) gets a sibling plan.** The 2026-05-22 latching-`TRIAGE_BUDGET_EXHAUSTED` + missing-`resolveReviewThread` issues turned out to be substantive enough that they earned their own plan doc — `docs/plans/shipped/merge-watcher-triage-recovery.md` — rather than being captured as deviations here. Same code area; separate plan because the fix introduces new helper functions + a new env knob + a documented manual-unblock recipe.

## Verification (actual)

- **Bucket A (pure-logic ctest)**: N/A — implementation is Python + bash + a Phase-4-only C++ TU; no `Source_Core/` pure-logic helpers exposed.
- **Bucket E (ImGui Test Engine)**: deferred. The plan promised `tests/ui/merge_watcher_toast_arrives.test.cpp` for Phase 4b; it never shipped. The 4b TU is exercised via Phase 5 bats (`maybe_notify`'s subprocess invocation of `smatchet-notify.sh`, which HTTP-POSTs to `127.0.0.1:7679` and surfaces success/failure). Tracked in tooling backlog as a follow-up — replacing it with a bucket-E probe would be one of the deferred bucket-E entries already on the list.
- **Bash-driver scenario**: `tests/bats/merge_watcher.bats` — passing on every CI run since Phase 5 landed. Coverage spans registry CRUD, lock concurrency, state-file lifecycle, classifier rule-set, notify suppression, HTTP-endpoint smoke, and (post-#487) the 9 triage-recovery cases.
- **Sanitizer build**: Phase 4b TU runs in the `Sanitizer (ASAN + UBSAN)` CI job; clean since #367.
- **Build gate**: dual-target `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — Phase 4b TU is `SMATCHET_WITH_MCP`-gated on Standalone (Unreal target keeps `SMATCHET_WITH_MCP_UNREAL` OFF), so Unreal-side compiles unaffected. Clean on every merge.
- **PR-fast perf scenario**: Phase 4b's POST handler does not run on the UI thread; dispatcher drain absorbs the toast append. No regression observed in `app_startup_with_toast_replay` baselines around the #367 timeframe.
- **Production validation**: the watcher has merged every PR in this repo that landed via "Register with watcher" since 2026-05-21, including a multi-PR cascade during the github-tracker work + the 2026-05-28 session's PRs once the sub-bug-b `resolveReviewThread` fix (#487) closed the stuck-threads gap.
