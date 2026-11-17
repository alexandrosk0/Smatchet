// gesture_keys.test.cpp — bucket-E (ImGui Test Engine) coverage for the keyboard equivalents of pointer
// gestures (docs/plans/input-ux-w2-gesture-keys.md, audit § 4.6). Each case presses the real key through the
// live SmatchetUI::dispatchKeybindings -> command registry -> consumer chain; none calls a command or a
// handler directly.
//
//   A. CommandsRegisteredAndBound      the seven commands are registered and have a default binding.
//   B. ToolbarList_KeysMoveAndRemove   Alt+Down / Alt+Up (Alt held first, as a person presses it) reorder and
//                                      Delete removes in the Customize Toolbar list (#122), hosted from the real
//                                      SmatchetToolbarUi.
//   C. McpSplitter_KeysResize          Shift+Alt+Down / Shift+Alt+Up move the MCP panel's divider.
//   D. PanelResize_KeysNudgeAVisiblePanel  a rebound view.panel.grow / view.panel.shrink chord, naming whichever
//                                      edge panel is visible, resizes it (#138); seen through the panel state. Skips
//                                      (logged) under the headless software-GL lane when no dock node is realised
//                                      there, and fails on a real-GL run: see the backlog entry
//                                      2026-11-17-bucket-e-headless-lane-realises-no-edge-panel.
//   E. Calendar_KeysPickDay            PageDown then Enter picks the day a month on; T then Enter picks today
//                                      (#44), through the real RenderGenericDatePicker.
//   F. PathRemaps_AddFocusesAndEnter   Add path remap puts the caret in the new row; Enter in the last filled
//                                      "to" adds the next row and focuses it (#130).
//   G. ReadOnlyText_CtrlAFromFocus     Ctrl+A selects a keyboard-focused run with the mouse elsewhere (#12).
//
// Each case puts back what it touched (bindings, collapsed panel, config value, the toolbar modal), red or green,
// through a scope guard, so a sibling case starts from the same state; the panel case's grow and shrink net to zero,
// and a red case between them can leave the panel one step larger. Mutation check (recorded in the plan's
// Verification): reverting the consumer's take, the command's post, the default binding or the focus branch turns
// the matching case red, except case D, which does not run in the headless lane.

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
#include "ui_test_skip.h" // SmatchetUiTestIsHeadlessSoftwareGl
#include "Ui/SmatchetToolbarUi.h"
#include "Ui/SelectableTextRun.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <functional>
#include <cstring>
#include <string>
#include <utility>
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

