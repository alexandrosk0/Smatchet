# Lua recorded ImGui command list — v2 follow-ups
<!-- index-summary: Stub tracking v2 follow-ups. B1 (per-window predicates) + C1 (focus-aware InputText) graduated to active plans; A1/D1/A2/E1/F1 still open, B2 subsumed by B1. -->

> **Status**: stub. Tracks the v2 follow-ups enumerated in [`lua-recorded-cmd-list.md`](lua-recorded-cmd-list.md) §Out-of-scope. **B1** and **C1** have graduated to active plans ([per-window predicates](lua-window-dirty-predicates.md), [focus-aware InputText](lua-focus-aware-inputtext.md)); **B2** is subsumed by B1. The rest each have a short context block + open questions and are not scoped to ship yet. Flesh out into a separate `docs/plans/active/<slug>.md` when work starts on a specific item.

## Context

The v1 cached cell + window bindings ([PR #66](https://github.com/alexandrosk0/Smatchet/pull/66), squash `5b740e9`) shipped with a deliberately small recorder vocabulary (12 ops) and a hard-coded auto-dirty contract. The follow-ups below all extend that surface or relax the contract along a single axis. They are independent — pick any one without affecting the others. The grouping below is by scope-of-change, not priority.

## A. Recorder vocabulary

### A1. `combo`, `drag_int / drag_float`, `slider_*`, `checkbox`, `radio`, `tree_node`

Mechanical extension. Each maps to the matching `ImGui::*` call at replay with a sandboxed callback for the commit predicate. `combo` is the most invasive — needs a list-of-strings backing buffer in `ImCmd`, similar to `InputText`'s `textBuf`.

**Open questions:**
- One `ImCmd::Op` enum per widget, or a single `Op::Widget` with a sub-tag and union'd fields? The latter keeps `ImCmd` smaller but pushes branching from the switch to a sub-switch.
- `tree_node` is hierarchical — recorder needs nesting state (`tree_pop` op). Decide whether nesting is by paired ops (`tree_push` / `tree_pop`) or by closure (`draw:tree_node(label, function(draw) … end)`). Closure is cleaner; paired ops match every other op.

### A2. `draw:button(label, on_click, { invalidate=false })` — chrome buttons

Currently every click marks the window dirty (auto-re-record next frame). For chrome buttons that don't change rendered state (e.g. "Help"), this is wasted re-record cost. Add an `invalidate=false` option-table arg.

**Open questions:**
- Same option for `input_text` commit? Probably yes by symmetry.
- Should `on_deactivated*` carry the same flag? It already doesn't auto-dirty (per plan §Q6), so no.

## B. Cache-invalidation refinement

### B1. Per-window dirty predicates

> **Graduated** → [`docs/plans/lua-window-dirty-predicates.md`](lua-window-dirty-predicates.md) (active). Open questions below are resolved there (zero-arg hashable signature; predicate runs only on data-gen-bump frames, not per-frame).

`luaWindowDataGen_` bumps invalidate **every** window. Most windows only care about a subset of state — a "ticket detail" window cares only about the focused ticket, a "global counts" window cares only about list size. Per-window predicate that the runner consults before deciding to dirty:

```lua
ui.register_window("Detail", {
  predicate = function() return smatchet.focused_ticket_id() end,
}, function(draw) … end)
```

Runner stashes the predicate's last return value, bumps dirty only when the return changes between gen-bumps.

**Open questions:**
- Predicate signature: zero-arg returning a hashable, or two-arg `(prev_gen, cur_gen)` returning bool?
- How to keep the predicate itself cheap (it runs every frame)?

### B2. `ui.register_window(name, { auto_invalidate = false }, fn)`

> **Subsumed by B1** — resolved. `auto_invalidate=false` is the limit case of B1 with a constant-return predicate (`predicate = function() return "" end`): a predicate whose value never changes never re-records on a `luaWindowDataGen_` bump. The B1 plan ([`docs/plans/lua-window-dirty-predicates.md`](lua-window-dirty-predicates.md)) ships one mechanism, not two; do **not** implement B2 separately.

Opt-out of `luaWindowDataGen_` auto-dirty (plan Q2 option d). Window only re-records on `ui.invalidate_window` or callback fire. For static config / info panels that don't display ticket data.

## C. Focused-cell semantics

### C1. Focus-aware InputText invalidation

> **Graduated** → [`docs/plans/lua-focus-aware-inputtext.md`](lua-focus-aware-inputtext.md) (active). Both open questions below are resolved there (stamp the active cell key from inside replay via `IsItemActive`, so no `GetActiveID`→key reverse lookup is needed; focus-drop commits the draft before re-record).

While a cached `InputText` cell has keyboard focus, a background sync that flips `rawValue` clobbers the in-progress edit. v1 documents this as a known limitation. Fix: when the cache-key comparison detects only `rawValue` changed AND the cell currently owns ImGui keyboard focus, skip re-record this frame. Defer until focus drops.

**Open questions:**
- How to detect "this cell owns focus" from outside the replay path? `ImGui::GetActiveID()` works but maps an ImGui ID, not the `ticket.id + fieldId` key. May need to capture the active-cell key as a side-effect of the replay loop.
- What if user clicks away during the deferred-invalidation window? The buffer holds stale typing. Probably fine — the user's draft is preserved until they explicitly cancel.

## D. Animation / cadence

### D1. `register_ticket_action`-style timer calling `ui.invalidate_window` on cadence

A Lua-side timer scheduled by the event loop: `ui.invalidate_window_every(name, ms)`. The runner stashes a deadline per window; once `ImGui::GetTime() >= deadline`, mark dirty and reset.

**Open questions:**
- Where does the deadline live — `LuaWindowEntry` or a side map? Side map is cleaner since cadence is a registration-time property, not a runtime one.
- Cadence + B1 predicate interaction: predicate wins, cadence only marks dirty if predicate-changed-since-last-bump.

## E. Refactor

### E1. Promote `LuaImmediateModeGuard` + `LuaHookGuard` → shared `LuaScopedExec`

Currently two separate RAII guards. If a third Lua entry point needs them, fold both into one `LuaScopedExec` helper that installs the hook + flips the immediate-mode flag in one `unique_ptr`-like scope.

**Trigger:** wait for the third entry point. Refactoring two for the sake of it is churn.

## F. Surprise contracts

### F1. Button / InputText `{ exclusive = true }` opt — suppress double-fire

Currently `draw:button(label, on_click)` + chained `draw:on_deactivated(fn)` both fire on the same frame (documented in `LUA_GUIDE.md` after the v1 round). If a real script ships and the double-fire surprises the author, add `{ exclusive = true }` to suppress the primary callback when `on_deactivated*` attached, or to suppress `on_deactivated*` when the primary fires same-frame.

**Trigger:** wait for first real complaint. Premature otherwise.

## Triage

Single-author priority:

1. ~~**B1 (per-window predicate)**~~ — **graduated** to [`docs/plans/lua-window-dirty-predicates.md`](lua-window-dirty-predicates.md).
2. ~~**C1 (focus-aware InputText)**~~ — **graduated** to [`docs/plans/lua-focus-aware-inputtext.md`](lua-focus-aware-inputtext.md).
3. **A1 (recorder vocabulary expansion)** — unblocks scripted forms / dashboards. Pick the 2-3 widgets users actually ask for, not all six.
4. **D1 (cadence timer)** — narrow utility; defer until a real need surfaces.
5. **A2 / E1 / F1** — quality-of-life or speculative; do not pre-implement. (**B2** is subsumed by B1 — see above.)

## Out-of-scope (v3)

- Replacing `LuaDrawList` with a proto-bufferable wire format for cross-process replay (would unlock MCP-driven UI).
- GPU-accelerated replay (vertex buffer once recorded, no per-frame ImGui call).
- Recorder-level diff replay (replay only the ops that changed between record N and N+1).
