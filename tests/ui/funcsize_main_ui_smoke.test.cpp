// funcsize_main_ui_smoke.test.cpp — Phase-0 GREEN batch 2 of the
// full-function-size-compliance program (docs/plans/shipped/full-function-size-compliance.md).
//
// PURPOSE — STRUCTURAL coverage for four large draw functions slated for
// decomposition in Phase 5. Each test boots the REAL app (via UiTestScenario)
// and drives the target function so it ticks for several frames; the ImGui Test
// Engine traps any in-frame IM_ASSERT (a Begin/End or Push/PopID imbalance a
// future decomposition might introduce) and fails the test. There is NO
// checked-in pixel golden — the live tick IS the coverage.
//
// Targets covered here:
//   1. SmatchetUI::drawMainMenuBar (SmatchetUI_MainMenu.cpp, ~504L) — the main
//      menu bar, drawn EVERY frame (not a toggle window). Coverage is the tick:
//      we boot, confirm the "View" menu resolves in the "##MainMenuBar" window,
//      and open it (MenuClick) so its submenu bodies submit. ZenMode defaults
//      off, so the bar draws every frame.
//   2. SmatchetUI::Draw (SmatchetUI.cpp, ~608L) — the root UI draw, runs every
//      frame. Booting the app under UiTestScenario AND finding the main menu bar
//      live already proves Draw ticked end-to-end (drawMainMenuBar is called
//      from inside Draw). This is folded into the menu-bar test — one combined
//      "main UI smoke" test rather than two redundant boots.
//   3. SmatchetToolbarUi::RenderEditor (SmatchetToolbarUi.cpp, ~288L) — the
//      toolbar EDITOR (a BeginPopupModal). Opened via the View-menu item
//      "Customize Toolbar..." which calls toolbar_.OpenEditor() — the exact
//      production path. Driving the menu item exercises drawMainMenuBar AND
//      RenderEditor in one flow. The editor renders config-backed widgets only
//      (it lists app.Commands().All(), valid at boot) — ZERO project/ticket
//      backend, so it is green.
//   4. SmatchetUI::drawBulkImportWindow (SmatchetBulkTicketsUi.cpp, ~399L) — a
//      docked top-level window. Owns its own ImGui::Begin("Bulk import tickets")
//      and the only early return before Begin is the !showBulkImport visibility
//      guard (which we satisfy by setting the flag). After Begin it submits the
//      "Load file" / "Paste clipboard" toolbar unconditionally with no backend,
//      so it is green and reuses the docked-window focus-re-arm recipe.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"
#include "SmatchetToast.h" // toast history: the observation seam for a refused toolbar button

#include "ui_test_skip.h" // SmatchetUiTestIsHeadlessSoftwareGl

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>
#include <vector>

// g_ui — the shared bag of UI-thread visibility flags. ViewToggleCommands.cpp
// flips these same fields to open/close panels; we set them directly here.
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

// --- Main menu bar + root Draw -------------------------------------------
// drawMainMenuBar (SmatchetUI_MainMenu.cpp) is called from SmatchetUI::Draw
// (SmatchetUI.cpp) every frame. The main menu bar lives in ImGui's reserved
// "##MainMenuBar" window. We assert that window is live (proving Draw +
// drawMainMenuBar both ran), then MenuClick the "View" menu so its submenu
// body submits its MenuItems — exercising the menu-bar draw path past the
// top-level BeginMenu into a nested body.
void RegisterMainMenuBarRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeMainUi", "MainMenuBar_RendersAndOpensViewMenu");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // The main menu bar draws every frame (ZenMode defaults off). Confirm
        // its reserved window is live — this proves SmatchetUI::Draw ran end to
        // end and called drawMainMenuBar.
        const bool barLive = YieldUntil(ctx, [&] { return WindowIsLive("##MainMenuBar"); });
        IM_CHECK_NO_RET(barLive);

        if (barLive) {
            // Open the View menu so drawMainMenuBar's nested BeginMenu("View")
            // body submits. MenuClick resolves the absolute path into the main
            // menu bar window. A missing menu or an in-draw IM_ASSERT fails here.
            ctx->MenuClick("//##MainMenuBar/View");
            ctx->Yield();
            // Close the menu so subsequent tests start from a clean state.
            ctx->KeyPress(ImGuiKey_Escape);
            ctx->Yield();
        }
    };
}

