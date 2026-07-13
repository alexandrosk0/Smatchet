// keybindings_editor_rebind.test.cpp — bucket-E (ImGui Test Engine) coverage
// for the rebindable keyboard-shortcut feature's visible UX (per
// docs/plans/shipped/keyboard-shortcuts-rebindable.md).
//
// Two real-UI assertions, each driven against the LIVE app (no replica window):
//
//   A. EditorTabRendersWithLiveConflict — seed a deliberate collision into the
//      live keybinding table (app.dock_debug.toggle rebound onto Ctrl+B, which
//      view.sidebar.primary already owns by default), open Preferences, click
//      the "Keyboard Shortcuts" tab, and YieldUntil the editor reports itself
//      active (d.preferencesActiveTab == Keybindings). Tab-active means
//      BeginTabItem returned true → DrawKeybindingsPreferencesTab's body ran for
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
// WHY A TEST SEAM RATHER THAN CLICKING THE EDITOR WIDGETS: the editor's mutating
// controls (the per-row capture control, the "Reset all to defaults" button below a
// 320px scroll-table) sit in the docked Preferences window's clipped content region,
// which this repo's bucket-E suite documented as unreliable for ItemClick (see
// funcsize_preferences_tabs.test.cpp). Driving the SAME MarkKeybindingsDirty() the
// editor calls — via SmatchetUiTestMarkKeybindingsDirty() (Ui/SmatchetUI.cpp,
// SMATCHET_BUILD_UI_TESTS only) — covers the cache-rebuild integration without
// fighting the clipped widget. The constituent pure logic (SetBindingHotkey upsert,
// FindKeybindingConflict, the ParseImGuiHotkey/StringifyImGuiHotkey round-trip,
// MatchHotkey) also has bucket-A coverage (tests/Core/KeybindingsConfig.test.cpp +
// tests/Core/ImGuiHotkey.test.cpp).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController
#include "Config/KeybindingsConfig.h"          // KeybindingsConfig, SetBindingHotkey
#include "SmatchetUiSession.h"                 // UiDrawSession, PreferencesActiveTab, g_ui
#include "Ui/SmatchetHotkeyCapture.h"          // smatchet::ui::FindKeybindingConflict

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>

// g_ui — the shared bag of UI-thread visibility flags + the live TrackerConfig
// (g_ui.cfg). We set showPreferences / requestPreferencesFocus and read
// preferencesActiveTab / showDockDebug exactly as the real View menu / dispatch
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

        // The Keyboard Shortcuts tab carries a locale-stable ###id suffix
        // ("Keyboard Shortcuts###prefsTabKeybindings"), so the header ref resolves
        // regardless of UI language (ImHashStr restarts at ###). A tab header is
        // addressed tab-bar-qualified: "<TabBarID>/<label>".
        const char* tabRef = "PreferencesTabs/Keyboard Shortcuts###prefsTabKeybindings";
        const bool tabExists = ctx->ItemExists(tabRef);
        IM_CHECK_NO_RET(tabExists);
        if (tabExists) {
            ctx->ItemClick(tabRef);
            // Tab-active is the editor's own signal: DrawKeybindingsPreferencesTab
            // sets d.preferencesActiveTab = Keybindings inside its BeginTabItem body,
            // so this is true only once the full body (incl. the conflict-render
            // branch above) has ticked. Locale-independent — set by code, not a label.
            const bool active =
                YieldUntil(ctx, [] { return g_ui.preferencesActiveTab == PreferencesActiveTab::Keybindings; });
            IM_CHECK_NO_RET(active);
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

} // namespace

extern "C" void SmatchetRegisterKeybindingsEditorRebindTests(ImGuiTestEngine* engine) {
    RegisterEditorTabRendersWithLiveConflict(engine);
    RegisterDefaultComboDispatchesToCommand(engine);
    RegisterZoomComboAdjustsFontSize(engine);
    RegisterRebindThenNewComboFires(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
