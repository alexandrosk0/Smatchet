# Plan — `smatchet-merge-watcher` (out-of-band CI / CodeRabbit poll daemon)

> **Slug**: `smatchet-merge-watcher`
>
> **Status**: PHASE-1-READY (2026-05-21 evening grill locked all 5 open design decisions). Lands the P1 backlog entry [`docs/backlog/agent-self-improvement/tooling.md`](../backlog/agent-self-improvement/tooling.md) — "Long-running CI / CodeRabbit polls block the interactive session; should run out-of-band" (`644f822` 2026-05-21). Per-user registry at `%LOCALAPPDATA%/Smatchet/merge-watch/`; foreground daemon default; 3-attempt triage budget; Smatchet toast + Windows native BurntToast notifications; explicit owner transfer on `register`. See § Open design decisions — LOCKED for the full rationale.
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

**Non-obvious trade-off**: phase 3's CR-triage classifier is a Python port of `agents/coderabbit-triage.md` rather than a `claude --headless` subprocess spawning the agent. Cold-start cost (2-5s per spawn) + per-invocation API charge would dominate the loop's wall-clock at scale; Python port adds duplication risk (agent.md + python script must stay in sync) but mitigates it by making `agents/coderabbit-triage.md` declare the python script as canonical implementation. See § Out of scope for the alternative subprocess shape if duplication-drift becomes a real concern.

## Files to modify

**New files (~8)**:

1. [`scripts/dev/merge-watcher.py`](../../scripts/dev/) — main daemon loop. ~180 LOC (was ~150; per-user registry adds `clone_path` handling + multi-clone cascade serialization).
2. [`scripts/dev/merge-watcher-cli.py`](../../scripts/dev/) — `register` / `unregister` / `status` / `list` subcommands. ~80 LOC.
3. [`scripts/dev/coderabbit-triage.py`](../../scripts/dev/) — Phase 3 classifier (CR-finding parser + Smatchet-invariant rejection table + delegated subsystem fixer). ~150 LOC.
4. [`scripts/dev/smatchet-notify.sh`](../../scripts/dev/) — Phase 4 shell bridge to Smatchet's local-only HTTP toast endpoint. ~30 LOC.
5. [`scripts/dev/smatchet-notify-windows.ps1`](../../scripts/dev/) — Phase 4 Windows native toast via BurntToast for "Smatchet not running" fallback. ~25 LOC.
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

8. [`AGENTS.md`](../../AGENTS.md) § Autonomous ship-loop § Post-ship § option 3 — reword to "Register with watcher" per the v2 re-grill (already named in `docs/design/agentic-ripout-doc-cleanup-v2.md` § AGENTS.md edits as "REWORD, not strip"). When this plan ships, the v2 plan's locked decision flips to "applied".
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

Per [`docs/design/pillar-1-2-perf-review-system.md`](pillar-1-2-perf-review-system.md):

1. **PR-fast CI** — **N/A for Phases 1-3 + 5** (no `Source_Core/` touch); **fires for Phase 4** — `notifyHttpEndpoint` exercises the existing toast pipeline. Scenario: `app_startup_with_toast_replay` if extant, else the closest match in [`agents/perf-gatekeeper.md`](../../agents/perf-gatekeeper.md) § Curated diff → scenario map; orchestrator picks the named scenario at PR-open time + records the choice in `## Verification (actual)`.
2. **Pillar 2 static scanner** — **fires for Phase 4** — new HTTP endpoint must not call any sync I/O reachable from `ImGui::*`. The endpoint accepts the POST on a cpp-httplib worker thread, parses JSON, queues `MainThreadDispatcher::PostToMainThread([&]{ toastManager.Append(...); })`. No sync I/O reaches the UI thread; annotate the cpp-httplib accept-loop function with `/* PILLAR2_WORKER_ONLY */ // est-latency: <50ms` per the convention.
3. **Dispatcher drain** — **fires for Phase 4** — toast append posts back through `MainThreadDispatcher::Drain()`. Existing drain-cap budget applies; toast notifications are < 1/sec sustained.
4. **Visible-cue bucket-E harness** — **N/A** — Phase 4 adds an INVISIBLE-when-empty surface; no new sync-stall code path.
5. **Marker inventory** — **N/A** — no new `SMATCHET_UI_PERF_SCOPE` markers (HTTP endpoint runs on worker; UI-thread toast append already inside `SMATCHET_UI_PERF_SCOPE("Toast::AppendOne")` per existing instrumentation).

**Pre-push local check**: for Phase 4 only, run [`docs/PERF_WORKFLOW.md`](../PERF_WORKFLOW.md) § Gate-check vs baseline (Step 7) against the chosen scenario before opening the PR.

**Override**: none anticipated; no perf regression expected from a localhost HTTP POST.

## Open design decisions — LOCKED 2026-05-21 evening grill

Five decisions locked via `grill-with-docs` pass before Phase 1 starts. Each affects the file surface above + the Phase 1 implementation shape.