// --- Toolbar editor (Customize Toolbar...) -------------------------------
// SmatchetToolbarUi::RenderEditor (SmatchetToolbarUi.cpp) is a BeginPopupModal
// that renders only once toolbar_.OpenEditor() has set requestEditorOpen_. The
// production trigger is the View-menu item "Customize Toolbar..." — clicking it
// exercises drawMainMenuBar AND RenderEditor in one flow. The modal title is
// "Customize Toolbar###SmatchetToolbarEditor" (the "###" stable-ID suffix built by
// SmatchetLocalization::WindowTitle so the popup ID survives title translation);
// once live, the editor submits its
// config-backed body (no project/ticket backend) and the engine traps any
// in-draw IM_ASSERT.
// The modal's title carries the "###SmatchetToolbarEditor" suffix the production
// BeginPopupModal uses (SmatchetLocalization::WindowTitle joins with "###" for a
// language-stable ID).
const char* const kToolbarEditorTitle = "Customize Toolbar###SmatchetToolbarEditor";

// Closes the editor through its Cancel button: Esc does not close a modal, and Save
// would persist the toolbar into later tests.
void CloseToolbarEditor(ImGuiTestContext* ctx) {
    if (!WindowIsLive(kToolbarEditorTitle)) {
        return;
    }
    ctx->SetRef(kToolbarEditorTitle);
    ctx->ItemClick("Cancel");
    YieldUntil(ctx, [] { return !WindowIsLive(kToolbarEditorTitle); }, 60);
}

// Opens the editor and leaves the ref on it. False (checked or logged) when the app is
// not booted or the editor never came up. An editor left open by an earlier case is
// closed first: it would keep that case's filter text and selection.
bool OpenToolbarEditor(ImGuiTestContext* ctx) {
    const AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
        return false;
    }

    const bool barLive = YieldUntil(ctx, [&] { return WindowIsLive("##MainMenuBar"); });
    IM_CHECK_NO_RET(barLive);
    if (!barLive) {
        return false;
    }
    CloseToolbarEditor(ctx);

    // Open the editor via the toolbar's right-click context menu, which has
    // a stable-text "Customize Toolbar..." item (the View-menu path can't be
    // kept latched open across the headless engine's multi-frame menu
    // navigation in this app; the toolbar's own ImGui::Button is glyph-
    // labelled with a non-deterministic id). Right-click the toolbar window's
    // void area to open "##ToolbarCtx" (BeginPopupContextWindow), then click
    // the leaf — both production code paths that set requestEditorOpen_.
    ctx->MouseMove("//##SmatchetToolbar");
    ctx->MouseClick(ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Customize Toolbar...");

    const bool editorLive = YieldUntil(ctx, [&] { return WindowIsLive(kToolbarEditorTitle); });
    IM_CHECK_NO_RET(editorLive);
    if (editorLive) {
        ctx->SetRef(kToolbarEditorTitle);
    }
    return editorLive;
}

void RegisterToolbarEditorRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeMainUi", "ToolbarEditor_RendersViaCustomizeToolbarMenu");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (OpenToolbarEditor(ctx)) {
            // Assert a stable in-body widget exists — the editor submits a
            // "Save" button at its footer unconditionally once Begin succeeds,
            // confirming the body ran past BeginPopupModal. Bare ref against the
            // SetRef'd modal window.
            const bool saveExists = ctx->ItemExists("Save");
            IM_CHECK_NO_RET(saveExists);
        }
        // Dismiss the modal so subsequent tests start clean.
        CloseToolbarEditor(ctx);
    };
}

bool ToolbarEditorSaveDisabled(ImGuiTestContext* ctx) {
    return (ctx->ItemInfo("Save").ItemFlags & ImGuiItemFlags_Disabled) != 0;
}

// Adds a Command button to the open editor; the editor selects it, so its fields show
// whatever the saved toolbar holds. Cancel drops it again. False (checked) when the
// field `ref` did not come up.
bool AddToolbarCommandButton(ImGuiTestContext* ctx, const char* ref) {
    ctx->ItemClick("**/Add command");
    const bool fieldShown = YieldUntil(ctx, [&] { return ctx->ItemExists(ref); }, 60);
    IM_CHECK_NO_RET(fieldShown);
    return fieldShown;
}

