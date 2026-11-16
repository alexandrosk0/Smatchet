// gesture_keys.test.cpp — bucket-E (ImGui Test Engine) coverage for the keyboard equivalents of pointer
// gestures (docs/plans/input-ux-w2-gesture-keys.md, audit § 4.6). Each case presses the real key through the
// live SmatchetUI::dispatchKeybindings -> command registry -> consumer chain; none calls a command or a
// handler directly.
//
//   A. CommandsRegisteredAndBound      the seven commands are registered and have a default binding.
//   B. ToolbarList_KeysMoveAndRemove   Alt+Down / Alt+Up reorder and Delete removes in the Customize Toolbar
//                                      list (#122), hosted from the real SmatchetToolbarUi.
//   C. McpSplitter_KeysResize          Shift+Alt+Down / Shift+Alt+Up move the MCP panel's divider.
//   D. PanelResize_KeysNudgeBottom     a rebound view.panel.grow / view.panel.shrink chord resizes the bottom
//                                      panel (#138).
//   E. Calendar_KeysPickDay            PageDown then Enter picks the day a month on; T then Enter picks today
//                                      (#44), through the real RenderGenericDatePicker.
//   F. PathRemaps_AddFocusesAndEnter   Add path remap puts the caret in the new row; Enter in the last filled
//                                      "to" adds the next row and focuses it (#130).
//   G. ReadOnlyText_CtrlAFromFocus     Ctrl+A selects a keyboard-focused run with the mouse elsewhere (#12).
//
// Each case ends by putting back what it touched (bindings, panel height, config value) so a sibling test starts
// from the same state. Mutation check (recorded in the plan's Verification): reverting the consumer's take, the
// command's post, the default binding or the focus branch turns the matching case red.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "ConfigManager.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "Tracker/TrackerDateTimeFieldEditor.h"
#include "Tracker/TrackerDateTimePure.h"
#include "Ui/SmatchetBottomPanelDrag.h"
#include "Ui/SmatchetMcpServerUi.h"
#include "Ui/SmatchetToolbarUi.h"
#include "Ui/SelectableTextRun.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern UiDrawSession g_ui;

// Seams in Ui/AnnotateAnalysisUi_Preferences.cpp (SMATCHET_BUILD_UI_TESTS only).
extern "C" void SmatchetUiTestDrawAnnotatePathRemaps();
extern "C" void SmatchetUiTestAnnotateRemapsBegin();
extern "C" void SmatchetUiTestAnnotateRemapsEnd();
extern "C" int SmatchetUiTestAnnotateRemapCount();

namespace {

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        if (pred()) {
            return true;
        }
        ctx->Yield();
    }
    return pred();
}

AppController* ArmApp(ImGuiTestContext* ctx) {
    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
    }
    return app;
}

// Rebinds `commandId` (args "{}" unless given) onto `chord` and waits for the dispatch cache to pick it up.
void Rebind(ImGuiTestContext* ctx, const char* commandId, const char* argsJson, const char* chord) {
    g_ui.cfg.Keybindings.SetBindingHotkey(commandId, argsJson, chord);
    SmatchetUiTestMarkKeybindingsDirty();
    ctx->Yield(2);
}

// --- A: registered and bound ------------------------------------------------------------------------------

const char* const kGestureCommands[] = {"ui.list.move_up",  "ui.list.move_down",  "ui.list.remove",
                                        "ui.splitter.grow", "ui.splitter.shrink", "view.panel.grow",
                                        "view.panel.shrink"};

void RegisterCommandsRegisteredAndBound(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GestureKeys", "CommandsRegisteredAndBound");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = ArmApp(ctx);
        if (app == nullptr) {
            return;
        }
        for (const char* id : kGestureCommands) {
            IM_CHECK_NO_RET(app->Commands().HasExact(id));
            // A binding row exists and is enabled, so the key does something out of the box.
            const int idx = g_ui.cfg.Keybindings.FindBindingIndex(id, "{}");
            IM_CHECK_NO_RET(idx >= 0);
            if (idx >= 0) {
                const Keybinding& b = g_ui.cfg.Keybindings.Bindings[static_cast<std::size_t>(idx)];
                IM_CHECK_NO_RET(b.Enabled);
                IM_CHECK_NO_RET(!b.PrimaryHotkey().empty());
            }
        }
        const int up = g_ui.cfg.Keybindings.FindBindingIndex("ui.list.move_up", "{}");
        if (up >= 0) {
            IM_CHECK_STR_EQ(g_ui.cfg.Keybindings.Bindings[static_cast<std::size_t>(up)].PrimaryHotkey().c_str(),
                            "Alt+Up");
        }
    };
}

