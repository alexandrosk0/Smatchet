# Plan — stop the MCP plugin restarting on every keystroke in Preferences (#2110)

> **Slug**: `fix-2110-mcp-keystroke-restart`
>
> **Status**: `active`

## Context

[Issue #2110](https://github.com/alexandrosk0/Smatchet/issues/2110) (`bug`, `P2`, `area:ui`): the
Integrations tab of Preferences tears down and re-binds the MCP HTTP listen socket **once per
character** typed into the "MCP auth token" field. `DrawMcpSectionBody`
(`Source/Core/src/Ui/SmatchetPreferencesUi.cpp`) runs a per-frame state diff of the five MCP
widget buffers against `d.cfg` and, when the diff trips, calls `ph->SyncMcpPluginWithConfig(app,
d.cfg)` **inline on that frame**. The disk write behind `MarkPrefsDirty` is debounced (~100 ms);
the plugin sync is not.

Reported harm: in-flight MCP requests killed mid-typing; a `TIME_WAIT` / port-steal race between
teardown and `bind_to_port` can leave no server listening at all; partial token prefixes reach the
config on disk; config visibly churns underneath concurrent MCP clients.

After this lands: typing a token or a port produces exactly **one** config commit and **one**
plugin sync, when the field stops being edited.

## Approach

Gate the commit on **"this field is not currently being edited"** — a *state* predicate composed
into the existing *state* diff, rather than converting the block to event-driven commits. A text
or numeric field that still holds keyboard focus has a **prefix** in its buffer; that prefix must
not reach `d.cfg`. Checkboxes are one event per change and stay ungated.

Both halves are gated per field: the dirty comparison **and** the assignment into `d.cfg`. Gating
only the comparison would still flush a half-typed token whenever some other field tripped the
diff.

`ImGui::IsItemActive()` reads the **last submitted item**, so it can only be sampled at a call
site whose widget is the last thing submitted. That holds for `ImGui::InputInt` — `EndGroup()`
copies a contained `ActiveId` into `LastItemData.ID` precisely so `IsItemActive()` works on a
whole group (`imgui.cpp`, "If the current ActiveId was declared within the boundary of our
group..."). It does **not** hold for `SmatchetSecretInputText`, which is compound and ends on a
`SmallButton` / `TextDisabled`; its editing state must be captured **inside** the helper,
immediately after the inner `ImGui::InputText`, and returned to the caller.

## Files to modify

| File | Change |
|---|---|
| `Source/Core/include/Ui/SmatchetSecretInput.h` | Add a defaulted `bool* outEditing = nullptr` out-param; set it from `ImGui::IsItemActive()` immediately after the inner `InputText`, before the `SameLine`/`SmallButton`/`TextDisabled` chain. Defaulted, so the other 8 call sites are untouched. |
| `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` | `DrawMcpSectionBody`: capture `tokenEditing` (via the new out-param) and `portEditing` (`IsItemActive()` after `InputInt`); gate the token + port terms of the dirty diff and their `d.cfg` assignments on `!editing`. |
| `docs/plans/active/fix-2110-mcp-keystroke-restart.md` | This plan. |

## Existing utilities reused

- `SmatchetSecretInputText` (`Source/Core/include/Ui/SmatchetSecretInput.h`) — extended, not forked.
- `ImGui::IsItemActive()` — the established repo commit-gate family; `IsItemDeactivatedAfterEdit()`
  is already used in `AnnotateAnalysisUi_Preferences.cpp` and `SmatchetViewsDashboardUi_widgets.cpp`.
- `MarkPrefsDirty` (`Source/Core/include/Ui/SmatchetUiSession.h`) — unchanged debounced disk write.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — a defaulted parameter plus five gated expressions; extracts and splits nothing.

## UX Pillar callouts

- **Pillar 1 (perf)**: strictly positive. Removes an unbounded-by-frame socket teardown + rebind
  from the UI draw path — one per keystroke today, at most one per edit-completion after.
- **Pillar 2 (UI never freezes)**: strictly positive, and the real prize. `SyncMcpPluginWithConfig`
  stops a plugin, closes a listen socket and re-binds it **synchronously on the UI thread**; today
  that runs on the same frame as a keypress. This does not make the call async — it stops it firing
  per character.
- **Pillar 3 (never crash)**: positive. Closes the "half-typed token ends with no MCP server
  listening" window (a bind race per rebind) and stops token prefixes reaching the sealed config.
- **Pillar 4 (accessibility)**: neutral — no layout, no focus order, no contrast change. The
  out-param carries no visual effect; the widget draws exactly as before.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

Diff touches `Source/Core/` (one header, one UI TU), so the gates are declared:

- **Scenario coverage**: no perf scenario drives the Preferences → Integrations tab, so the curated
  diff→scenario map yields no affected subset; a `perf-run.sh` delta would measure noise.
- **Budget**: the change is net-negative work per frame (one `IsItemActive()` bool read added; a
  socket teardown+rebind removed from the typing path). No new allocation, no new I/O, no new
  per-frame string work — `tokenBufStr` is constructed exactly as before.
- **Verdict**: no measurable regression risk against the 6.94 ms steady-state budget; a scenario
  run is not warranted. Re-declare in review if a reviewer disagrees.

## Risks / non-goals

- **Risk: a mid-edit value is lost if the app dies while the field still has focus.** Today the
  buffer commits per keystroke, so a hard quit mid-typing keeps the prefix; after this change the
  final value commits when focus leaves. This is inherent to every commit-on-edit-complete gate
  and is exactly what the Issue asks for. All ordinary exits are safe: clicking a checkbox, another
  tab, or the window's close button clears `ActiveId` on that frame **while the section body still
  draws**, so the commit fires. Escape is consumed by `InputText` (reverts + deactivates) before it
  reaches the window.
- **Non-goal: debouncing `SyncMcpPluginWithConfig` itself** (the Issue's second suggested-fix
  bullet). See § Deviations.
- **Non-goal: the other 8 `SmatchetSecretInputText` call sites.** Tracker / Assistant / Whisper
  credential fields are Save-button gated and drive no per-frame sync; they keep the default
  `nullptr` and behave identically.
- **Non-goal: the `InputInt` clamp-to-[1,65535] UX** (clearing the port field snaps it to `1`).
  Pre-existing, unrelated to the restart storm, and out of scope.

## Verification

1. `cmake --build` the standalone target — no new warnings (warnings-as-errors).
2. `bash scripts/dev/test-all.sh` — no regression.
3. `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` — all delta gates green.
4. Manual (repro from the Issue): launch with MCP enabled, Preferences → Integrations, type a
   multi-character token — `debug.mcp_status` shows **zero** stop/rebind/start cycles while typing,
   and **exactly one** after focus leaves the field.
5. Manual: same for "MCP Port" — per-digit restarts gone.
6. Manual: toggling each of the three checkboxes still syncs immediately (one cycle per toggle).

## Out of scope (flagged, not designed)

- Making `SyncMcpPluginWithConfig` non-blocking on the UI thread (it stops a plugin and re-binds a
  socket synchronously). Worth a Pillar-2 look independently of the keystroke storm.

## Implementation log

_(filled post-implementation)_

## Deviations from plan

_(filled post-implementation)_

## Verification (actual)

_(filled post-implementation)_

## Archive (post-ship — DO IN THIS PR, never a follow-up)

Flip § Status to `shipped` and `git mv` this file to `docs/plans/shipped/` in the same PR that
fills the post-ship sections.
