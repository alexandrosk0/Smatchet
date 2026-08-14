// keybindings_editor_rebind.test.cpp — bucket-E (ImGui Test Engine) coverage
// for the rebindable keyboard-shortcut feature's visible UX (per
// docs/plans/shipped/keyboard-shortcuts-rebindable.md).
//
// Cases A–D drive the LIVE app; E–F host the capture widgets in a floating replica window (the
// SmatchetTest::… pattern) to reach the editor interior the docked Preferences window clips:
//
//   A. EditorTabRendersWithLiveConflict — seed a deliberate collision into the
//      live keybinding table (app.dock_debug.toggle rebound onto Ctrl+B, which
//      view.sidebar.primary already owns by default), open Preferences, select
//      the "Keyboard Shortcuts" category via its nav-rail Selectable, and
//      YieldUntil g_ui.preferencesCategory == Keybindings. Since slice 2a the
//      dispatch switch draws the selected category's body every frame, so
//      category-selected means DrawKeybindingsPreferencesTab's body ran for
//      several frames WITH a live conflict present, so the per-row
//      FindKeybindingConflict() + the "conflicts with <action>" TextColored
//      branch + the searchable table + the System-shortcuts CollapsingHeader all
//      ticked. A Begin/End or Push/PopID imbalance in any of those paths would
//      trip an ImGui IM_ASSERT trapped by the engine and fail the test. This is
//      the plan's "the conflict warning shows on a colliding bind" line, covered
//      at the render-without-crash level.
//
//   B. DefaultComboDispatchesToCommand — press the default Ctrl+Alt+D combo and
//      YieldUntil the public g_ui.showDockDebug flag flips. This exercises the
//      full live dispatch chain end-to-end: the dispatch cache (built from the
//      bindings at config load) → smatchet::ui::MatchHotkey → the unified command
//      registry Dispatch("app.dock_debug.toggle") → the observable UI flag. This
//      is the plan's "assert the combo fires the command" line.
//
//   D. RebindThenNewComboDispatches — the rebind→dispatch integration seam that
//      used to be the documented residue. Programmatically rebinds
//      app.dock_debug.toggle onto a fresh Ctrl+Alt+Shift+K, forces the dispatch-
//      cache rebuild via the new SmatchetUiTestMarkKeybindingsDirty() seam (the
//      editor's own ui.MarkKeybindingsDirty() trigger, exposed to bucket-E), then
//      presses the NEW combo and asserts g_ui.showDockDebug flips — proving the
//      full SetBindingHotkey upsert → dirty → rebuildKeybindingCache → MatchHotkey
//      on the new combo → registry Dispatch loop, not just the default combos.
//
//   E. CaptureWidgetClickThenKeyCommits — hosts the shared DrawHotkeyRebindControl
//      (used by both the per-row editor + the quick-bind popup) in a floating replica
//      window, clicks its "Click to rebind" button to arm capture, presses a combo,
//      and asserts it commits (the captured string carries the pressed key + capture
//      ends). The click→arm→press→commit interior directly.
//
//   F. QuickBindPopupCaptureThenSetBinds — hosts the real QuickBindPopup in a replica
//      window, Opens it on an unbound command, captures a combo through its embedded
//      capture control, clicks "Set", and asserts the binding lands in the config —
//      the quick-bind modal's capture→Set interior end-to-end.
//
// WHY A/D USE A CODE SEAM AND E/F USE A REPLICA WINDOW: the editor's mutating controls
// (per-row capture control, the quick-bind popup body, "Reset all to defaults" below a
// 320px scroll-table) sit in the docked Preferences window's clipped content region,
// unreliable for ItemClick in this repo's headless suite (see funcsize_preferences_
// tabs.test.cpp). D drives the cache-rebuild integration through the same
// MarkKeybindingsDirty() the editor calls, exposed via SmatchetUiTestMarkKeybindingsDirty()
// (Ui/SmatchetUI.cpp, SMATCHET_BUILD_UI_TESTS only). E/F re-host the capture widgets at
// full height in a SmatchetTest::… window (option (b)), so the click-through capture path
// is exercised without the docked clipping. The constituent pure logic (SetBindingHotkey
// upsert, FindKeybindingConflict, the ParseImGuiHotkey/StringifyImGuiHotkey round-trip,
// MatchHotkey) also has bucket-A coverage (tests/Core/KeybindingsConfig.test.cpp +
// tests/Core/ImGuiHotkey.test.cpp).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController
#include "Config/KeybindingsConfig.h"          // KeybindingsConfig, SetBindingHotkey
#include "SmatchetUiSession.h"                 // UiDrawSession, PreferencesCategory, g_ui
#include "Ui/SmatchetHotkeyCapture.h"          // smatchet::ui::FindKeybindingConflict
// Private companion header of the Preferences split TUs — not on this target's include
// path (it is only ever included by its own sibling .cpp files), hence the explicit
// relative path. Needed for the DrawKeybindingCombosCellForTest bucket-E seam.
#include "../../Source/Core/src/Ui/SmatchetPreferencesUi_detail.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>
#include <vector>

