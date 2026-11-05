// command_palette_cancel.test.cpp — bucket-E coverage for the Cancel button on the main command
// palette view (CommandPaletteUi::Draw). Same harness as command_palette_deferred_dispatch.test.cpp:
// a TU-owned real CommandPaletteUi driven against the live AppController's registry.
//
// What it locks in:
//   - Clicking Cancel ends in the same state as pressing Escape: palette closed, nothing dispatched,
//     no active item left behind, keyboard focus back on the window that held it before the palette.
//   - Enter on the keyboard-focused button cancels. It must not also run the selected command, which
//     handleListNavigation would latch if Draw carried on past the button.
//   - The button stays inside the palette and the viewport with a list long enough to scroll, in a
//     small window, and at the touch-target style sizes.
//   - The required-args form keeps its own single Cancel button below the parameters.
//
// A small window is simulated by shrinking the main viewport's Size around Draw() only. Draw reads
// the size from the viewport, and the size is restored straight after, so the rest of the frame and
// the real display are untouched.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandPaletteUi.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController
#include "SmatchetLocalization.h"
#include "SmatchetTheme.h"
#include "Ui/TouchSizingPure.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiContext, ImGuiWindow, ImRect
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <nlohmann/json.hpp> // Command.h is json_fwd; this TU constructs nlohmann::json::object()

#include <utility>

