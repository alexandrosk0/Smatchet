// command_palette_input_recovery.test.cpp — bucket-E coverage for three command
// palette input behaviours (docs/audits/INPUT_UX_AUDIT.md #14, #15, #16):
//
//   #14  A failed run keeps the palette (and its argument form, when one is up) open,
//        with the typed values intact, the failure message on screen and the caret
//        back in the field, so the input can be corrected and re-run. The palette
//        closes only on success.
//   #15  The list scrolls to the selected row only when the selection moves, so the
//        mouse wheel is not snapped back to that row on the next frame.
//   #16  Editing the query moves the highlight back to the best match (row 0)
//        instead of leaving it at a stale index in the re-ranked list.
//
// Same replica harness as command_palette_deferred_dispatch.test.cpp: a TU-owned
// CommandPaletteUi drawn every frame against the live AppController's registry,
// which runs the production Draw / drainPendingDispatch / drawCommandList code.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/CommandPaletteUi.h"
#include "Commands/CommandRegistry.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiContext::Windows, ImGuiWindow::Scroll
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>
#include <string>
#include <utility>

#include <nlohmann/json.hpp> // Command.h is json_fwd; this TU reads and builds nlohmann::json

// Defined in Source/Core/src/Commands/Scenarios/UiTestScenario.cpp.
AppController* SmatchetActiveUiTestAppController();

namespace {

const char* kFailCmd = "buckete.palette.zqxfail_probe";
const char* kFailMessage = "zqxfail rejected the value";

int g_runCount = 0;
std::string g_lastValue;

// One required string argument; only the value "good" succeeds.
void EnsureFailCommand(AppController& app) {
    using smatchet::cmd::Command;
    smatchet::cmd::CommandRegistry& reg = app.Commands();
    if (reg.HasExact(kFailCmd)) {
        return;
    }
    Command c;
    c.Name = kFailCmd;
    c.Category = "buckete";
    c.Summary = "Synthetic failing-dispatch probe (zqxfail)";
    smatchet::cmd::ParamSpec p;
    p.Name = "value";
    p.Required = true;
    p.Description = "Only the value good succeeds.";
    c.Params.push_back(std::move(p));
    c.Handler = [](const nlohmann::json& args, const smatchet::cmd::CommandContext&) {
        ++g_runCount;
        g_lastValue = args.value("value", std::string());
        if (g_lastValue != "good") {
            return smatchet::cmd::CommandResult::Failure(smatchet::cmd::ErrorCode::ValidationError, kFailMessage);
        }
        return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
    };
    reg.Register(std::move(c));
}

const char* kAlwaysFailCmd = "buckete.palette.zqwnoarg_probe";
int g_alwaysFailRuns = 0;

// No arguments; always fails.
void EnsureAlwaysFailCommand(AppController& app) {
    using smatchet::cmd::Command;
    smatchet::cmd::CommandRegistry& reg = app.Commands();
    if (reg.HasExact(kAlwaysFailCmd)) {
        return;
    }
    Command c;
    c.Name = kAlwaysFailCmd;
    c.Category = "buckete";
    c.Summary = "Synthetic always-failing probe (zqwnoarg)";
    c.Handler = [](const nlohmann::json&, const smatchet::cmd::CommandContext&) {
        ++g_alwaysFailRuns;
        return smatchet::cmd::CommandResult::Failure(smatchet::cmd::ErrorCode::HandlerError, kFailMessage);
    };
    reg.Register(std::move(c));
}

struct RecoveryState {
    smatchet::cmd::CommandPaletteUi palette;
    AppController* app = nullptr;
};

RecoveryState g_recovery;

void DrawHarness(ImGuiTestContext* ctx) {
    auto* s = static_cast<RecoveryState*>(ctx->Test->UserData);
    if (s->app != nullptr) {
        s->palette.Draw(*s->app);
    }
}

// The palette's scrolling list is a child window named "##cmdpalette/##cmdlist_<id>". The parent
// is part of the match: the toolbar editor has a "##cmdlist" child of its own, which stays in the
// window list after that editor has closed.
ImGuiWindow* FindPaletteListWindow() {
    static const char kPrefix[] = "##cmdpalette/##cmdlist";
    const ImGuiContext& g = *GImGui;
    for (int i = 0; i < g.Windows.Size; ++i) {
        ImGuiWindow* w = g.Windows[i];
        if (w != nullptr && w->Name != nullptr && std::strncmp(w->Name, kPrefix, sizeof(kPrefix) - 1) == 0) {
            return w;
        }
    }
    return nullptr;
}

AppController* OpenPalette(ImGuiTestContext* ctx) {
    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
        return nullptr;
    }
    g_recovery.app = app;
    g_recovery.palette.Open();
    ctx->Yield();
    ctx->Yield();
    ctx->SetRef("##cmdpalette");
    return app;
}

void ClosePalette(ImGuiTestContext* ctx) {
    g_recovery.palette.Close();
    g_recovery.app = nullptr;
    ctx->Yield();
}

