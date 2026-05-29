# Per-entry invalidation for the Lua field-display cache

The `register_field_display_cached` cache (introduced in [lua-recorded-cmd-list.md](../plans/shipped/lua-recorded-cmd-list.md)) keys validity off **per-entry inputs** — `rawValue`, `fieldName`, `quantAvailWidth`, `readOnly`, `allowEdits`, `providerGen` — not a single global epoch. A single-cell edit invalidates only that one cell; unrelated cells keep their recorded `ImCmd` lists and replay without invoking Lua.

## Considered options

- **Global `fieldValueCacheEpoch_` bumped on every mutation.** Simpler — one atomic; trivial cache check. Rejected: any single-cell edit re-records every visible cell next frame, which both burns the recorded-cmd-list savings (one Lua call per cell per edit, not per refresh) and wipes any in-progress `InputText` buffer in an unrelated cell.
- **Hash the provider closure's upvalues each frame.** Detects arbitrary Lua-state reads automatically. Rejected: re-walking upvalues every frame defeats the cache's whole point.
- **Bump a global gen on every Lua callback / script event.** Re-records all cells whenever any callback fires. Rejected — same thrash problem, just spread across more triggers.

## Consequences

- Providers that read Lua state **outside** the 6 documented args (theme tables, user prefs, action-mutated globals) must call `ui.invalidate_field_cache()` (or `ui.invalidate_field_cache_for(ticket_id [, field_id])`) after mutating that state — the cache cannot detect it.
- `luaProviderGen_` is the only global gen affecting cells; it bumps on `register_field_display_cached` / `_by_name` (un)registration to invalidate cached closures.
- Verification has an explicit per-cell-independence step (edit ticket A's `priority`; confirm ticket B's `priority` and ticket A's `summary` providers are not re-invoked). A future contributor "simplifying" back to a global epoch will fail that test.
