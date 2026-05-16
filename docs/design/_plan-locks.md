# Plan-locks — parallel-plan coordination scratchpad

Single source of truth for **which design plan is currently writing which files**. Every multi-phase or multi-PR plan in `docs/design/` claims its in-flight write set here so concurrent plans (and their delegated agents) can spot collisions before they happen.

This file replaces ad-hoc "I'll edit the same file by accident" failures with an explicit, lightweight handshake. Append-only entries; status transitions in-place.

## Protocol

Every plan that ships in more than one PR (or that hands off to delegated agents) appends an entry here **before** the first edit and updates it on every state transition.

**Entry shape:**

```
### <plan-slug> · <slice-id> · status: <claimed|in-flight|shipped|on-hold|abandoned>

- **Branch**: `claude/<branch-name>` (or `feat/<branch-name>` for autonomous-plan branches)
- **Owner agent**: `<agent-name>` (or `orchestrator` if direct)
- **Originating plan**: [`docs/design/<plan>.md`](./<plan>.md) § <section>
- **Claimed write set** (paths the slice will edit / create / delete):
  - `<path 1>`
  - `<path 2>`
  - ...
- **Read-only adjacency** (paths the slice reads but does not edit — list when high overlap risk):
  - `<path>`
- **Started**: `<YYYY-MM-DD>`
- **Last update**: `<YYYY-MM-DD>` — `<one-line state change>`
- **Cleared by**: PR `#<number>` merged at `<sha>` (fill on `shipped` / `abandoned`).
```

**Pre-flight check — every orchestrator + every delegation packet:**

1. Read this file.
2. Compute the intersection of the new slice's planned write set with every `status: claimed | in-flight` entry below.
3. **Empty intersection** → append the new claim, proceed.
4. **Non-empty intersection** → STOP. Either:
   - Coordinate with the holding slice — adjust scope, sequence behind it, or pick a different slice.
   - Promote the conflict to the user via `AskUserQuestion` with the overlap inventoried.

Agent prompts must include the lock-file path explicitly. The standard wording added to `AGENTS.md` § Orchestrator delegation packet is: *"Read `docs/design/_plan-locks.md` first. Refuse if your write set overlaps an `in-flight` or `claimed` entry; surface to the orchestrator."*

**State transitions:**

- `claimed` — orchestrator added the entry, no commits yet on the branch.
- `in-flight` — at least one commit pushed; PR may or may not be open.
- `shipped` — PR merged; entry stays for ~one merge window then prunes.
- `on-hold` — entry retained without active work; downstream slices may pre-emptively claim.
- `abandoned` — branch dropped without merge; entry pruned immediately.

**Pruning:** `shipped` entries that are older than 14 days OR whose merge sha is already in `git log origin/develop` should be deleted in the next coordination PR. Keep this file shallow.

## In-flight entries

### backend-audit-trail-per-event-path · status: shipped

- **Branch**: `feat/audit-trail-per-event-path`
- **Owner agent**: orchestrator
- **Originating plan**: backlog entries 2026-05-16 `security-review` + `offline-sync` in [`docs/backlog/AGENT_SELF_IMPROVEMENT.md`](../backlog/AGENT_SELF_IMPROVEMENT.md)
- **Claimed write set**:
  - `Source_Core/src/BackendAuditTrail.cpp` (writer re-resolves `GetAuditFilePath()` per-event)
  - `tests/Source_Core/BackendAuditTrail.test.cpp` (add runtime-dir-change case; existing TEST_CASE workaround drops)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip)
- **Read-only adjacency**: `Source_Core/include/BackendAuditTrail.h`, `Source_Core/include/ConfigManager.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — merged via PR #108 at sha 98ed9ea.
- **Cleared by**: PR `#108` merged at `98ed9ea`.

### cached-ticket-types-header-split · status: in-flight