// --- B: the toolbar editor list -----------------------------------------------------------------------------

struct ToolbarHost {
    SmatchetToolbarUi toolbar;
    TrackerConfig cfg;
    AppController* app = nullptr;
    bool armed = false;
};
ToolbarHost g_toolbarHost;

const char* ToolbarTitleRef() {
    return SmatchetLocalization::WindowTitle("toolbar.editor.title", "Customize Toolbar", "SmatchetToolbarEditor");
}

std::string Tips(const ToolbarHost& h) {
    std::string out;
    for (const ToolbarButton& b : h.toolbar.EditorButtonsForTest()) {
        out += b.Tooltip;
    }
    return out;
}

void SubmitToolbarHost(ImGuiTestContext*) {
    if (!g_toolbarHost.armed || g_toolbarHost.app == nullptr) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(700.0f, 560.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::GestureToolbarHost", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        g_toolbarHost.toolbar.DrawEditorOnlyForTest(*g_toolbarHost.app, g_toolbarHost.cfg);
    }
    ImGui::End();
}

void RegisterToolbarListKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GestureKeys", "ToolbarList_KeysMoveAndRemove");
    t->GuiFunc = SubmitToolbarHost;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = ArmApp(ctx);
        if (app == nullptr) {
            return;
        }
        ToolbarHost& h = g_toolbarHost;
        h.app = app;
        h.cfg = TrackerConfig();
        h.cfg.Toolbar.Buttons.clear();
        for (const char* tip : {"A", "B", "C"}) {
            ToolbarButton b;
            b.Kind = ToolbarButtonKind::Command;
            b.CommandId = "app.about.open";
            b.Tooltip = tip;
            h.cfg.Toolbar.Buttons.push_back(b);
        }
        h.armed = true;
        h.toolbar.OpenEditor();
        ctx->Yield(3);
        IM_CHECK_STR_EQ(Tips(h).c_str(), "ABC");

        const std::string modalRef = std::string("//") + ToolbarTitleRef();
        ctx->SetRef(modalRef.c_str());
        ctx->ItemClick("**/$$0/###row"); // select the first row; the list now has the keyboard
        ctx->Yield(2);
        IM_CHECK_EQ(h.toolbar.EditorSelectedForTest(), 0);

        ctx->KeyPress(ImGuiMod_Alt | ImGuiKey_DownArrow);
        YieldUntil(ctx, [&] { return Tips(h) == "BAC"; }, 20);
        IM_CHECK_STR_EQ(Tips(h).c_str(), "BAC");
        IM_CHECK_EQ(h.toolbar.EditorSelectedForTest(), 1);

        ctx->KeyPress(ImGuiMod_Alt | ImGuiKey_UpArrow);
        YieldUntil(ctx, [&] { return Tips(h) == "ABC"; }, 20);
        IM_CHECK_STR_EQ(Tips(h).c_str(), "ABC");
        IM_CHECK_EQ(h.toolbar.EditorSelectedForTest(), 0);

        // The keys act on the selected row, as the Up / Down / Delete buttons do: select the middle row, remove it.
        ctx->ItemClick("**/$$1/###row");
        ctx->Yield(2);
        IM_CHECK_EQ(h.toolbar.EditorSelectedForTest(), 1);
        ctx->KeyPress(ImGuiKey_Delete);
        YieldUntil(ctx, [&] { return Tips(h) == "AC"; }, 20);
        IM_CHECK_STR_EQ(Tips(h).c_str(), "AC");

        // Alt+Up on the first row has nowhere to go: nothing moves, nothing throws.
        ctx->ItemClick("**/$$0/###row");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiMod_Alt | ImGuiKey_UpArrow);
        ctx->Yield(4);
        IM_CHECK_STR_EQ(Tips(h).c_str(), "AC");

        h.armed = false;
        ctx->Yield(2);
        ImGui::ClosePopupsOverWindow(nullptr, false); // the modal belongs to this test's host; close it
        ctx->Yield();
    };
}

// --- C: the MCP panel's splitter ----------------------------------------------------------------------------

#if defined(SMATCHET_WITH_MCP)
bool g_mcpHostArmed = false;
AppController* g_mcpApp = nullptr;

void SubmitMcpHost(ImGuiTestContext*) {
    if (!g_mcpHostArmed || g_mcpApp == nullptr) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(640.0f, 900.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::GestureMcpHost", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        SmatchetDrawMcpServerPanel(*g_mcpApp, g_ui.cfg, g_ui);
    }
    ImGui::End();
}

void RegisterMcpSplitterKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GestureKeys", "McpSplitter_KeysResize");
    t->GuiFunc = SubmitMcpHost;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = ArmApp(ctx);
        if (app == nullptr) {
            return;
        }
        const float original = g_ui.cfg.McpServerInfoPanelHeightPx;
        g_ui.cfg.McpServerInfoPanelHeightPx = 300.0f; // a size well inside the clamp, so a step is not clamped away
        g_mcpApp = app;
        g_mcpHostArmed = true;
        ctx->Yield(3);
        ctx->WindowFocus("//SmatchetTest::GestureMcpHost");
        ctx->Yield(2);

        const float before = g_ui.cfg.McpServerInfoPanelHeightPx;
        ctx->KeyPress(ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_DownArrow);
        YieldUntil(ctx, [&] { return g_ui.cfg.McpServerInfoPanelHeightPx > before + 1.0f; }, 20);
        const float grown = g_ui.cfg.McpServerInfoPanelHeightPx;
        IM_CHECK_GT(grown, before + 1.0f);

        ctx->KeyPress(ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_UpArrow);
        YieldUntil(ctx, [&] { return g_ui.cfg.McpServerInfoPanelHeightPx < grown - 1.0f; }, 20);
        IM_CHECK_LT(g_ui.cfg.McpServerInfoPanelHeightPx, grown - 1.0f);

        g_mcpHostArmed = false;
        g_ui.cfg.McpServerInfoPanelHeightPx = original;
        ctx->Yield(2);
    };
}
#endif // SMATCHET_WITH_MCP

// --- D: the bottom panel ------------------------------------------------------------------------------------

float BottomPanelHeight() {
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(SmatchetDockNodeIds::kBottomPanel);
    return node != nullptr ? node->Size.y : 0.0f;
}

void RegisterPanelResizeKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GestureKeys", "PanelResize_KeysNudgeBottom");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = ArmApp(ctx);
        if (app == nullptr) {
            return;
        }
        const KeybindingsConfig original = g_ui.cfg.Keybindings;
        // The default chords resolve "focused"; this case names the panel, so it does not depend on what the test
        // engine focused last. It also proves the chord is rebindable.
        Rebind(ctx, "view.panel.grow", "{\"panel\":\"bottom\"}", "Ctrl+Alt+Shift+9");
        Rebind(ctx, "view.panel.shrink", "{\"panel\":\"bottom\"}", "Ctrl+Alt+Shift+8");

        const bool wasVisible = SmatchetBottomPanelDrag::IsPanelVisible();
        SmatchetBottomPanelDrag::Expand(g_ui, 0.0f);
        const bool visible = YieldUntil(ctx, [] { return SmatchetBottomPanelDrag::IsPanelVisible(); });
        IM_CHECK_NO_RET(visible);
        if (!visible) {
            g_ui.cfg.Keybindings = original;
            SmatchetUiTestMarkKeybindingsDirty();
            return;
        }
        ctx->Yield(6);

        const float start = BottomPanelHeight();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_8); // shrink first: a clamp can only
        YieldUntil(ctx, [&] { return BottomPanelHeight() < start - 1.0f; }, 30);   // stop a grow at the maximum
        const float shrunk = BottomPanelHeight();
        IM_CHECK_LT(shrunk, start - 1.0f);

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_9);
        YieldUntil(ctx, [&] { return BottomPanelHeight() > shrunk + 1.0f; }, 30);
        IM_CHECK_GT(BottomPanelHeight(), shrunk + 1.0f);

        if (!wasVisible) {
            SmatchetBottomPanelDrag::Collapse(g_ui);
        }
        g_ui.cfg.Keybindings = original;
        SmatchetUiTestMarkKeybindingsDirty();
        ctx->Yield(2);
    };
}

bool g_panelHostArmed = false;
AppController* g_panelApp = nullptr;
// Set by the test, taken on the UI thread by the GuiFunc: the command hops to the UI thread, so it must not be
// dispatched from the test-engine coroutine thread, which would block waiting on a main thread that waits on it.
bool g_panelDispatchRequested = false;
bool g_panelDispatchDone = false;
smatchet::cmd::CommandResult g_panelGrow;
smatchet::cmd::CommandResult g_panelShrink;