// g_ui — the shared bag of UI-thread visibility flags + the live TrackerConfig
// (g_ui.cfg). We set showPreferences / requestPreferencesFocus and read
// preferencesCategory / showDockDebug exactly as the real View menu / dispatch
// paths do. Same handle the funcsize_preferences_tabs pilot drives.
extern UiDrawSession g_ui;

namespace {

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// The app's update-available modal owns NavWindow and nulls HoveredWindow, so no
// widget under it can be hovered, clicked or nav-activated — an ItemClick in a
// replica window silently never lands. BeginPopupModal closes on a false p_open,
// so clearing the flag every frame dismisses it for the run.
void DismissAppUpdateModal() { g_ui.appUpdateModalOpen = false; }

// Open Preferences and tick until its window is live (Active). Mirrors the pilot's
// docked-window open recipe: re-arm the focus latch every frame until the docked
// tab activates, since the draw fn consumes it in one frame.
bool OpenPreferences(ImGuiTestContext* ctx) {
    g_ui.showPreferences = true;
    g_ui.requestPreferencesFocus = true;
    ctx->SetRef("Preferences");
    return YieldUntil(ctx, [&] {
        g_ui.requestPreferencesFocus = true;
        return WindowIsLive("Preferences");
    });
}

// --- Test A: editor body renders with a live conflict --------------------
void RegisterEditorTabRendersWithLiveConflict(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Keybindings", "EditorTabRendersWithLiveConflict");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // Snapshot the live bindings so we restore them no matter how the test exits.
        const KeybindingsConfig original = g_ui.cfg.Keybindings;

        // Seed a deliberate collision: rebind app.dock_debug.toggle onto Ctrl+B,
        // which view.sidebar.primary owns by default. The editor reads the live
        // table each frame, so the conflict surfaces immediately — no dirty flag
        // needed (the dirty flag gates the *dispatch* cache, not the editor view).
        g_ui.cfg.Keybindings.SetBindingHotkey("app.dock_debug.toggle", "{}", "Ctrl+B");

        // Sanity (pure, pre-UI): the shared conflict check the editor renders from
        // reports the collision. Non-empty == Ctrl+B is double-bound.
        const std::string conflict = smatchet::ui::FindKeybindingConflict(g_ui.cfg.Keybindings.Bindings, "Ctrl+B",
                                                                          "app.dock_debug.toggle", "{}");
        IM_CHECK_NO_RET(!conflict.empty());

        const bool prefsLive = OpenPreferences(ctx);
        IM_CHECK_NO_RET(prefsLive);
        if (!prefsLive) {
            g_ui.cfg.Keybindings = original;
            g_ui.showPreferences = false;
            return;
        }

        // The Shortcuts rail Selectable carries a locale-stable ###id suffix
        // ("Shortcuts###prefsNavShortcuts"), so the ref resolves regardless of UI
        // language (ImHashStr restarts at ###). It lives inside the "PrefsNavRail"
        // child window → wildcard ref. Below the narrow-width threshold the rail is
        // a combo instead; fall back to selecting the category directly — same body
        // coverage.
        const char* railRef = "**/Shortcuts###prefsNavShortcuts";
        if (ctx->ItemExists(railRef)) {
            ctx->ItemClick(railRef);
        } else {
            ctx->LogInfo("nav rail item absent (combo mode) — selecting category directly");
            g_ui.preferencesCategory = PreferencesCategory::Shortcuts;
        }
        // Category-selected means the dispatch switch draws the Shortcuts body
        // (incl. the conflict-render branch above) every subsequent frame.
        // Locale-independent — an enum, not a label.
        const bool active = YieldUntil(ctx, [] { return g_ui.preferencesCategory == PreferencesCategory::Shortcuts; });
        IM_CHECK_NO_RET(active);
        for (int i = 0; i < 5; ++i) {
            ctx->Yield();
        }

        // Restore the seeded binding + close Preferences so sibling tests start clean.
        g_ui.cfg.Keybindings = original;
        g_ui.showPreferences = false;
        ctx->Yield();
    };
}