void RegisterFailedRunKeepsArgs(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "CommandPalette", "FailedRunKeepsPaletteAndArguments");
    t->UserData = &g_recovery;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        EnsureFailCommand(*app);
        g_runCount = 0;
        g_lastValue.clear();
        if (OpenPalette(ctx) == nullptr) {
            return;
        }

        // Select the probe and press Enter: a required argument opens the form.
        ctx->ItemInput("##cmdq");
        ctx->KeyChars("zqxfail");
        ctx->Yield();
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(g_recovery.palette.IsArgFormOpen());
        IM_CHECK_EQ(g_runCount, 0);

        // A rejected value: ItemInputValue types it and presses Enter, which runs it.
        ctx->ItemInputValue("##arg0", "bad");
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(g_runCount, 1);
        IM_CHECK_STR_EQ(g_lastValue.c_str(), "bad");
        // The failure is shown and nothing was thrown away.
        IM_CHECK(g_recovery.palette.IsOpen());
        IM_CHECK(g_recovery.palette.IsArgFormOpen());
        IM_CHECK(g_recovery.palette.DispatchError().find(kFailMessage) != std::string::npos);
        // The caret is back in the argument field. (A key-press check alone would not
        // prove this: the form's Enter handler fires whichever widget is active.)
        IM_CHECK_EQ(GImGui->ActiveId, ctx->GetID("##arg0"));

        // Enter again without retyping re-runs with the SAME value: the buffer survived.
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(g_runCount, 2);
        IM_CHECK_STR_EQ(g_lastValue.c_str(), "bad");
        IM_CHECK(g_recovery.palette.IsOpen());

        // Correcting the value succeeds, clears the message and closes the palette.
        ctx->ItemInputValue("##arg0", "good");
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(g_runCount, 3);
        IM_CHECK_STR_EQ(g_lastValue.c_str(), "good");
        IM_CHECK(!g_recovery.palette.IsOpen());
        IM_CHECK(g_recovery.palette.DispatchError().empty());

        // The successful run already closed the palette.
        g_recovery.app = nullptr;
        ctx->Yield();
    };
}

// A command with no arguments that fails: the list stays up, the failure is shown in
// the footer, and the caret returns to the query so the next keystroke refines it.
void RegisterFailedRunWithoutArgsKeepsPalette(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "CommandPalette", "FailedRunWithoutArgumentsKeepsPalette");
    t->UserData = &g_recovery;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        EnsureAlwaysFailCommand(*app);
        g_alwaysFailRuns = 0;
        if (OpenPalette(ctx) == nullptr) {
            return;
        }

        ctx->ItemInput("##cmdq");
        ctx->KeyChars("zqwnoarg");
        ctx->Yield();
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();

        IM_CHECK_EQ(g_alwaysFailRuns, 1);
        IM_CHECK(g_recovery.palette.IsOpen());
        IM_CHECK(!g_recovery.palette.IsArgFormOpen());
        IM_CHECK(g_recovery.palette.DispatchError().find(kFailMessage) != std::string::npos);
        // Enter deactivated the query input; the failure path re-activated it.
        IM_CHECK_EQ(GImGui->ActiveId, ctx->GetID("##cmdq"));
        IM_CHECK_STR_EQ(g_recovery.palette.FilterText(), "zqwnoarg");

        // The next edit drops the message: it described the previous attempt.
        ctx->KeyChars("x");
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(g_recovery.palette.DispatchError().empty());
        IM_CHECK_EQ(g_alwaysFailRuns, 1);

        ClosePalette(ctx);
    };
}

void RegisterQueryEditResetsSelection(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "CommandPalette", "QueryEditResetsSelectionToBestMatch");
    t->UserData = &g_recovery;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (OpenPalette(ctx) == nullptr) {
            return;
        }
        ctx->ItemInput("##cmdq");
        IM_CHECK_EQ(g_recovery.palette.SelectedIndex(), 0);

        // The unfiltered list holds every registered command, so three steps down fit.
        ctx->KeyPress(ImGuiKey_DownArrow, 3);
        ctx->Yield();
        IM_CHECK_EQ(g_recovery.palette.SelectedIndex(), 3);
        // The arrows moved the highlight, not the focus: the query input is still active.
        IM_CHECK_EQ(GImGui->ActiveId, ctx->GetID("##cmdq"));

        // Typing re-ranks the list; the highlight returns to the best match.
        ctx->KeyChars("a");
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(g_recovery.palette.SelectedIndex(), 0);

        ClosePalette(ctx);
    };
}

void RegisterWheelScrollNotSnappedBack(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "CommandPalette", "WheelScrollIsNotSnappedBackToSelection");
    t->UserData = &g_recovery;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (OpenPalette(ctx) == nullptr) {
            return;
        }
        ImGuiWindow* list = FindPaletteListWindow();
        IM_CHECK(list != nullptr);
        // The unfiltered registry is far longer than the list is tall.
        IM_CHECK(list->ScrollMax.y > 0.0f);
        IM_CHECK_EQ(list->Scroll.y, 0.0f);

        // Wheel down over the list, then let several frames pass. Scrolling to the
        // selected row (still row 0) every frame would put the list back at the top.
        ctx->MouseMoveToPos(list->Rect().GetCenter());
        ctx->MouseWheelY(-3.0f);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(list->Scroll.y > 0.0f);
        IM_CHECK_EQ(g_recovery.palette.SelectedIndex(), 0);

        // Moving the selection by keyboard still brings it into view: one step down
        // from row 0 scrolls the wheel-displaced list back to the top.
        ctx->ItemInput("##cmdq");
        ctx->KeyPress(ImGuiKey_DownArrow);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(g_recovery.palette.SelectedIndex(), 1);
        IM_CHECK_EQ(list->Scroll.y, 0.0f);

        ClosePalette(ctx);
    };
}

} // namespace

extern "C" void SmatchetRegisterCommandPaletteInputRecoveryTests(ImGuiTestEngine* engine) {
    RegisterFailedRunKeepsArgs(engine);
    RegisterFailedRunWithoutArgsKeepsPalette(engine);
    RegisterQueryEditResetsSelection(engine);
    RegisterWheelScrollNotSnappedBack(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
