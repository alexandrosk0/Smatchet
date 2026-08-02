# Plan — Per-window dirty predicates for cached Lua windows

> **Slug**: `lua-window-dirty-predicates` (matches this file's basename without `.md`).
>
> **Status**: `active`.
>
> Graduates item **B1** from the [v2 follow-up stub](docs/plans/lua-recorded-cmd-list-v2.md) into a scoped plan. Builds directly on the shipped [recorded-cmd-list feature](docs/plans/lua-recorded-cmd-list.md) (PR #66, `5b740e9`).

## Context

`AppController::NotifyLuaTicketDataChanged()` bumps a single global `luaWindowDataGen_` on every active-ticket state change (`AppController_LuaBindings_Ui.cpp:405`). `DrawLuaWindows` re-records **every** registered window whose `cachedDataGen` lags that counter (`AppController_LuaBindings_Draw.cpp:1219`). So a single-cell edit or a sync that touches one ticket re-runs the Lua draw fn of *all* windows, even ones whose content can't have changed — a "ticket detail" window that renders only the focused ticket re-records when an unrelated ticket's status flips.

For a user with one window this is free (record cost ≈ once per event). For a dashboard with N windows it is N × record-cost per event, on the UI thread. The record cost is the whole reason the recorded-cmd-list cache exists; paying it for windows that didn't change is the one remaining avoidable cost in the window path.

**Intended outcome — after this lands:** a window registered with a `predicate` re-records on a data-gen bump **only when the predicate's return value changed since the last record**, instead of on every bump. Windows without a predicate keep today's behaviour exactly.

## Approach

Add an optional `predicate` to window registration: `ui.register_window(name, { predicate = fn }, draw_fn)`. The predicate is a **zero-arg Lua function returning a hashable value** (nil / bool / number / string / table). The runner stashes the predicate's last return (canonicalised to a string via the existing `LuaToJson(...).dump()` leaf) on the `LuaWindowEntry`. On a frame where only the data-gen changed (not `dirty`, not a provider-gen change), the runner evaluates the predicate once; if the stringified return equals the stored one, it advances `cachedDataGen` **without** re-recording. Any other trigger (`dirty` from a `Click`/`Commit` callback or `ui.invalidate_window`, or a `providerGen` change) still forces a record unconditionally — the predicate only gates the *data-gen* path.

Resolving the stub's two open questions:
- **Signature** → zero-arg returning a hashable, **not** `(prev_gen, cur_gen) → bool`. The gens are opaque monotonic counters with no meaning to a script; a script can only make a useful decision from *its own* view of app state (`smatchet.focused_ticket_id()`, list size, …), which is exactly a zero-arg accessor.
- **"How to keep the predicate cheap since it runs every frame?"** → it does **not** run every frame. It runs at most once per window per *data-gen-bump* frame — i.e. only on sync/edit events, never in steady-state replay. This is the key correction to the stub's stated assumption. A predicate that returns a constant string therefore reduces to "never auto-invalidate on data changes", which subsumes stub item **B2** (`auto_invalidate = false`) — so B2 should be dropped rather than built separately (see § Out of scope).

## Files to modify

1. `Source/Core/include/AppController_LuaTypes.h:78` — add to `LuaWindowEntry`: `sol::protected_function predicate;`, `std::string lastPredicateValue;`, `bool hasPredicate;` (default `false`). Add `sol::protected_function predicate;` to `PendingLuaWindowOp` (`:91`) so the registration op can carry it through the deferred-op queue.
2. `Source/Core/src/AppController_LuaBindings_Ui.cpp` — three touch points:
   - `LuaUiRegisterWindowGlue` (`:203`) — change signature to `(sol::this_state, const std::string& name, sol::object second, sol::optional<sol::object> third)` and dispatch: if `second` is a function → legacy `(name, drawFn)`; if `second` is a table → `(name, opts, drawFn=third)`, reading `opts["predicate"]` when it is a function.
   - `LuaUiRegisterWindowBind` (`:327`) — accept the parsed `predicate` and store it on the `PendingLuaWindowOp`.
   - `ApplyOrQueueLuaWindowOp` Register case (`:306`) — copy `op.predicate` into the new `LuaWindowEntry`, set `e.hasPredicate = op.predicate.valid()`.
3. `Source/Core/src/AppController_LuaBindings_Draw.cpp` — the decision logic:
   - New file-scope helper `EvalWindowPredicate(sol::state&, LuaWindowEntry&) → std::string` in the anonymous namespace: calls `w.predicate()` under `LuaHookGuard` + `LuaImmediateModeGuard(false)`, catches C++/Lua errors (on error returns a sentinel that forces a record + surfaces via the existing error sink), coerces the first return through `LuaToJson(obj).dump()`.
   - `DrawLuaWindows` (`:1207`) / the `needRecord` computation (`:1219`) — split into: `dirty || providerChanged` forces record; a data-gen-only change consults the predicate (see § Design below).
   - `RecordLuaWindow` (`:1147`) — after a successful record, prime `w.lastPredicateValue` (reuse the value already computed in the decision when available; avoid a second eval).
4. `Source/Core/src/AppController_LuaBindings_detail.h:159` — update the `LuaUiRegisterWindowGlue` forward declaration to the new signature.
5. `Source/Core/src/AppController_LuaBindings.cpp:254` — `ui.set_function("register_window", &...LuaUiRegisterWindowGlue)` is unchanged (sol binds the new signature by address); no edit unless the address-of overload needs disambiguation.
6. `Source/Core/src/AppController_LuaBindingsCore.cpp:310` — the no-Lua `register_window` no-op is a generic `noop`; confirm it still swallows the extra arg (it does — generic sink). No behavioural change.
7. `docs/guides/lua.md:94` — document the options-table form and the predicate contract.
8. `scripts/SmatchetHooks.lua` — add one commented example window using `predicate = function() return smatchet.focused_ticket_id() end`.
9. `docs/plans/lua-recorded-cmd-list-v2.md` — mark B1 as graduated (link here); drop B2 (subsumed).

## Design (decision logic)

```cpp
const bool providerChanged = w.cachedProviderGen != curProviderGen;
const bool dataChanged     = w.cachedDataGen != curDataGen;
bool needRecord = w.dirty || providerChanged;

std::string predValue;
bool predEvaluated = false;
if (!needRecord && dataChanged) {
    if (w.hasPredicate && w.predicate.valid()) {
        predValue = EvalWindowPredicate(lua, w);   // one Lua call, hook-guarded
        predEvaluated = true;
        if (predValue != w.lastPredicateValue)
            needRecord = true;                      // relevant state changed → record
        // else: data changed but this window doesn't care → fall through, just advance gen
    } else {
        needRecord = true;                          // no predicate = legacy "any data change dirties"
    }
}

if (needRecord) {
    RecordLuaWindow(w, curDataGen, curProviderGen); // sets cachedDataGen/ProviderGen, clears dirty
    if (w.hasPredicate)
        w.lastPredicateValue = predEvaluated ? std::move(predValue)
                                             : EvalWindowPredicate(lua, w); // prime for next bump
} else if (dataChanged) {
    w.cachedDataGen = curDataGen;                   // predicate-skip: advance gen so we don't
    if (predEvaluated)                              // re-run the predicate until the NEXT bump
        w.lastPredicateValue = std::move(predValue);
}
```

**Invariant**: the predicate is evaluated at most once per window per frame, and only on a frame where `luaWindowDataGen_` advanced past this window's `cachedDataGen`. Steady-state (no bump) frames evaluate nothing and replay the cached list, exactly as today. Advancing `cachedDataGen` in the skip branch is load-bearing — without it the predicate would re-run every frame until the next bump.

**Error handling**: an erroring predicate returns the force-record sentinel → the window records this frame (fail-safe: a broken predicate degrades to today's always-record behaviour, never to a stale window), and the error is surfaced through the same `errorSinks_` / scripting-window path `RecordLuaWindow` already uses on a draw-fn error.

## Existing utilities reused

- `LuaHookGuard` / `LuaImmediateModeGuard` — `AppController_LuaBindings_detail.h:71,62` — bound instruction count + block `imgui.*` while the predicate runs, same as every other Lua entry point.
- `LuaToJson` — `Json/LuaJsonConvert.h` (via detail header `:17`) — canonical stringify of the predicate return; already handles nil/bool/number/string/table.
- `RecordLuaWindow` — `AppController_LuaBindings_Draw.cpp:1147` — unchanged record + negative-cache + error-surface path; the plan only adds the `lastPredicateValue` prime after it.
- `ApplyOrQueueLuaWindowOp` — `AppController_LuaBindings_Ui.cpp:288` — the mid-iteration-safe register/unregister/invalidate path the predicate rides through unchanged.

## Extraction sizing

N/A — this plan adds a field + one helper; it extracts/splits nothing.

## UX Pillar callouts

- **Pillar 1 (perf)**: net win. Predicate windows skip record on irrelevant bumps; the added cost is one hook-guarded Lua call per window only on bump frames (bounded, not per-frame). Non-predicate windows are byte-for-byte unchanged.
- **Pillar 2 (UI-thread never blocks)**: the predicate runs on the UI thread inside `DrawLuaWindows`, bounded by the 100000-instruction `LuaHookGuard`. It introduces no new sync I/O; a predicate that itself does I/O is the author's error, same exposure as an existing window draw fn (and no worse — it runs *less* often than the draw fn it gates).
- **Pillar 3 (never crash)**: predicate C++/Lua errors are caught and fail-safe to "record this frame" + surfaced through the existing error sink. No new throw crosses the sol2 boundary unguarded.
- **Pillar 4 (accessibility)**: N/A — no rendered-surface or keyboard-nav change.

## Perf-review-system gates

Diff touches `Source/Core/` → gates fire.

1. **PR-fast CI** — no existing scenario exercises multi-window record cost. Add a bucket-E/scenario `lua-window-predicate` (N windows, one predicate window; bump `luaWindowDataGen_` via a single-cell edit; assert only the predicate-changed window re-records) and register it in `scripts/dev/perf-pr-fast-set.json`. Until then, `lua-recorder-fuzz` covers the record/replay path for regressions.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`. N/A.
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()`; registration still rides the existing dispatcher hop in `LuaUiRegisterWindowBind`. N/A.
4. **Visible-cue bucket-E harness** — adds no >100 ms sync path. N/A.
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers (predicate eval reuses the `LuaWindow::Record` scope region conceptually but adds none). N/A.

## Risks / non-goals

- **Predicate cost on huge N** — a pathological predicate (deep table return, heavy compute) run per window per bump could add up. Mitigation: hook-guarded, and it runs strictly less often than the draw fn it replaces; documented as "keep predicates to a cheap accessor + comparison."
- **Stringify collisions** — two distinct app states that `LuaToJson().dump()` to the same string would be treated as "unchanged." Accepted: the predicate contract is "return a value that changes iff the window's inputs change"; canonical JSON of nil/bool/number/string is collision-free, and table ordering is deterministic in `LuaToJson`.
- **Non-goal**: per-*cell* predicates. Cells already invalidate precisely off their own cache-key inputs (`TryRenderCachedLuaField`); predicates are a windows-only concept.
- **Non-goal**: changing the default (no-predicate) behaviour. Backward compatibility is absolute — `register_window(name, fn)` is untouched.

## Verification

- **Bucket A (pure-logic ctest)**: extract the decision (`dirty`/`providerChanged`/`dataChanged`/`predicate-equal` → `record?` / `advance-gen?`) into a pure predicate function and table-test the truth matrix, including the "advance gen on skip" and "prime on record" transitions. No ImGui/sol needed.
- **Bucket E (ImGui Test Engine)**: register two windows (one with `predicate = focused_ticket_id`, one without); fire `NotifyLuaTicketDataChanged` via an unrelated single-cell edit; assert the non-predicate window's record count increments and the predicate window's does not; then change the focused ticket and assert the predicate window records exactly once.
- **Bash-driver scenario**: new `lua-window-predicate` scenario (above) for the perf-fast set; `lua-recorder-fuzz` re-run to confirm no regression in the shared record/replay path.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — the new `LuaWindowEntry` fields are behind `SMATCHET_WITH_LUA_AUTOMATION`; confirm the `-DSMATCHET_WITH_LUA_AUTOMATION=OFF` stub build still links).
- **Doc validation**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; record outcome.
- **Manual residue**: none intended — all buckets automatable.

## Out of scope (flagged, not designed)

**Deferral residue-sweep** — on finalising, grep `docs/plans/lua-recorded-cmd-list-v2.md` and update B1 (graduated) + B2 (subsumed) references.

- **B2 `auto_invalidate = false`** — subsumed: `predicate = function() return "" end` (constant return) never re-records on data-gen bumps, which is exactly B2. Drop B2 from the v2 stub rather than shipping a second mechanism (the stub itself flags "pick one mechanism, not both").
- **Predicate scheduling / cadence (stub D1)** — separate follow-up; a predicate is pull-on-bump, a cadence timer is push-on-clock. No interaction designed here.
- **Multi-value / dependency-list predicates** (`{ depends_on = {"ticket:123"} }`) — a declarative form could be sugar over the function predicate later; not designed.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