// Runs `fn` when the case ends, however it ends. IM_CHECK* return from the test body on failure, so cleanup placed
// after a check would be skipped by a red case, and what it leaves behind (a modal in ImGui's popup stack, a rebound
// chord, a grown panel) would turn the cases after it red for a reason of their own.
struct ScopeExit {
    std::function<void()> Fn;
    explicit ScopeExit(std::function<void()> fn) : Fn(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() {
        if (Fn) {
            Fn();
        }
    }
};

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

// Presses `key` with Alt already held, the way a person does. The test engine would otherwise deliver Alt and the
// arrow in one frame, and ImGui's Alt-release menu-layer toggle (NavUpdateWindowing) then moves focus off the list
// on the next frame; an Alt that was down first cancels that toggle.
void PressWithAltHeld(ImGuiTestContext* ctx, ImGuiKey key) {
    ctx->KeyDown(ImGuiMod_Alt);
    ctx->Yield();
    ctx->KeyPress(key);
    ctx->Yield(2);
    ctx->KeyUp(ImGuiMod_Alt);
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
        // A red case must not leave the modal behind: an undrawn modal in the popup stack keeps blocking, and mutes
        // the app-wide shortcuts, for every case that follows.
        ScopeExit cleanup([] {
            g_toolbarHost.armed = false;
            ImGui::ClosePopupsOverWindow(nullptr, false);
        });
        h.toolbar.OpenEditor();
        ctx->Yield(3);
        IM_CHECK_STR_EQ(Tips(h).c_str(), "ABC");

        const std::string modalRef = std::string("//") + ToolbarTitleRef();
        ctx->SetRef(modalRef.c_str());
        ctx->ItemClick("**/$$0/###row"); // select the first row; the list now has the keyboard
        ctx->Yield(2);
        IM_CHECK_EQ(h.toolbar.EditorSelectedForTest(), 0);

        PressWithAltHeld(ctx, ImGuiKey_DownArrow);
        YieldUntil(ctx, [&] { return Tips(h) == "BAC"; }, 20);
        IM_CHECK_STR_EQ(Tips(h).c_str(), "BAC");
        IM_CHECK_EQ(h.toolbar.EditorSelectedForTest(), 1);

        PressWithAltHeld(ctx, ImGuiKey_UpArrow);
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
        PressWithAltHeld(ctx, ImGuiKey_UpArrow);
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
        ScopeExit cleanup([original] {
            g_mcpHostArmed = false;
            g_ui.cfg.McpServerInfoPanelHeightPx = original;
        });
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

// --- D: the edge panels ---------------------------------------------------------------------------------------

// The four per-panel drag states the nudge writes. A nudge sets pendingExtent (the extent asked for) the frame it
// runs, so a key press that reached NudgePanelExtent shows up there without depending on dock geometry.
struct PanelStateSnapshot {
    float pending[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

EdgePanelDragState* const* PanelStates() {
    static EdgePanelDragState* const kStates[4] = {&g_ui.bottomPanelDrag, &g_ui.viewsColumnDrag,
                                                   &g_ui.primarySideBarDrag, &g_ui.secondarySideBarDrag};
    return kStates;
}

PanelStateSnapshot SnapshotPanelStates() {
    PanelStateSnapshot snap;
    for (int i = 0; i < 4; ++i) {
        snap.pending[i] = PanelStates()[i]->pendingExtent;
    }
    return snap;
}

// The index of the first state whose pendingExtent differs from `before`, or -1.
int ChangedPanelState(const PanelStateSnapshot& before) {
    for (int i = 0; i < 4; ++i) {
        if (PanelStates()[i]->pendingExtent != before.pending[i]) {
            return i;
        }
    }
    return -1;
}

// The edge panel to resize: whichever one is on screen. Which are open depends on the layout the earlier cases left
// behind, and the nudge does the same thing for each, so the case does not hang on one of them.
const char* FirstVisiblePanelName() {
    if (SmatchetBottomPanelDrag::IsPanelVisible()) {
        return "bottom";
    }
    if (SmatchetBottomPanelDrag::IsSideBarVisible(SmatchetBottomPanelDrag::SideBar::Primary)) {
        return "primary";
    }
    if (SmatchetBottomPanelDrag::IsSideBarVisible(SmatchetBottomPanelDrag::SideBar::Secondary)) {
        return "secondary";
    }
    return nullptr;
}

void RegisterPanelResizeKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GestureKeys", "PanelResize_KeysNudgeAVisiblePanel");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = ArmApp(ctx);
        if (app == nullptr) {
            return;
        }
        const KeybindingsConfig originalBindings = g_ui.cfg.Keybindings;
        const bool bottomWasVisible = SmatchetBottomPanelDrag::IsPanelVisible();
        bool expandedBottom = false;
        ScopeExit cleanup([&] {
            if (expandedBottom && !bottomWasVisible) {
                SmatchetBottomPanelDrag::Collapse(g_ui);
            }
            g_ui.cfg.Keybindings = originalBindings;
            SmatchetUiTestMarkKeybindingsDirty();
        });
        const char* panel = FirstVisiblePanelName();
        if (panel == nullptr) {
            SmatchetBottomPanelDrag::Expand(g_ui, 0.0f);
            expandedBottom = true;
            YieldUntil(ctx, [&] { return FirstVisiblePanelName() != nullptr; }, 120);
            panel = FirstVisiblePanelName();
        }
        if (panel == nullptr) {
            const ImGuiDockNode* node = ImGui::DockBuilderGetNode(SmatchetDockNodeIds::kBottomPanel);
            const bool realized = node != nullptr && node->ParentNode != nullptr && node->HostWindow != nullptr;
            ctx->LogInfo(
                "no edge panel is visible: bottom node=%d parent=%d isVisible=%d windows=%d realized=%d showLog=%d",
                node != nullptr ? 1 : 0, (node != nullptr && node->ParentNode != nullptr) ? 1 : 0,
                (node != nullptr && node->IsVisible) ? 1 : 0, node != nullptr ? node->Windows.Size : -1,
                realized ? 1 : 0, g_ui.showLogWindow ? 1 : 0);
            // The headless software-GL lane does not realise the canonical dock nodes even when healthy
            // (reset_layout_docking.test.cpp, LiveHostProbe), so there is no panel to resize there. On a real-GL run
            // a missing panel is a failure. The nudge's bounds are covered by the NudgeTargetExtent doctests.
            if (SmatchetUiTestIsHeadlessSoftwareGl() && !realized) {
                ctx->LogInfo("SKIP: no dock node realised under headless software GL (see ui_test_skip.h)");
                return;
            }
            IM_CHECK(false);
            return;
        }
        ctx->LogInfo("resizing the %s panel", panel);

        // The chords name the panel, so the case does not depend on what the test engine focused last, and rebinding
        // them is part of what it proves.
        const std::string args = std::string("{\"panel\":\"") + panel + "\"}";
        Rebind(ctx, "view.panel.grow", args.c_str(), "Ctrl+Alt+Shift+9");
        Rebind(ctx, "view.panel.shrink", args.c_str(), "Ctrl+Alt+Shift+8");
        ctx->Yield(4);

        const PanelStateSnapshot beforeGrow = SnapshotPanelStates();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_9);
        int which = -1;
        YieldUntil(ctx, [&] { return (which = ChangedPanelState(beforeGrow)) >= 0; }, 30);
        IM_CHECK_GE(which, 0);
        if (which >= 0) {
            const float grown = PanelStates()[which]->pendingExtent;
            ctx->Yield(8); // let the extent land, so the shrink steps from the new size
            const PanelStateSnapshot beforeShrink = SnapshotPanelStates();
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_8);
            YieldUntil(ctx, [&] { return PanelStates()[which]->pendingExtent != beforeShrink.pending[which]; }, 30);
            IM_CHECK_LT(PanelStates()[which]->pendingExtent, grown - 1.0f);
        }

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
        ScopeExit cleanup([] {
            g_panelHostArmed = false;
            g_panelDispatchRequested = false;
            g_panelApp = nullptr;
        });
        g_panelHostArmed = true;
        ctx->Yield(3);
        ctx->WindowFocus("//SmatchetTest::GesturePanelHost");
        ctx->Yield(2);

        const PanelStateSnapshot before = SnapshotPanelStates();
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
        IM_CHECK_EQ(ChangedPanelState(before), -1); // no panel was asked to resize

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
        ScopeExit cleanup([] { SmatchetUiTestAnnotateRemapsEnd(); }); // puts the user's rules back, red or green
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