void SubmitPanelHost(ImGuiTestContext*) {
    if (!g_panelHostArmed) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(240.0f, 80.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::GesturePanelHost", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("outside every panel");
        // Dispatched while this window holds the keyboard, from the UI thread (where this GuiFunc runs).
        if (g_panelDispatchRequested && g_panelApp != nullptr &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            g_panelDispatchRequested = false;
            smatchet::cmd::CommandContext cc;
            cc.ScenarioHost = g_panelApp;
            cc.Threading = g_panelApp;
            cc.Source = smatchet::cmd::CommandSource::Internal;
            g_panelGrow = g_panelApp->Commands().Dispatch("view.panel.grow", nlohmann::json::object(), cc);
            g_panelShrink =
                g_panelApp->Commands().Dispatch("view.panel.shrink", nlohmann::json{{"panel", "focused"}}, cc);
            g_panelDispatchDone = true;
        }
    }
    ImGui::End();
}

// The default chord with the keyboard outside every panel (the grid, a dialog): nothing to resize is the
// normal case, so the command succeeds with resized:false rather than failing on every press and key repeat.
void RegisterPanelResizeFocusedNoPanel(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GestureKeys", "PanelResize_FocusedWithNoPanelIsNotAnError");
    t->GuiFunc = SubmitPanelHost;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = ArmApp(ctx);
        if (app == nullptr) {
            return;
        }
        g_panelApp = app;
        g_panelDispatchDone = false;
        g_panelHostArmed = true;
        ctx->Yield(3);
        ctx->WindowFocus("//SmatchetTest::GesturePanelHost");
        ctx->Yield(2);

        const float before = BottomPanelHeight();
        g_panelDispatchRequested = true;
        const bool done = YieldUntil(ctx, [] { return g_panelDispatchDone; }, 60);
        IM_CHECK(done);
        if (done) {
            IM_CHECK(g_panelGrow.Ok);
            IM_CHECK(g_panelShrink.Ok);
            IM_CHECK(g_panelGrow.Data != nullptr && g_panelGrow.Data->is_object() &&
                     g_panelGrow.Data->value("resized", true) == false);
            IM_CHECK(g_panelShrink.Data != nullptr && g_panelShrink.Data->is_object() &&
                     g_panelShrink.Data->value("resized", true) == false);
        }
        ctx->Yield(4);
        IM_CHECK_EQ(BottomPanelHeight(), before); // nothing moved

        g_panelHostArmed = false;
        g_panelDispatchRequested = false;
        g_panelApp = nullptr;
        ctx->Yield(2);
    };
}

// --- E: the calendar ----------------------------------------------------------------------------------------

std::string g_calendarValue;

void SubmitCalendarHost(ImGuiTestContext*) {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 120.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::GestureCalendarHost", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        TrackerDateTimeFieldEditor::RenderGenericDatePicker("##GestureCal", g_calendarValue, /*isDateTime=*/false,
                                                            340.0f);
    }
    ImGui::End();
}

void OpenCalendar(ImGuiTestContext* ctx) {
    ctx->SetRef("//SmatchetTest::GestureCalendarHost");
    ctx->ItemClick("**/📅");
    ctx->Yield(3);
}

void RegisterCalendarKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GestureKeys", "Calendar_KeysPickDay");
    t->GuiFunc = SubmitCalendarHost;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_calendarValue = "2026-11-16";
        ctx->Yield(3);

        // PageDown moves the day a month on; Enter picks the focused day.
        OpenCalendar(ctx);
        ctx->KeyPress(ImGuiKey_PageDown);
        ctx->Yield(3);
        ctx->KeyPress(ImGuiKey_Enter);
        YieldUntil(ctx, [] { return g_calendarValue != "2026-11-16"; }, 30);
        IM_CHECK_EQ(g_calendarValue.compare(0, 10, "2026-12-16"), 0);

        // Shift+PageUp moves a year back.
        OpenCalendar(ctx);
        ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_PageUp);
        ctx->Yield(3);
        ctx->KeyPress(ImGuiKey_Enter);
        YieldUntil(ctx, [] { return g_calendarValue.compare(0, 10, "2026-12-16") != 0; }, 30);
        IM_CHECK_EQ(g_calendarValue.compare(0, 10, "2025-12-16"), 0);

        // T goes to today, whatever the month on show.
        int ty = 0;
        int tm = 0;
        int td = 0;
        IM_CHECK_NO_RET(TrackerDateTimePure::TodayLocalDate(ty, tm, td));
        OpenCalendar(ctx);
        ctx->KeyPress(ImGuiKey_T);
        ctx->Yield(3);
        ctx->KeyPress(ImGuiKey_Enter);
        char want[16];
        std::snprintf(want, sizeof(want), "%04d-%02d-%02d", ty, tm, td);
        YieldUntil(ctx, [&] { return g_calendarValue.compare(0, 10, "2025-12-16") != 0; }, 30);
        IM_CHECK_EQ(g_calendarValue.compare(0, 10, want), 0);
    };
}