// --- Test B: a registry combo dispatches end-to-end ----------------------
void RegisterDefaultComboDispatchesToCommand(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Keybindings", "DefaultComboDispatchesToCommand");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // app.dock_debug.toggle's default combo (Ctrl+Alt+D) was parsed into the
        // dispatch cache at config load. It is ungated (no backend reachability
        // gate) and flips the public g_ui.showDockDebug — the cleanest observable.
        const bool before = g_ui.showDockDebug;
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_D);
        const bool flipped = YieldUntil(ctx, [&] { return g_ui.showDockDebug != before; });
        IM_CHECK_NO_RET(flipped);

        // Leave the dev overlay flag as we found it.
        g_ui.showDockDebug = before;
        ctx->Yield();
    };
}

// --- Test C: the Ctrl+= zoom combo dispatches end-to-end -----------------
// This is the plan's marquee root-cause regression ("Zoom In is an example"),
// which had TWO independent causes, both fixed by keybindings-menu-shortcuts-fix.md:
//   (1) ParseImGuiHotkey could not parse "=" / "-", so even a present Ctrl+= bind
//       never produced a MatchHotkey (fixed in ImGuiHotkey.cpp — see
//       tests/Core/ImGuiHotkey.test.cpp); and
//   (2) the new menu-shortcut defaults never reached an upgrading user at all,
//       because KeybindingsConfig::from_json REPLACES (not merges) the binding
//       table — so a stale on-disk config kept only its old binds (fixed by the
//       migrated_menu_shortcuts_v1 seed migration — see ConfigMigration.test.cpp).
// This test presses the real Ctrl+= combo and YieldUntil the observable
// g_ui.cfg.FontSizePt rises — exercising the punctuation-token parse → dispatch
// cache (built from the loaded+migrated bindings) → MatchHotkey → ui.zoom.in
// registry command → font-size field, the same end-to-end seam as Test B but over
// the punctuation key that was the actual bug.
void RegisterZoomComboAdjustsFontSize(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Keybindings", "ZoomComboAdjustsFontSize");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // ui.zoom.in nudges cfg.FontSizePt by +1, clamped to 8..32. Start from a
        // mid-range value so a single +1 is observable (and never clamp-swallowed).
        const int before = g_ui.cfg.FontSizePt;
        g_ui.cfg.FontSizePt = 16;
        ctx->Yield();

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Equal); // Ctrl+= — Zoom In default
        const bool grew = YieldUntil(ctx, [] { return g_ui.cfg.FontSizePt > 16; });
        IM_CHECK_NO_RET(grew);

        // Restore the user's original font size so sibling tests start clean.
        g_ui.cfg.FontSizePt = before;
        ctx->Yield();
    };
}