// Args that would not run block Save, and valid args give it back.
void RegisterToolbarEditorArgsValidation(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeMainUi", "ToolbarEditor_InvalidArgsBlockSave");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenToolbarEditor(ctx)) {
            CloseToolbarEditor(ctx);
            return;
        }
        const char* kArgs = "**/Args (JSON)";
        if (!AddToolbarCommandButton(ctx, kArgs)) {
            CloseToolbarEditor(ctx);
            return;
        }
        IM_CHECK_NO_RET(!ToolbarEditorSaveDisabled(ctx));

        ctx->ItemInputValue(kArgs, "{'a':1}"); // single quotes: not JSON
        ctx->Yield(2);
        IM_CHECK_NO_RET(ToolbarEditorSaveDisabled(ctx));

        ctx->ItemInputValue(kArgs, "{\"a\": 1}");
        ctx->Yield(2);
        IM_CHECK_NO_RET(!ToolbarEditorSaveDisabled(ctx));

        ctx->ItemInputValue(kArgs, "[1]"); // JSON, but not an object
        ctx->Yield(2);
        IM_CHECK_NO_RET(ToolbarEditorSaveDisabled(ctx));

        CloseToolbarEditor(ctx);
    };
}

// The command filter matches like the palette: any case, letters in order, trimmed.
void RegisterToolbarEditorCommandFilter(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeMainUi", "ToolbarEditor_CommandFilterIsFuzzy");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenToolbarEditor(ctx)) {
            CloseToolbarEditor(ctx);
            return;
        }
        const char* kFilter = "**/##cmdsearch";
        const char* kCommand = "**/view.list";
        if (!AddToolbarCommandButton(ctx, kFilter)) {
            CloseToolbarEditor(ctx);
            return;
        }

        ctx->ItemInputValue(kFilter, "VIEW.LIST");
        ctx->Yield(2);
        const bool upperCaseMatches = ctx->ItemExists(kCommand);
        IM_CHECK_NO_RET(upperCaseMatches);

        ctx->ItemInputValue(kFilter, "  vwlst ");
        ctx->Yield(2);
        const bool lettersInOrderMatch = ctx->ItemExists(kCommand);
        IM_CHECK_NO_RET(lettersInOrderMatch);

        ctx->ItemInputValue(kFilter, "zzzzqq");
        ctx->Yield(2);
        const bool noMatchHidesIt = !ctx->ItemExists(kCommand);
        IM_CHECK_NO_RET(noMatchHidesIt);

        CloseToolbarEditor(ctx);
    };
}

// How many notices in the history carry `title` as a warning.
int CountWarnings(const SmatchetToastManager& toasts, const char* title) {
    int n = 0;
    for (const ToastHistoryEntry& entry : toasts.History()) {
        if (entry.Type == ToastType::Warning && entry.Title == title) {
            ++n;
        }
    }
    return n;
}

// A toolbar button whose saved args are not a JSON object is not run: one warning says so.
// A second click while that warning is up adds no second one, and the button's menu offers
// no shortcut for it.
// The probe has no icon, so its label is the first word of its tooltip, and it sits first
// in the bar. The toolbar is put back at the end; nothing here saves it.
void RegisterToolbarBadArgsButtonNotRun(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeMainUi", "Toolbar_BadArgsButtonIsNotRun");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        CloseToolbarEditor(ctx);
        const ToolbarConfig saved = g_ui.cfg.Toolbar;
        ToolbarButton probe;
        probe.Kind = ToolbarButtonKind::Command;
        probe.Tooltip = "BadArgsProbe";
        probe.CommandId = "view.list";
        probe.ArgsJson = "{'a':1}"; // single quotes: not JSON
        g_ui.cfg.Toolbar.Visible = true;
        g_ui.cfg.Toolbar.Buttons.insert(g_ui.cfg.Toolbar.Buttons.begin(), probe);
        SmatchetToastManager& toasts = SmatchetToastManager::Instance();
        toasts.ClearHistory();

        const char* kProbe = "//##SmatchetToolbar/BadArgsProbe##tb0";
        const bool shown = YieldUntil(ctx, [&] { return ctx->ItemExists(kProbe); }, 60);
        IM_CHECK_NO_RET(shown);
        if (shown) {
            ctx->ItemClick(kProbe);
            ctx->Yield(2);
            IM_CHECK_EQ_NO_RET(CountWarnings(toasts, "Toolbar button not run"), 1);
            ctx->ItemClick(kProbe);
            ctx->Yield(2);
            IM_CHECK_EQ_NO_RET(CountWarnings(toasts, "Toolbar button not run"), 1);

            ctx->ItemClick(kProbe, ImGuiMouseButton_Right);
            const char* kSetShortcut = "//$FOCUSED/Set shortcut...";
            const bool menuShown = YieldUntil(ctx, [&] { return ctx->ItemExists(kSetShortcut); }, 60);
            IM_CHECK_NO_RET(menuShown);
            if (menuShown) {
                const bool shortcutOffered = (ctx->ItemInfo(kSetShortcut).ItemFlags & ImGuiItemFlags_Disabled) == 0;
                IM_CHECK_NO_RET(!shortcutOffered);
            }
            ctx->PopupCloseAll(); // the button's menu
            ctx->Yield(2);
        }
        g_ui.cfg.Toolbar = saved;
        toasts.DismissAllLive(); // the warning would sit over a later case's click target
        ctx->Yield(2);
    };
}