1. **Registry location — `%LOCALAPPDATA%/Smatchet/merge-watch/active.json` (per-user)**. Single registry per machine, watches PRs across all Smatchet clones. Implication: each registry entry carries a `clone_path` field so the daemon knows where to `git pull --rebase` during cascade. Survives clone-deletion → orphaned-clone PRs surface in `merge-watch status` for explicit unregister. File-lock (`.lockfile` next to `active.json`) serializes multiple daemon-process accesses — though the foreground-default rule should make multi-daemon a rare misconfiguration anyway.
2. **Triage failure budget — 3 attempts** (2 retries + 1 surface). Tunable via `MERGE_WATCH_TRIAGE_BUDGET` env var (default 3). After budget exhausted: notify the user with the last attempt's CR-feedback delta + cumulative-attempt log; stop polling that PR until user `unregister`s + re-`register`s OR explicitly invokes `merge-watch retry-triage <pr>` (Phase 3 CLI extension — out of Phase 1 scope).
3. **Notification channel — Smatchet toast + Windows native (P0); webhooks deferred**. Two-prong: (a) Smatchet toast via existing `SmatchetToastManager` + Phase 4's localhost HTTP endpoint (only fires when Smatchet is running); (b) Windows native via BurntToast PowerShell module (or `New-BurntToastNotification` shell call) so user sees notifications even when Smatchet is closed. Webhook (Slack / Discord / Teams) deferred to a P1 follow-up once user-need surfaces.
4. **Daemon foreground default + `--background` opt-in**. `merge-watch daemon` runs in foreground (user sees structured stdout per poll cycle); `--background` opt-in detaches via Python `subprocess.Popen` with `creationflags=CREATE_NEW_PROCESS_GROUP` (Windows). State file (`%LOCALAPPDATA%/Smatchet/merge-watch/daemon.pid`) records PID for `merge-watch status --daemon` checks. Foreground catches failures early during dogfooding; opt-in detach matches the production shape once user trusts the loop.
5. **Explicit owner transfer on register**. `merge-watch register <pr>` prints "watcher now owns this PR; use `unregister` to take back control". Orchestrator NEVER auto-merges a registered PR (checks registry before any merge-gates poll). User can `merge-watch unregister <pr>` to take back; daemon stops polling on next cycle. Clean ownership boundary; no race between orchestrator + watcher + user.

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

## Verification

Per [`AGENTS.md`](../../AGENTS.md) § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: **N/A** for phases 1-5; the watcher is a Python script, not C++. Phase 3's classifier may have a Python doctest equivalent — `pytest scripts/dev/test_coderabbit_triage.py` covering the 18-rule override table + Smatchet-invariant rejection cases.
- **Bucket E (ImGui Test Engine)**: **N/A for phases 1-3 + 5**. Phase 4 only — `tests/ui/merge_watcher_toast_arrives.test.cpp` mirrors `tests/ui/views_columns_reorder.test.cpp`'s shape; posts a synthesised `{pr:999, state:CI_FAIL, message:"..."}` to the local endpoint, asserts toast appears in `SmatchetToastManager` within 1s, asserts toast text matches the message.
- **Bash-driver scenario / screenshot / sanitizer**: **bats** for phases 1-3 + 5 via `tests/bats/merge_watcher.bats`. Sanitizer build mandatory for Phase 4 (Pillar 3 requirement; HTTP endpoint is C++).
- **Build gate**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — only Phase 4 touches `Source_Core/`; other phases skip this gate (Python + bash only).
- **Manual residue**: zero expected. The watcher CLI is a user-driven interactive surface, but `merge-watch register` / `unregister` / `status` / `list` are all bats-coverable (CRUD + state-transition assertions); `daemon` mode is exercised by the bats integration test against `gh api`-mocked fixtures.

## Out of scope (flagged, not designed)

- **Subprocess-based CR triage (`claude --headless`)** — alternative shape if Phase 3's Python-port duplication-drift becomes a real concern. Cost: 2-5s spawn latency + per-invocation API charge. Switch path documented for future implementer — not built now.
- **MCP-tool-based CR triage (against user's running session)** — third alternative. Requires Smatchet running + open Claude Code session. Most-coupled option; deferred until a concrete user-need surfaces.
- **In-app watcher panel (`SmatchetMergeWatcherUi.cpp`)** — visual surface in Smatchet showing watched PRs + state, mirroring `SmatchetMcpServerUi.cpp`'s shape. Deferred — phase 4's toast handles surface; full panel waits for "I have N watched PRs and want to see them all at a glance" user need.
- **Auto-revert on post-merge regression** — if a merged PR breaks `develop`'s CI, watcher could auto-revert. Out of scope — too dangerous to default-on; needs a separate plan.
- **`merge-watch unregister --reason=<text>`** — audit-trail every unregister with a reason string for later analysis of why users abandoned watcher-owned PRs. Deferred — not P0.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run; named PR-fast scenario for Phase 4)*