// --- Test C2: the zoom ALIAS combos dispatch too ---------------------------------
// The marquee acceptance for docs/plans/keybindings-multi-combo.md: one action, three
// alternative combos, each firing on its own. Ctrl+Shift+Equal is what "Ctrl and +"
// physically is on a US layout — the exact-modifier matcher treats it as a different
// keystroke from Ctrl+= (Test C), so before the alias set it did nothing at all. The
// keypad variant covers the same intent layout-independently. Both must survive the
// flatten-into-the-dispatch-cache path AND the per-action de-dup that guards it.
void RegisterZoomAliasCombosAdjustFontSize(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Keybindings", "ZoomAliasCombosAdjustFontSize");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        const int before = g_ui.cfg.FontSizePt;

        // Alias 2 of ui.zoom.in: Ctrl+Shift+= (the "Ctrl and +" keystroke).
        g_ui.cfg.FontSizePt = 16;
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal);
        const bool grewShifted = YieldUntil(ctx, [] { return g_ui.cfg.FontSizePt > 16; });
        if (!grewShifted) {
            ctx->LogError("Ctrl+Shift+= did not zoom in — the ui.zoom.in alias set never reached the "
                          "dispatch cache (or the shifted spec failed to parse)");
        }
        IM_CHECK_NO_RET(grewShifted);

        // Alias 3: the numeric keypad '+', layout-independent.
        g_ui.cfg.FontSizePt = 16;
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd);
        const bool grewKeypad = YieldUntil(ctx, [] { return g_ui.cfg.FontSizePt > 16; });
        if (!grewKeypad) {
            ctx->LogError("Ctrl+numpad-plus did not zoom in — the keypad token is missing from the "
                          "hotkey grammar or from the ui.zoom.in alias set");
        }
        IM_CHECK_NO_RET(grewKeypad);

        // Zoom OUT's keypad alias, so the widened set is not just an in-only change.
        g_ui.cfg.FontSizePt = 16;
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract);
        const bool shrank = YieldUntil(ctx, [] { return g_ui.cfg.FontSizePt < 16; });
        IM_CHECK_NO_RET(shrank);

        g_ui.cfg.FontSizePt = before;
        ctx->Yield();
    };
}

// --- Test D: rebind to a fresh combo, rebuild the cache, the NEW combo fires ---
// The rebind→MarkKeybindingsDirty→rebuildKeybindingCache→new-combo-dispatches integration seam —
// previously the documented residue: no accessor reached the live SmatchetUI to mark the dispatch
// cache dirty after a programmatic edit, so a test could only fire the DEFAULT combos (Tests B/C).
// SmatchetUiTestMarkKeybindingsDirty() (Ui/SmatchetUI.cpp, ui-tests only) is that seam. This closes
// the loop end-to-end: SetBindingHotkey upsert → dirty → rebuild → MatchHotkey on the NEW combo →
// registry Dispatch → observable flip.
void RegisterRebindThenNewComboFires(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Keybindings", "RebindThenNewComboDispatches");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        const KeybindingsConfig original = g_ui.cfg.Keybindings;
        const char* kNewCombo = "Ctrl+Alt+Shift+K";

        // Sanity: the fresh combo is unused by any other command, so a dispatch on it is
        // unambiguously our rebind (not a pre-existing owner firing).
        const std::string preConflict = smatchet::ui::FindKeybindingConflict(g_ui.cfg.Keybindings.Bindings, kNewCombo,
                                                                             "app.dock_debug.toggle", "{}");
        IM_CHECK_NO_RET(preConflict.empty());

        // Rebind app.dock_debug.toggle onto the fresh combo, then force the dispatch-cache rebuild
        // through the test seam (the editor would call this via ui.MarkKeybindingsDirty()).
        g_ui.cfg.Keybindings.SetBindingHotkey("app.dock_debug.toggle", "{}", kNewCombo);
        SmatchetUiTestMarkKeybindingsDirty();
        ctx->Yield(); // dispatchKeybindings sees dirty next frame → rebuilds with the new combo
        ctx->Yield();

        // Press the NEW combo: it must now flip showDockDebug. If the dirty→rebuild seam were
        // broken, the stale cache would still hold the default Ctrl+Alt+D and this press would no-op.
        const bool before = g_ui.showDockDebug;
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_K);
        const bool flipped = YieldUntil(ctx, [&] { return g_ui.showDockDebug != before; });
        if (!flipped) {
            ctx->LogError("Rebound combo did not dispatch — the rebind→MarkKeybindingsDirty→rebuild-"
                          "cache seam failed to route the new combo to app.dock_debug.toggle");
            IM_CHECK(false);
        }

        // Restore: bindings back to defaults + rebuild so siblings (B: Ctrl+Alt+D, C: Ctrl+=) see
        // the original cache; leave the dev-overlay flag as found.
        g_ui.showDockDebug = before;
        g_ui.cfg.Keybindings = original;
        SmatchetUiTestMarkKeybindingsDirty();
        ctx->Yield();
        ctx->Yield();
    };
}