// --- F: path remaps -----------------------------------------------------------------------------------------

void SubmitRemapHost(ImGuiTestContext*) {
    ImGui::SetNextWindowSize(ImVec2(640.0f, 220.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::GestureRemapHost", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        SmatchetUiTestDrawAnnotatePathRemaps();
    }
    ImGui::End();
}

void RegisterPathRemapKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GestureKeys", "PathRemaps_AddFocusesAndEnterAddsNext");
    t->GuiFunc = SubmitRemapHost;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SmatchetUiTestAnnotateRemapsBegin();
        ctx->Yield(3);
        ctx->SetRef("//SmatchetTest::GestureRemapHost");

        ctx->ItemClick("Add path remap");
        ctx->Yield(3);
        IM_CHECK_EQ(SmatchetUiTestAnnotateRemapCount(), 1);
        // The caret is already in the new row's "from": typing needs no click.
        IM_CHECK_EQ(GImGui->ActiveId, ctx->ItemInfo("$$0/from").ID);
        ctx->KeyChars("C:/src");
        ctx->KeyPress(ImGuiKey_Tab); // to the "to" field
        ctx->KeyChars("D:/dst");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);

        // Enter in the last row's filled "to" added the next rule and put the caret in its "from".
        IM_CHECK_EQ(SmatchetUiTestAnnotateRemapCount(), 2);
        IM_CHECK_EQ(GImGui->ActiveId, ctx->ItemInfo("$$1/from").ID);

        // An empty last row does not breed more: Enter in its empty "to" adds nothing.
        ctx->KeyPress(ImGuiKey_Tab);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK_EQ(SmatchetUiTestAnnotateRemapCount(), 2);

        SmatchetUiTestAnnotateRemapsEnd();
        ctx->Yield(2);
    };
}

// --- G: read-only text --------------------------------------------------------------------------------------

SelectableText::Context* g_runCtx = nullptr;
const char* const kRunText = "read only text run";

void SubmitRunHost(ImGuiTestContext*) {
    ImGui::SetNextWindowSize(ImVec2(360.0f, 120.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::GestureRunHost", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        SelectableText::Context& run = SelectableText::Begin("##GestureRun");
        g_runCtx = &run;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::TextUnformatted(kRunText);
        const float width = ImGui::CalcTextSize(kRunText).x;
        SelectableText::RegisterSegment(run, kRunText, kRunText + std::strlen(kRunText), pos,
                                        ImGui::GetTextLineHeight(), ImGui::GetFont(), width);
        SelectableText::End(run);
    }
    ImGui::End();
}

void RegisterReadOnlyTextSelectAll(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GestureKeys", "ReadOnlyText_CtrlAFromFocus");
    t->GuiFunc = SubmitRunHost;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->Yield(3);
        ImGuiWindow* host = ImGui::FindWindowByName("SmatchetTest::GestureRunHost");
        IM_CHECK_NO_RET(host != nullptr);
        IM_CHECK_NO_RET(g_runCtx != nullptr);
        if (host == nullptr || g_runCtx == nullptr) {
            return;
        }
        // A click on the run gives it the keyboard; then the pointer leaves the window altogether.
        const ImVec2 inRun(host->InnerRect.Min.x + 24.0f, host->InnerRect.Min.y + 14.0f);
        ctx->MouseMoveToPos(inRun);
        ctx->MouseClick(0);
        ctx->Yield(2);
        ctx->MouseMoveToPos(ImVec2(host->Pos.x + host->Size.x + 80.0f, host->Pos.y + host->Size.y + 80.0f));
        ctx->Yield(2);
        IM_CHECK(!ImGui::IsMouseHoveringRect(host->Pos, host->Pos + host->Size, false));

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
        ctx->Yield(2);
        IM_CHECK(SelectableText::HasSelection(*g_runCtx));
        IM_CHECK_STR_EQ(SelectableText::GetSelectedText(*g_runCtx).c_str(), kRunText);
    };
}

} // namespace

extern "C" void SmatchetRegisterGestureKeysTests(ImGuiTestEngine* engine) {
    RegisterCommandsRegisteredAndBound(engine);
    RegisterToolbarListKeys(engine);
#if defined(SMATCHET_WITH_MCP)
    RegisterMcpSplitterKeys(engine);
#endif
    RegisterPanelResizeKeys(engine);
    RegisterPanelResizeFocusedNoPanel(engine);
    RegisterCalendarKeys(engine);
    RegisterPathRemapKeys(engine);
    RegisterReadOnlyTextSelectAll(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
