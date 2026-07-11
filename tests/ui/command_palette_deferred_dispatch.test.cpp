// command_palette_deferred_dispatch.test.cpp — bucket-E regression for Issue
// #1703: CommandPaletteUi must dispatch the selected command OUTSIDE its own
// ImGui::Begin("##cmdpalette")/End() scope (a top-of-Draw deferral), never inline
// mid-frame. Dispatching inline (the pre-fix path) corrupted the ImGui window
// stack when the command touched it — "In window '##cmdpalette': Missing End()"
// abort (Pillar-3 crash, reproduced by the UI monkey at seed 424242).
//
// This drives the REAL CommandPaletteUi + real CommandRegistry (same harness as
// command_palette_inline_typing.test.cpp) and registers a synthetic command whose
// handler records whether "##cmdpalette" is on the ImGui window stack AT DISPATCH
// TIME:
//   - pre-fix (inline dispatch, inside Begin/End) -> palette IS on the stack.
//   - post-fix (deferred to top-of-Draw, before Begin) -> palette is NOT.
// It asserts the command dispatched (flow intact) AND dispatched with the palette
// NOT begun (the deferral). The inline path fails the second assertion.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/CommandPaletteUi.h"
#include "Commands/CommandRegistry.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiContext, CurrentWindowStack, ImGuiWindow
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>

#include <nlohmann/json.hpp> // Command.h is json_fwd; this TU constructs nlohmann::json::object()

// Defined in Source/Core/src/Commands/Scenarios/UiTestScenario.cpp.
AppController* SmatchetActiveUiTestAppController();

namespace {

const char* kDeferCmd = "buckete.palette.zqxdefer_probe";

int g_dispatchCount = 0;
bool g_dispatchedWithPaletteBegun = false;

// True iff a window whose name contains "cmdpalette" is currently on the ImGui
// BeginStack — i.e. we are executing inside the palette's Begin/End scope.
bool PaletteOnBeginStack() {
    const ImGuiContext& g = *GImGui;
    for (int i = 0; i < g.CurrentWindowStack.Size; ++i) {
        const ImGuiWindow* w = g.CurrentWindowStack[i].Window;
        if (w != nullptr && w->Name != nullptr && std::strstr(w->Name, "cmdpalette") != nullptr) {
            return true;
        }
    }
    return false;
}

void EnsureDeferCommand(AppController& app) {
    using smatchet::cmd::Command;
    smatchet::cmd::CommandRegistry& reg = app.Commands();
    if (!reg.HasExact(kDeferCmd)) {
        Command c;
        c.Name = kDeferCmd;
        c.Category = "buckete";
        c.Summary = "Synthetic deferred-dispatch probe (zqxdefer)";
        c.Handler = [](const nlohmann::json&, const smatchet::cmd::CommandContext&) {
            ++g_dispatchCount;
            g_dispatchedWithPaletteBegun = PaletteOnBeginStack();
            return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
        };
        reg.Register(std::move(c));
    }
}

struct DeferState {
    smatchet::cmd::CommandPaletteUi palette;
    AppController* app = nullptr;
};

DeferState g_deferState;

void DrawHarness(ImGuiTestContext* ctx) {
    auto* s = static_cast<DeferState*>(ctx->Test->UserData);
    if (s->app != nullptr) {
        s->palette.Draw(*s->app);
    }
}

void RegisterDeferredDispatchVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "CommandPalette", "DispatchDeferredOutsidePaletteBegin");
    t->UserData = &g_deferState;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        EnsureDeferCommand(*app);

        g_dispatchCount = 0;
        g_dispatchedWithPaletteBegun = false;
        g_deferState.app = app;
        g_deferState.palette.SetFilterText("");
        g_deferState.palette.Open();
        ctx->Yield();
        ctx->Yield();

        // Type the unique query so the probe is the top (selected_ == 0) match.
        ctx->SetRef("##cmdpalette");
        ctx->ItemInput("##cmdq");
        ctx->KeyChars("zqxdefer");
        ctx->Yield();
        ctx->Yield();

        // Enter dispatches the selected command via handleListNavigation. Pre-fix
        // that dispatched inline inside Begin/End; post-fix it latches and Draw()
        // drains it at top-of-frame. Yield enough frames for the deferred drain.
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();

        // The command ran (the dispatch flow still works)...
        IM_CHECK_EQ(g_dispatchCount, 1);
        // ...and it ran with the palette NOT on the ImGui window stack — dispatched
        // OUTSIDE Begin("##cmdpalette")/End(). The pre-fix inline path dispatched
        // WITH the palette begun (the #1703 window-stack corruption); this fails there.
        IM_CHECK(!g_dispatchedWithPaletteBegun);
        // The deferred dispatch closed the palette.
        IM_CHECK(!g_deferState.palette.IsOpen());

        g_deferState.app = nullptr;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterCommandPaletteDeferredDispatchTests(ImGuiTestEngine* engine) {
    RegisterDeferredDispatchVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