// --- Test E: the capture widget's click→press→commit interior (replica window) ---
// DrawHotkeyRebindControl is the shared per-row / quick-bind capture control. Its click-to-arm →
// press-a-combo → commit path sits in the docked Preferences window's clipped content region under
// the real editor (unreliable for ItemClick — see the header, option (b)). Hosting it in a floating
// full-height replica window (the repo's SmatchetTest::… pattern) drives that interior directly.
struct CaptureWidgetState {
    std::string display;
    bool capturing = false;
    std::string out;
    bool committed = false;
};
CaptureWidgetState g_captureState;

void RegisterCaptureWidgetClickCaptureCommit(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Keybindings", "CaptureWidgetClickThenKeyCommits");
    t->GuiFunc = [](ImGuiTestContext*) {
        DismissAppUpdateModal();
        ImGui::SetNextWindowSize(ImVec2(420.0f, 140.0f), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::HotkeyCaptureWidget")) {
            if (smatchet::ui::DrawHotkeyRebindControl("captureTest", g_captureState.display, g_captureState.capturing,
                                                      g_captureState.out)) {
                g_captureState.committed = true;
            }
        }
        ImGui::End();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_captureState = CaptureWidgetState{};
        ctx->SetRef("SmatchetTest::HotkeyCaptureWidget");
        // Arm capture: click the "Click to rebind" button (id "…##rebindcaptureTest").
        ctx->ItemClick("**/Click to rebind##rebindcaptureTest");
        const bool armed = YieldUntil(ctx, [] { return g_captureState.capturing; }, 60);
        IM_CHECK_NO_RET(armed);
        if (armed) {
            // Press a fresh combo; CaptureImGuiHotkeyThisFrame stringifies it and commits.
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_J);
            const bool committed = YieldUntil(ctx, [] { return g_captureState.committed; }, 60);
            IM_CHECK_NO_RET(committed);
            IM_CHECK_NO_RET(!g_captureState.capturing);                         // capture ends on commit
            IM_CHECK_NO_RET(g_captureState.out.find('J') != std::string::npos); // captured our key
        }

        // The capture set is derived from the grammar (BindableImGuiKeys), so keys the
        // stringifier learned are capturable too. Before that, '=' and the keypad were
        // rejected outright — you could not bind Ctrl+= from the UI at all, even though
        // it shipped as a default.
        g_captureState = CaptureWidgetState{};
        ctx->Yield();
        ctx->ItemClick("**/Click to rebind##rebindcaptureTest");
        if (YieldUntil(ctx, [] { return g_captureState.capturing; }, 60)) {
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal);
            const bool committedEq = YieldUntil(ctx, [] { return g_captureState.committed; }, 60);
            IM_CHECK_NO_RET(committedEq);
            if (committedEq) {
                // Canonical form of the "Ctrl and +" keystroke.
                IM_CHECK_NO_RET(g_captureState.out == "Ctrl+Shift+=");
            }
        }

        g_captureState = CaptureWidgetState{};
        ctx->Yield();
        ctx->ItemClick("**/Click to rebind##rebindcaptureTest");
        if (YieldUntil(ctx, [] { return g_captureState.capturing; }, 60)) {
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd);
            const bool committedPad = YieldUntil(ctx, [] { return g_captureState.committed; }, 60);
            IM_CHECK_NO_RET(committedPad);
            if (committedPad) {
                IM_CHECK_NO_RET(g_captureState.out == "Ctrl+NumAdd");
            }
        }
    };
}