// A shortcut carries its command's args, and one set from a toolbar button copies that button's.
// Args that are not a JSON object are refused on the key as they are on the button.
void RegisterKeybindingBadArgsNotRun(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeMainUi", "Keybinding_BadArgsShortcutIsNotRun");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        CloseToolbarEditor(ctx);
        const std::vector<Keybinding> saved = g_ui.cfg.Keybindings.Bindings;
        Keybinding probe;
        probe.CommandId = "view.list";
        probe.Hotkeys.push_back("Ctrl+Alt+Shift+F9");
        probe.ArgsJson = "[1]"; // JSON, but not an object
        g_ui.cfg.Keybindings.Bindings.push_back(probe);
        SmatchetUiTestMarkKeybindingsDirty();
        ctx->Yield(2);
        SmatchetToastManager& toasts = SmatchetToastManager::Instance();
        toasts.ClearHistory();

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_F9);
        ctx->Yield(2);
        bool refused = false;
        for (const ToastHistoryEntry& entry : toasts.History()) {
            // The notice names the command and says where the shortcut is cleared.
            refused = refused || (entry.Type == ToastType::Warning && entry.Title == "Shortcut not run" &&
                                  entry.Message.find("view.list") != std::string::npos &&
                                  entry.Message.find("Preferences") != std::string::npos);
        }
        IM_CHECK_NO_RET(refused);
        // The same key again while that notice is up: refused again, and not announced again.
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_F9);
        ctx->Yield(2);
        IM_CHECK_EQ_NO_RET(CountWarnings(toasts, "Shortcut not run"), 1);

        g_ui.cfg.Keybindings.Bindings = saved;
        SmatchetUiTestMarkKeybindingsDirty();
        toasts.DismissAllLive();
        ctx->Yield(2);
    };
}

// --- Bulk import window --------------------------------------------------
// SmatchetUI::drawBulkImportWindow (SmatchetBulkTicketsUi.cpp). Title "Bulk
// import tickets". Docked top-level window opened via g_ui.showBulkImport +
// g_ui.requestBulkImportFocus, so it reuses the focus-re-arm recipe exactly
// like the Log/Audit/Preferences pilot. Once Begin() succeeds the body submits
// the "Load file" button unconditionally (no backend), so that label is the
// robust child probe.
void RegisterBulkImportWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeMainUi", "BulkImportWindow_RendersAndShowsLoadFile");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        if (SmatchetUiTestIsHeadlessSoftwareGl()) {
            // Under llvmpipe the docked window never goes live within the yield
            // budget (deterministic on every CI run; passes on real GL).
            ctx->LogInfo("SKIP: render/timing-dependent under headless software GL (see ui_test_skip.h)");
            return;
        }

        g_ui.showBulkImport = true;
        g_ui.requestBulkImportFocus = true;
        ctx->SetRef("Bulk import tickets");
        const bool visible = YieldUntil(ctx, [&] {
            g_ui.requestBulkImportFocus = true;
            return WindowIsLive("Bulk import tickets");
        });
        IM_CHECK_NO_RET(visible);

        if (visible) {
            const bool itemPresent = ctx->ItemExists("Load file");
            IM_CHECK_NO_RET(itemPresent);
        }

        g_ui.showBulkImport = false;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterFuncSizeMainUiSmokeTests(ImGuiTestEngine* engine) {
    RegisterMainMenuBarRenderSmoke(engine);
    RegisterToolbarEditorRenderSmoke(engine);
    RegisterToolbarEditorArgsValidation(engine);
    RegisterToolbarEditorCommandFilter(engine);
    RegisterToolbarBadArgsButtonNotRun(engine);
    RegisterKeybindingBadArgsNotRun(engine);
    RegisterBulkImportWindowRenderSmoke(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