namespace {

// Unique tokens so each query selects exactly its own probe, whatever real commands exist. A
// positive control below proves the query reaches the probe, so "nothing dispatched" is not vacuous.
const char* kProbeCmd = "buckete.palette.zqxcancel_probe";
const char* kProbeQuery = "zqxcancel";
const char* kArgProbeCmd = "buckete.palette.zqwargform_probe";
const char* kArgProbeQuery = "zqwargform";

const char* kPaletteWindow = "##cmdpalette";
const char* kHostWindow = "PaletteCancelFocusHost";
const char* kHostWindowRef = "//PaletteCancelFocusHost";

// The label doubles as the item ref, so it comes from the same localization key the button uses.
const char* CancelLabel() { return SmatchetLocalization::T("common.cancel", "Cancel"); }

int g_dispatchCount = 0;

struct CancelState {
    smatchet::cmd::CommandPaletteUi palette;
    AppController* app = nullptr;
    ImVec2 simulatedViewport = ImVec2(0.0f, 0.0f); // zero = the real viewport size
};

CancelState g_cancel;

void RegisterProbe(smatchet::cmd::CommandRegistry& reg, const char* name, const char* summary, bool requiresArg) {
    if (reg.HasExact(name)) {
        return;
    }
    smatchet::cmd::Command c;
    c.Name = name;
    c.Category = "buckete";
    c.Summary = summary;
    c.Handler = [](const nlohmann::json&, const smatchet::cmd::CommandContext&) {
        ++g_dispatchCount;
        return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
    };
    if (requiresArg) {
        smatchet::cmd::ParamSpec param;
        param.Name = "value";
        param.Required = true;
        c.Params.push_back(std::move(param));
    }
    reg.Register(std::move(c));
}

// Draws a second window to hold focus before the palette opens, then the real palette. In a simulated
// small window the palette overlaps the host, but it has focus and so stays on top of it.
void DrawHarness(ImGuiTestContext* /*ctx*/) {
    if (g_cancel.app == nullptr) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(8.0f, 40.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(120.0f, 60.0f), ImGuiCond_Always);
    ImGui::Begin(kHostWindow, nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("focus host");
    ImGui::End();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 realSize = viewport->Size;
    if (g_cancel.simulatedViewport.x > 0.0f) {
        viewport->Size = g_cancel.simulatedViewport;
    }
    g_cancel.palette.Draw(*g_cancel.app);
    viewport->Size = realSize;
}

ImGuiWindow* FocusedRootWindow() {
    const ImGuiWindow* nav = GImGui->NavWindow;
    return nav != nullptr ? nav->RootWindow : nullptr;
}

// Boots the harness. False means the app is not up, which the caller reports as a skip.
bool Arm(ImGuiTestContext* ctx) {
    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
        return false;
    }
    RegisterProbe(app->Commands(), kProbeCmd, "Synthetic palette cancel probe", false);
    RegisterProbe(app->Commands(), kArgProbeCmd, "Synthetic palette arg form probe", true);
    g_dispatchCount = 0;
    g_cancel.app = app;
    g_cancel.simulatedViewport = ImVec2(0.0f, 0.0f);
    ctx->Yield(2); // submit the host window before anything addresses it
    return true;
}

void Disarm(ImGuiTestContext* ctx) {
    g_cancel.palette.Close();
    g_cancel.simulatedViewport = ImVec2(0.0f, 0.0f);
    ctx->Yield();
    g_cancel.app = nullptr;
    ctx->Yield();
}

// Puts focus on the host, opens the palette over it and, for a non-null query, types it for real.
void OpenPalette(ImGuiTestContext* ctx, const char* query) {
    ctx->WindowFocus(kHostWindowRef);
    g_cancel.palette.Open();
    ctx->Yield(2);
    ctx->SetRef(kPaletteWindow);
    if (query != nullptr) {
        ctx->ItemInput("##cmdq");
        ctx->KeyChars(query);
        ctx->Yield(2);
    }
}

// The palette needs up to two frames to stop being submitted and for ImGui to hand focus back.
void LetCloseSettle(ImGuiTestContext* ctx) { ctx->Yield(4); }

void CheckExitRestoresFocus(ImGuiTestContext* ctx, bool viaButton) {
    OpenPalette(ctx, kProbeQuery);
    const ImGuiWindow* palette = ImGui::FindWindowByName(kPaletteWindow);
    const ImGuiWindow* host = ImGui::FindWindowByName(kHostWindow);
    IM_CHECK(palette != nullptr);
    IM_CHECK(host != nullptr);
    IM_CHECK(g_cancel.palette.IsOpen());
    IM_CHECK(FocusedRootWindow() == palette);

    if (viaButton) {
        ctx->ItemClick(CancelLabel());
    } else {
        ctx->KeyPress(ImGuiKey_Escape);
    }
    LetCloseSettle(ctx);

    IM_CHECK(!g_cancel.palette.IsOpen());
    IM_CHECK_EQ(g_dispatchCount, 0);
    IM_CHECK(FocusedRootWindow() == host);
    IM_CHECK_EQ(GImGui->ActiveId, static_cast<ImGuiID>(0));
}

// Asserts the geometry the button has to keep in this state, then that a click on it still closes.
void CheckCancelReachable(ImGuiTestContext* ctx, ImVec2 simulatedViewport) {
    g_cancel.simulatedViewport = simulatedViewport;
    OpenPalette(ctx, nullptr); // empty filter = the whole registry, far more rows than fit
    ctx->Yield(2);

    const ImGuiWindow* palette = ImGui::FindWindowByName(kPaletteWindow);
    IM_CHECK(palette != nullptr);
    const ImGuiTestItemInfo cancel = ctx->ItemInfo(CancelLabel());
    IM_CHECK(cancel.ID != 0);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 viewportSize = simulatedViewport.x > 0.0f ? simulatedViewport : viewport->Size;
    const ImRect viewportRect(viewport->Pos, viewport->Pos + viewportSize);
    IM_CHECK(viewportRect.Contains(cancel.RectFull));
    IM_CHECK(palette->Rect().Contains(cancel.RectFull));
    IM_CHECK(viewportRect.Contains(palette->Rect()));

    // The list really is long enough to scroll, and scrolls inside the palette instead of pushing
    // anything out of it.
    IM_CHECK_GE(palette->DC.ChildWindows.Size, 1);
    const ImGuiWindow* list = palette->DC.ChildWindows[0];
    IM_CHECK_GT(list->ScrollMax.y, 0.0f);
    IM_CHECK_LE(list->Rect().Max.y, palette->Rect().Max.y);

    ctx->ItemClick(CancelLabel());
    LetCloseSettle(ctx);
    IM_CHECK(!g_cancel.palette.IsOpen());
    IM_CHECK_EQ(g_dispatchCount, 0);
}

// The test engine backs up the whole style around every test, so the overlay needs no undo here.
void CheckCancelTouchSized(ImGuiTestContext* ctx) {
    SmatchetTheme::ApplyTouchTargetOverlay(1.0f);
    ctx->Yield();

    OpenPalette(ctx, nullptr);
    const ImGuiTestItemInfo cancel = ctx->ItemInfo(CancelLabel());
    IM_CHECK(cancel.ID != 0);
    const float minHeight = ImGui::GetFontSize() + SmatchetTouchSizingPure::kTouchFramePaddingYPx * 2.0f;
    IM_CHECK_GE(cancel.RectFull.GetHeight() + 0.5f, minHeight);
    IM_CHECK_GE(cancel.RectFull.GetWidth(), 40.0f);
    g_cancel.palette.Close();
    LetCloseSettle(ctx);

    // The larger frames still leave it reachable and clickable in a small window.
    CheckCancelReachable(ctx, ImVec2(320.0f, 200.0f));
}

void CheckEnterOnFocusedCancel(ImGuiTestContext* ctx) {
    // Positive control: this query selects the probe and Enter from the filter runs it.
    OpenPalette(ctx, kProbeQuery);
    ctx->KeyPress(ImGuiKey_Enter);
    LetCloseSettle(ctx);
    IM_CHECK_EQ(g_dispatchCount, 1);
    IM_CHECK(!g_cancel.palette.IsOpen());

    // The same state with focus tabbed from the filter to Cancel closes without a second dispatch.
    OpenPalette(ctx, kProbeQuery);
    ctx->KeyPress(ImGuiKey_Tab);
    ctx->Yield(2);
    IM_CHECK_EQ(GImGui->NavId, ctx->ItemInfo(CancelLabel()).ID);
    ctx->KeyPress(ImGuiKey_Enter);
    LetCloseSettle(ctx);
    IM_CHECK(!g_cancel.palette.IsOpen());
    IM_CHECK_EQ(g_dispatchCount, 1);
}

void CheckArgFormCancel(ImGuiTestContext* ctx) {
    OpenPalette(ctx, kArgProbeQuery);
    ctx->KeyPress(ImGuiKey_Enter); // required param: opens the form, dispatches nothing
    ctx->Yield(2);
    IM_CHECK(g_cancel.palette.IsOpen());
    IM_CHECK(ctx->ItemExists("##arg0"));

    // The only Cancel in this state is the form's own, below the filter row.
    const ImGuiTestItemInfo cancel = ctx->ItemInfo(CancelLabel());
    const ImGuiTestItemInfo filter = ctx->ItemInfo("##cmdq");
    IM_CHECK(cancel.ID != 0);
    IM_CHECK(filter.ID != 0);
    IM_CHECK_GE(cancel.RectFull.Min.y, filter.RectFull.Max.y);

    ctx->ItemClick(CancelLabel());
    LetCloseSettle(ctx);
    IM_CHECK(!g_cancel.palette.IsOpen());
    IM_CHECK_EQ(g_dispatchCount, 0);
}

// Each test body keeps its checks in a helper: a failed IM_CHECK returns only from that helper, so
// Disarm still runs and leaves no palette or simulated viewport behind for the next test.
ImGuiTest* RegisterCancelTest(ImGuiTestEngine* engine, const char* name) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "CommandPalette", name);
    t->GuiFunc = DrawHarness;
    return t;
}

} // namespace