// --- Test F: the QuickBindPopup capture→Set writes the binding (replica window) ---
// The right-click "Set shortcut…" quick-bind modal (QuickBindPopup) hosts the same capture control
// plus Set/Clear; its interior was the other half of the residual. Host the real popup at full
// height, Open it on an unbound command, capture a combo, click Set, and assert the binding landed.
smatchet::ui::QuickBindPopup g_qbPopup;
bool g_qbChanged = false;

void RegisterQuickBindCaptureThenSet(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Keybindings", "QuickBindPopupCaptureThenSetBinds");
    t->GuiFunc = [](ImGuiTestContext*) {
        DismissAppUpdateModal();
        ImGui::SetNextWindowSize(ImVec2(460.0f, 220.0f), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::QuickBindHost")) {
            if (g_qbPopup.Draw(g_ui.cfg)) {
                g_qbChanged = true;
            }
        }
        ImGui::End();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const KeybindingsConfig original = g_ui.cfg.Keybindings;
        g_qbChanged = false;
        // Open on a command with NO current binding so the combo is unambiguous.
        const char* kCmd = "app.dock_debug.toggle";
        g_ui.cfg.Keybindings.RemoveBinding(kCmd, "{}");
        g_qbPopup.Open(kCmd, "Dock Debug", "{}");

        const char* kPopup = "Set shortcut###QuickBindPopup";
        const bool popupLive = YieldUntil(ctx, [&] { return WindowIsLive(kPopup); }, 90);
        IM_CHECK_NO_RET(popupLive);
        if (popupLive) {
            ctx->SetRef(kPopup);
            ctx->ItemClick("**/Click to rebind##rebindquickbind");
            ctx->Yield();
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_J);
            ctx->Yield();
            ctx->Yield();
            // Set applies the captured draft to the config + closes the popup.
            ctx->ItemClick("**/Set");
            const bool bound = YieldUntil(
                ctx, [&] { return g_qbChanged && g_ui.cfg.Keybindings.FindBindingIndex(kCmd, "{}") >= 0; }, 90);
            if (!bound) {
                ctx->LogError("QuickBind Set did not write the captured combo to the binding table — "
                              "the popup capture→Set interior regressed");
                IM_CHECK(false);
            }
        }

        // Restore: dismiss any lingering popup + put the bindings back.
        if (WindowIsLive(kPopup)) {
            ctx->KeyPress(ImGuiKey_Escape);
            ctx->Yield();
        }
        g_ui.cfg.Keybindings = original;
        ctx->Yield();
    };
}

// --- Test G: the combo-chip cell's add / remove / rebind-in-place interior --------
// The multi-combo editor row (docs/plans/keybindings-multi-combo.md) renders one
// clickable chip per alternative combo plus "+ Add". Like E/F this lives in the
// docked Preferences content region, so host the REAL cell in a floating replica
// window (SmatchetPreferencesUiDetail::DrawKeybindingCombosCellForTest) rather than
// reimplementing the layout here. Beyond the behaviour, the live tick is the guard on
// the chip loop's PushID/PopID balance and its DEFERRED erase — dropping a chip
// mid-loop would invalidate both the iteration and ImGui's id sequence, and the test
// engine traps the resulting IM_ASSERT.
struct ComboCellState {
    Keybinding row;
    std::vector<Keybinding> all;
    std::string capturingKey;
    bool mutated = false;
};
ComboCellState g_comboCell;