- **Branch**: `feat/cached-ticket-types-header-split`
- **Owner agent**: orchestrator
- **Originating plan**: backlog entry 2026-05-16 `test-rig · [infra]` in [`docs/backlog/AGENT_SELF_IMPROVEMENT.md`](../backlog/AGENT_SELF_IMPROVEMENT.md)
- **Claimed write set**:
  - `Source_Core/include/CachedTicketTypes.h` (NEW)
  - `Source_Core/include/LocalCacheManager.h`
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md`
- **Read-only adjacency**: 20 callers of `LocalCacheManager.h` (no code edit — re-include keeps API)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — PR #109 opened, awaiting merge.
- **Cleared by**: pending merge of [#109](https://github.com/alexandrosk0/Smatchet/pull/109).

### test-suite-expansion-completion · wave-A1 · callstack-adversarial-subcases · status: claimed

- **Branch**: `feat/test-callstack-adversarial`
- **Owner agent**: `test-rig`
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Carry-over A
- **Claimed write set**:
  - `tests/Source_Core/CallstackParser.test.cpp`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip on `code-review+security-review · [test]` entry)
- **Read-only adjacency**: `Source_Core/src/CallstackParser.cpp`, `Source_Core/include/CallstackParser.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — packet dispatched to `test-rig` (worktree isolated).
- **Cleared by**: TBD PR against `develop`.

### test-suite-expansion-completion · wave-A1 · p4blame-parse-tu-split · status: in-flight

- **Branch**: `feat/p4blame-parse-tu-split`
- **Owner agent**: `test-rig` (TU-split pre-authorised per AGENTS.md applied rule)
- **Originating plan**: [`docs/design/test-suite-expansion-completion.md`](./test-suite-expansion-completion.md) § Carry-over B
- **Claimed write set**:
  - `Source_Core/include/P4BlameParse.h` (NEW)
  - `Source_Core/src/P4BlameParse.cpp` (NEW)
  - `Source_Core/src/P4Blame.cpp` (call-site rewire of the four lifted helpers only — no semantic change)
  - `tests/Source_Core/P4BlameParse.test.cpp` (NEW)
  - `tests/CMakeLists.txt`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (status flip on `test-rig · [infra] — Phase 2 P4BlameParse deferred`)