extern "C" void SmatchetRegisterCommandPaletteCancelTests(ImGuiTestEngine* engine) {
    RegisterCancelTest(engine, "Cancel_ClickMatchesEscapeAndRestoresFocus")->TestFunc = [](ImGuiTestContext* ctx) {
        if (Arm(ctx)) {
            CheckExitRestoresFocus(ctx, false);
            CheckExitRestoresFocus(ctx, true);
            Disarm(ctx);
        }
    };
    RegisterCancelTest(engine, "Cancel_EnterOnFocusedButtonDoesNotDispatch")->TestFunc = [](ImGuiTestContext* ctx) {
        if (Arm(ctx)) {
            CheckEnterOnFocusedCancel(ctx);
            Disarm(ctx);
        }
    };
    RegisterCancelTest(engine, "Cancel_ReachableWithLongListAndSmallWindow")->TestFunc = [](ImGuiTestContext* ctx) {
        if (Arm(ctx)) {
            CheckCancelReachable(ctx, ImVec2(0.0f, 0.0f));
            CheckCancelReachable(ctx, ImVec2(320.0f, 200.0f));
            Disarm(ctx);
        }
    };
    RegisterCancelTest(engine, "Cancel_UsableAtTouchSizes")->TestFunc = [](ImGuiTestContext* ctx) {
        if (Arm(ctx)) {
            CheckCancelTouchSized(ctx);
            Disarm(ctx);
        }
    };
    RegisterCancelTest(engine, "Cancel_ArgFormKeepsItsOwnButton")->TestFunc = [](ImGuiTestContext* ctx) {
        if (Arm(ctx)) {
            CheckArgFormCancel(ctx);
            Disarm(ctx);
        }
    };
}

#endif // SMATCHET_BUILD_UI_TESTS
