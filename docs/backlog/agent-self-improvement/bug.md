# Agent self-improvement — bug

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-17 · code-review · [bug] · P1 — `CommandPaletteFuzzyScenario` flips `BackendHasBeenReachable=true` before `outErr` early-return guard; latch persists if OnStart errors
  Details: `Source_Core/src/Commands/Scenarios/CommandPaletteFuzzyScenario.cpp` snapshots `savedBackendReachable_` then flips `cfg.BackendHasBeenReachable=true` *before* the `outErr` early-return guard. If `OnStart` errors out early, `OnCancel`/`OnFinish` may never run → latch persists for the session.
  Concrete next action: move the flip *after* the error-return guard. Surfaced by retrospective code-review sweep on PR #146.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P1 — `Source_Core/include/AppController.h:660-693` asymmetric `override` keyword guarding under `SMATCHET_WITH_LUA_AUTOMATION`
  Details: Declarations themselves wrapped in `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` but some `override` keywords sit outside the guard. If `LUA_AUTOMATION=0` is ever exercised, compile break.
  Concrete next action: make `override` follow each declaration's guard. Surfaced by retrospective code-review sweep on PR #144.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source_Core/src/AiClientFactory.cpp:14,17,20` uses `new OpenAiClient()` wrapped in `unique_ptr` instead of `std::make_unique`
  Details: Violates AGENTS.md § Quality "no raw `new`/`delete` — use `std::unique_ptr` + `make_unique`".
  Concrete next action: rewrite three call sites to `std::make_unique<OpenAiClient>(...)`. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source_Core/include/AiSseParser.h:38` + `.cpp:101` — `partial_` member + `emitIfReady` is a stub no-op
  Details: Either dead code or unfinished — must be resolved before Phase B of the AI assistant work.
  Concrete next action: delete or wire up before Phase B. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source_Core/src/TicketSyncService.cpp:86` empty-fetch guard is permanent; legitimately empty cache never reconverges
  Details: A user who legitimately deletes the last ticket or filters all rows never reconverges (stale cache forever). The guard installed via the prior empty-fetch fix is unconditional.
  Concrete next action: timestamp + age-out, or require two consecutive empty full-syncs before allowing the wipe. Surfaced by retrospective code-review sweep on PR #139.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source_Core/src/PlaneIssueSearch.cpp:480` asymmetric vs `JiraIssueSearch.cpp:393` for empty-page handling
  Details: Jira requires `fetchedPages > 0`; Plane does not. The new TicketSyncService guard is the only thing standing between a zero-page Plane response and a wipe.
  Concrete next action: align Plane's empty-page handling with Jira's `fetchedPages > 0` predicate. Surfaced by retrospective code-review sweep on PR #139.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `LuaToJson` / `JsonToLua` + `kJsonToLuaMaxDepth` duplicated verbatim across `AppController_LuaBindingsCore.cpp` ↔ `AppController_LuaBindings.cpp`
  Details: File comment names the duplication as intentional (post-split keeps Core ImGui-free). Drift risk on the next marshalling change.
  Concrete next action: lift to a shared internal header reachable from both TUs. Surfaced by retrospective code-review sweep on PR #144.
  Status: open
  Last-reviewed: 2026-05-17