void RegisterComboChipsAddAndRemove(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Keybindings", "ComboChipsAddAndRemove");
    t->GuiFunc = [](ImGuiTestContext*) {
        DismissAppUpdateModal();
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(640.0f, 200.0f), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::KeybindingCombosCell")) {
            if (SmatchetPreferencesUiDetail::DrawKeybindingCombosCellForTest(
                    *app, g_comboCell.all, g_comboCell.row, "testrow", g_comboCell.capturingKey)) {
                g_comboCell.mutated = true;
            }
        }
        ImGui::End();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        g_comboCell = ComboCellState{};
        g_comboCell.row.CommandId = "ui.zoom.in";
        g_comboCell.row.ArgsJson = "{}";
        g_comboCell.row.Hotkeys.push_back("Ctrl+=");
        g_comboCell.row.Hotkeys.push_back("Ctrl+NumAdd");
        ctx->SetRef("SmatchetTest::KeybindingCombosCell");
        ctx->Yield();
        ctx->Yield();

        // Each combo renders as its own chip, labelled with the combo itself.
        IM_CHECK_NO_RET(g_comboCell.row.Hotkeys.size() == 2);

        // 1. "+ Add" arms capture and appends a THIRD combo (the set grows, the
        //    existing two are untouched).
        ctx->ItemClick("**/+ Add##rebindkbadd");
        const bool armedAdd = YieldUntil(ctx, [] { return !g_comboCell.capturingKey.empty(); }, 60);
        IM_CHECK_NO_RET(armedAdd);
        if (armedAdd) {
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_J);
            const bool added = YieldUntil(ctx, [] { return g_comboCell.row.Hotkeys.size() == 3; }, 60);
            IM_CHECK_NO_RET(added);
            if (added) {
                IM_CHECK_NO_RET(g_comboCell.row.Hotkeys[0] == "Ctrl+=");      // order preserved
                IM_CHECK_NO_RET(g_comboCell.row.Hotkeys[1] == "Ctrl+NumAdd"); // sibling untouched
                IM_CHECK_NO_RET(g_comboCell.mutated);
            }
        }

        // 2. Clicking a CHIP arms capture for that slot and replaces it IN PLACE —
        //    the one-click rebind the single-combo row always had, now per combo.
        g_comboCell.capturingKey.clear();
        ctx->Yield();
        ctx->ItemClick("**/Ctrl+=##rebindkbchip");
        const bool armedChip = YieldUntil(ctx, [] { return !g_comboCell.capturingKey.empty(); }, 60);
        IM_CHECK_NO_RET(armedChip);
        if (armedChip) {
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_Y);
            const bool replaced = YieldUntil(
                ctx, [] { return !g_comboCell.row.Hotkeys.empty() && g_comboCell.row.Hotkeys[0] != "Ctrl+="; }, 60);
            IM_CHECK_NO_RET(replaced);
            // Replaced, not appended: the slot count is unchanged.
            IM_CHECK_NO_RET(g_comboCell.row.Hotkeys.size() == 3);
        }

        // 3. A chip's "x" drops just that combo, leaving the rest of the set intact.
        const std::size_t beforeRemove = g_comboCell.row.Hotkeys.size();
        ctx->ItemClick("**/x###kbDropCombo");
        const bool removed =
            YieldUntil(ctx, [beforeRemove] { return g_comboCell.row.Hotkeys.size() == beforeRemove - 1U; }, 60);
        IM_CHECK_NO_RET(removed);
    };
}

} // namespace

extern "C" void SmatchetRegisterKeybindingsEditorRebindTests(ImGuiTestEngine* engine) {
    RegisterEditorTabRendersWithLiveConflict(engine);
    RegisterDefaultComboDispatchesToCommand(engine);
    RegisterZoomComboAdjustsFontSize(engine);
    RegisterZoomAliasCombosAdjustFontSize(engine);
    RegisterRebindThenNewComboFires(engine);
    RegisterCaptureWidgetClickCaptureCommit(engine);
    RegisterQuickBindCaptureThenSet(engine);
    RegisterComboChipsAddAndRemove(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