- **Read-only adjacency**: `Source_Core/include/P4Blame.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — first commit pushed; PR pending against `develop`.
- **Cleared by**: TBD PR against `develop`.

### test-suite-expansion · phase 1 · status: in-flight

- **Branch**: `feat/test-phase-1-tracker-pure-logic`
- **Owner agent**: orchestrator (autonomous multi-phase mode per the plan's § Execution contract)
- **Originating plan**: [`docs/design/applied/test-suite-expansion.md`](./applied/test-suite-expansion.md) § Phase 1
- **Claimed write set**:
  - `Source_Core/include/IssueCreatePipelineHelpers.h` (NEW)
  - `Source_Core/src/IssueCreatePipeline.cpp`
  - `Source_Core/src/IssueCreatePipelineHelpers.cpp` (NEW)
  - `Source_Core/src/IssueDraft.cpp`
  - `Source_Core/src/TrackerFieldValueParser.cpp`
  - `tests/CMakeLists.txt`
  - `tests/Source_Core/IssueCreatePipeline.test.cpp` (NEW)
  - `tests/Source_Core/IssueDraft.test.cpp` (NEW)
  - `tests/Source_Core/TrackerFieldValueParser.extended.test.cpp` (NEW)
  - `tests/Source_Core/TrackerFieldValueUtils.test.cpp` (NEW)
  - `docs/backlog/AGENT_SELF_IMPROVEMENT.md`
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix)
- **Read-only adjacency**: `Source_Core/include/IssueDraft.h`, `Source_Core/include/TrackerFieldValueParser.h`, `Source_Core/include/IssueCreatePipeline.h`
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — PR `#103` opened against `develop`.
- **Cleared by**: pending merge of [#103](https://github.com/alexandrosk0/Smatchet/pull/103).

### test-suite-expansion · phases 2–9 · status: claimed

- **Branch**: TBD per phase (`feat/test-phase-{N}-{slug}`)
- **Owner agent**: orchestrator (autonomous; see [`docs/design/applied/test-suite-expansion.md`](./applied/test-suite-expansion.md) § Execution contract)
- **Originating plan**: [`docs/design/applied/test-suite-expansion.md`](./applied/test-suite-expansion.md) § Phases 2-9
- **Claimed write set** (umbrella claim — narrows per phase as each kicks off):
  - `Source_Core/src/TicketSyncService.cpp` (phase ~2 — `ApplyIssueFetchPack` tests)
  - `Source_Core/src/LocalCacheManager.cpp` (phase ~3 — `SaveTicket` transaction tests)
  - `Source_Core/src/ConfigManager*.cpp` (phase ~4 — config migration tests)
  - `Source_Core/src/CallstackParser.cpp` (phase ~5 — blame-path tests)
  - `Plugins/Mcp/**` (phase ~6 — JSON-RPC wire-protocol harness)
  - `Plugins/LuaConsole/**` (phase ~7 — Lua sandbox / timeout tests)
  - `tests/CMakeLists.txt` (every phase)
  - `tests/Source_Core/**` (every phase, new files)
  - `docs/design/applied/test-suite-expansion.md` (impl-log appendix per phase)
- **Read-only adjacency**: every Source_Core header used by tested code.
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — phase 1 in flight; downstream phases umbrella-claimed to block conflicting Track-B work on the same files.
- **Cleared by**: TBD.

### large-files-and-phase-2 · Track B (B1–B3 + fix-up) · status: on-hold

- **Branch**: TBD per slice (B1: `claude/offline-queue-icache-access`, B2: TBD, B3: TBD)
- **Owner agent**: `offline-sync` (B1, B2), `lua-binder` (B3)
- **Originating plan**: [`docs/design/applied/large-files-and-phase-2.md`](./applied/large-files-and-phase-2.md) § Track B
- **Reason on-hold**: overlapping write set with `test-suite-expansion` phases 2-9 (`TicketSyncService.cpp`, `ConfigManager.cpp`, `AppController.h`, `tests/CMakeLists.txt`). Resuming Track B before those test phases land would force a multi-way rebase that defeats both efforts.
- **Resume gate**: `test-suite-expansion` § Implementation log shows phase 9 shipped, OR the user explicitly green-lights an earlier resume with a narrower-than-umbrella write set.
- **Claimed write set on resume** (preview — re-asserted at resume time):
  - B1: `Source_Core/include/ICacheAccess.h` (NEW), `Source_Core/include/AppController.h`, `Source_Core/src/AppController.cpp`, `Source_Core/src/AppController_*.cpp` (rename `Cache` → `cache_`), `Source_Core/include/OfflineQueueService.h`, `Source_Core/src/OfflineQueueService.cpp`, `tests/CMakeLists.txt` (if rename cascades to test-rig-compiled sources)
  - B2: `Source_Core/include/ITicketSyncHost.h` (NEW), `Source_Core/include/AppController.h`, `Source_Core/src/AppController.cpp`, `Source_Core/include/TicketSyncService.h`, `Source_Core/src/TicketSyncService.cpp`
  - B3a-d: `Source_Core/include/LuaAutomationHost.h`, `Source_Core/src/LuaAutomationHost.cpp`, `Source_Core/src/AppController_LuaBindings.cpp`, `Source_Core/src/AppController_LuaStubs.cpp`, `Source_Core/include/AppController.h`, `Source_Core/src/AppController.cpp` (Lua state + worker thread + Phase-2 `ITrackerActions` interface)
- **Started**: 2026-05-16
- **Last update**: 2026-05-16 — held by user after parallel `test-suite-expansion` plan surfaced. Track A (5 mechanical splits) already shipped; Track B paused at the boundary.
- **Cleared by**: TBD (gate above).

## Shipped recent entries

(Pruned after ~14 days. Kept here briefly to give concurrent agents recent context.)

### large-files-and-phase-2 · Track A · status: shipped

- A5 → PR [#93](https://github.com/alexandrosk0/Smatchet/pull/93) (merged 2026-05-16)
- A3 → PR [#97](https://github.com/alexandrosk0/Smatchet/pull/97) (merged 2026-05-16)
- A2 → PR [#98](https://github.com/alexandrosk0/Smatchet/pull/98) (merged 2026-05-16)
- A1 → PR [#100](https://github.com/alexandrosk0/Smatchet/pull/100) (pending)
- A4 → PR [#101](https://github.com/alexandrosk0/Smatchet/pull/101) (pending)
- Plan revision → PR [#102](https://github.com/alexandrosk0/Smatchet/pull/102) (pending)
