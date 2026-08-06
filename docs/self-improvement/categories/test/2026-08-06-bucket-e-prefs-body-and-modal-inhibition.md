- 2026-08-06 · orchestrator · [test] · P2 — two undocumented bucket-E harness traps cost ~2h each this slice: (a) the **docked Preferences body stops being submitted** unless `g_ui.requestPreferencesFocus` is re-armed from `GuiFunc` on *every* frame, and (b) an open **`Update Available###AppUpdateAvailable`** modal silently blocks hover / nav / item-registration for every widget beneath it — including widgets in unrelated *floating replica* windows
  Details: Surfaced writing `tests/ui/prefs_search_filter.test.cpp` + `prefs_schema_coverage.test.cpp`
    (slice 3 of `preferences-ia-resegmentation-and-search`), and again re-triaging two pre-existing
    reds in `tests/ui/keybindings_editor_rebind.test.cpp` (tests E/F).
    (a) `beginPreferencesWindow` (`Source/Core/src/Ui/SmatchetPreferencesUi.cpp:185-204`) consumes the
    `requestPreferencesFocus` latch in one frame. Arming it from the test *coroutine* is not enough:
    `ItemExists` / `ItemClick` / `ItemInfo` yield frames internally, and those frames run un-armed, so
    `Begin()` returns false, the whole body stops drawing, and no ref ever resolves — the failure reads
    as "the widget does not exist" / "78 always-drawn descriptors were never drawn", not as "the window
    closed". `GuiFunc` runs on every frame while a test is active and is the only correct arming site.
    (b) The update modal owns `g.NavWindow` and nulls `HoveredWindow`. Symptoms are three different
    messages for one cause: a silently-never-landing `ItemClick` (hover check), `"Unable to set NavId"`
    on `ItemNavActivate`, and items not registering at all. It is **not** scoped to the window under the
    modal — keybindings E/F fail on widgets in their own `Begin("SmatchetTest::…")` replica windows.
    `ImGui::BeginPopupModal(name, &p_open, …)` closes when `*p_open == false`, so clearing
    `g_ui.appUpdateModalOpen` every frame dismisses it for the run; `ctx->PopupCloseAll()` clears
    leftovers from sibling tests but does **not** stop this one reopening.
    This supersedes the claim at `tests/ui/ai_prefs_autosave_flow.test.cpp:155-165` that Preferences
    sub-items are simply unreachable in bucket-E — they are reachable, with the two arms above.
    (c) Trap (b) has a **profile-level kill switch** that removes the whole failure class without any
    per-TU code: run the harness against an isolated `SMATCHET_USER_DATA` directory whose config
    carries `"update_check_enabled": false`, and the modal never opens in the first place. This is
    strictly better than per-test dismissal for local triage — the affected suite is much wider than
    the three TUs above (`AiPrefs*`, `AiPrefsTab*`, `AnnotatePrefs`, `Keybindings`, `Preferences` all
    fail intermittently on it), and a full `scripts/dev/test-all.sh` run without it produced 84 reds
    that were entirely environmental.
  Concrete next action: promote both to a shared bucket-E fixture rather than a copy in every TU
    (Pillar 5 — the arming block is already duplicated across three test TUs). Cheapest shape: a
    `tests/ui/prefs_test_fixture.h` exposing `ArmPreferencesFrame()` + `DismissAppUpdateModal()` for
    `GuiFunc` bodies, and a one-paragraph "docked windows and modals" section in
    `docs/agent-rules/` (or the bucket-E how-to) so the next author does not re-derive it. Est ~2h.
    Separately (and cheaper, ~30m): have the bucket-E runner seed its throwaway profile with
    `"update_check_enabled": false` by default, so trap (b) cannot reach any suite.
  Cross-ref: `tests/ui/prefs_search_filter.test.cpp` (the documented arming comment, lines 36-61);
    `tests/ui/prefs_schema_coverage.test.cpp`; `tests/ui/keybindings_editor_rebind.test.cpp`
    (`DismissAppUpdateModal()` in tests E/F); `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:185-204`
    (the focus latch); `Source/Core/src/Ui/SmatchetUI.cpp:164,174` (modal open sites);
    `docs/plans/shipped/preferences-ia-resegmentation-and-search.md` § Verification.
  Status: open
  Last-reviewed: 2026-08-06
