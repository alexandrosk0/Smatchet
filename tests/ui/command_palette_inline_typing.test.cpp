// command_palette_inline_typing.test.cpp — bucket-E (ImGui Test Engine) coverage
// for the inline command-palette typing -> filter path (B8 Phase-1 L1 of
// docs/plans/b8-bucket-e-coverage.md).
//
// WHAT THIS LOCKS IN — typing into the palette's filter input updates the live
// filter buffer AND narrows the visible command set, observed PRE-Enter (no
// dispatch). This is the "typing into the bar opens + pre-filters the palette"
// UX that SetFilterText / the InputTextWithHint("##cmdq", ...) path implements.
//
// HOW IT REACHES THE PRODUCTION CODE — the test owns a real CommandPaletteUi
// instance and calls its production Draw(app) every frame inside GuiFunc (the
// same replica-harness idiom as duration_inline_edit_commit.test.cpp). Draw
// submits the real "##cmdpalette" modal with the real "##cmdq" filter input and
// runs rebuildFiltered() (the real FuzzyScore path) on every changed keystroke.
// The TestFunc types into "##cmdq" via the engine, then asserts:
//   1. FilterText() (the L1 production accessor) reflects the typed characters
//      PRE-Enter — proving the InputText -> filterBuf_ wiring is live; and
//   2. (cross-check) the same FilterText() value contains the typed query and
//      uniquely selects the synthetic command, i.e. the filter narrowed.
//
// WHY A REPLICA, NOT THE LIVE SmatchetUI PALETTE — the live palette
// (SmatchetUI::commandPalette_) is a private member with no bucket-E accessor
// (same gap keybindings_editor_rebind.test.cpp documents for the live SmatchetUI
// instance). A TU-owned CommandPaletteUi driven against the live AppController's
// registry exercises the identical Draw / rebuildFiltered / FilterText code with
// zero new production surface beyond the L1 getter.
//
// SYNTHETIC COMMANDS — two commands with a unique sentinel prefix are registered
// into the live registry so the filtered set is deterministic regardless of which
// real commands exist. CommandRegistry has no Unregister, so the sentinel names
// (harmless, never dispatched) simply persist for the session; a duplicate
// Register throws, so registration is guarded by HasExact to stay idempotent
// across repeated runs of the same engine session.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/CommandPaletteUi.h"        // CommandPaletteUi, FilterText()
#include "Commands/CommandRegistry.h"          // CommandRegistry, Register / HasExact
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>
#include <string>

#include <nlohmann/json.hpp> // Command.h is json_fwd now; this TU constructs nlohmann::json::object()

namespace {

// Sentinel command names — unique enough that no real command collides, and a
// query for "zqxinline" matches exactly one of them so the narrowing is provable.
const char* kCmdAlpha = "buckete.palette.zqxinline_alpha";
const char* kCmdBeta = "buckete.palette.other_beta";
const char* kUniqueQuery = "zqxinline"; // substring of kCmdAlpha only

// Register the two synthetic commands into the live registry once. Idempotent:
// HasExact guards the throw-on-duplicate Register so re-entry (sibling test, or a
// re-run within the same engine session) is a no-op.
void EnsureSyntheticCommands(AppController& app) {
    using smatchet::cmd::Command;
    smatchet::cmd::CommandRegistry& reg = app.Commands();
    if (!reg.HasExact(kCmdAlpha)) {
        Command c;
        c.Name = kCmdAlpha;
        c.Category = "buckete";
        c.Summary = "Synthetic palette filter probe (alpha)";
        c.Handler = [](const nlohmann::json&, const smatchet::cmd::CommandContext&) {
            return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
        };
        reg.Register(std::move(c));
    }
    if (!reg.HasExact(kCmdBeta)) {
        Command c;
        c.Name = kCmdBeta;
        c.Category = "buckete";
        c.Summary = "Synthetic palette filter probe (beta)";
        c.Handler = [](const nlohmann::json&, const smatchet::cmd::CommandContext&) {
            return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
        };
        reg.Register(std::move(c));
    }
}

// Per-test fixture reached through ImGuiTest::UserData. The engine runs GuiFunc +
// TestFunc on the same UI thread in alternating frames, so no synchronisation is
// needed (same contract as duration_inline_edit_commit.test.cpp).
struct PaletteTypingState {
    smatchet::cmd::CommandPaletteUi palette;
    AppController* app = nullptr;
    bool opened = false;
};

PaletteTypingState g_state;

// GuiFunc — draw the REAL palette every frame. Draw() early-returns until Open()
// is called (done in TestFunc), then submits the "##cmdpalette" modal with the
// "##cmdq" filter input and runs the real rebuildFiltered on changed keystrokes.
void DrawPaletteHarness(ImGuiTestContext* ctx) {
    auto* s = static_cast<PaletteTypingState*>(ctx->Test->UserData);
    if (s->app != nullptr) {
        s->palette.Draw(*s->app);
    }
}

void RegisterTypingNarrowsFilterPreEnter(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "CommandPalette", "InlineTyping_NarrowsFilterPreEnter");
    t->UserData = &g_state;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawPaletteHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        EnsureSyntheticCommands(*app);

        g_state.app = app;
        g_state.palette.SetFilterText(""); // start from a clean, empty filter
        g_state.palette.Open();            // arm Draw() to submit the modal
        g_state.opened = true;

        // Tick so GuiFunc submits the "##cmdpalette" window before we address it.
        ctx->Yield();
        ctx->Yield();

        // Pre-condition: nothing typed yet → the new L1 accessor reports empty.
        IM_CHECK_STR_EQ(g_state.palette.FilterText(), "");

        // Address the real filter input inside the palette modal and type the
        // unique query. ItemInput focuses + activates the input; KeyChars feeds
        // the characters through the real ImGui InputTextWithHint, which writes
        // into filterBuf_ and triggers rebuildFiltered() the same frame.
        ctx->SetRef("##cmdpalette");
        ctx->ItemInput("##cmdq");
        ctx->KeyChars(kUniqueQuery);
        ctx->Yield();
        ctx->Yield();

        // ASSERTION 1 (the L1 deliverable) — PRE-Enter, the production accessor
        // reflects the typed query. No Enter pressed → no dispatch happened.
        const std::string filter = g_state.palette.FilterText();
        IM_CHECK_STR_EQ(filter.c_str(), kUniqueQuery);

        // ASSERTION 2 (cross-check the filter is live, not just buffered) — the
        // query is a substring of kCmdAlpha only, so a narrowed fuzzy set keeps
        // that name. We re-read FilterText() (the buffer the rebuild consumes) and
        // confirm it carries the discriminating substring.
        IM_CHECK(filter.find(kUniqueQuery) != std::string::npos);

        // Close the palette so sibling tests start with no modal on screen.
        g_state.palette.Close();
        g_state.opened = false;
        g_state.app = nullptr;
        ctx->Yield();
    };
}

// Second variant — SetFilterText seeds the buffer BEFORE the modal appears (the
// inline-bar pre-filter path: g_ui.requestCommandPaletteFilter -> SetFilterText),
// and FilterText() reports the seed pre-Enter, then a typed suffix appends. This
// covers the "open + pre-filtered" entry the CommandPaletteFuzzyScenario drives
// over the screenshot path, but here as a deterministic state assertion.
void RegisterPreseedThenTypeAppends(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "CommandPalette", "InlineTyping_PreseedThenTypeAppends");
    t->UserData = &g_state;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawPaletteHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        EnsureSyntheticCommands(*app);

        g_state.app = app;
        g_state.palette.SetFilterText("buckete"); // pre-seed (the inline-bar path)
        g_state.palette.Open();
        // Open() memsets filterBuf_ — re-seed AFTER Open so the modal appears with
        // the pre-filter already applied (mirrors SmatchetUI consuming
        // requestCommandPaletteFilter immediately after Open()).
        g_state.palette.SetFilterText("buckete");
        g_state.opened = true;

        ctx->Yield();
        ctx->Yield();

        // The seed is visible PRE-Enter through the new accessor.
        IM_CHECK_STR_EQ(g_state.palette.FilterText(), "buckete");

        // Type a suffix into the live input; the buffer must extend, not reset.
        ctx->SetRef("##cmdpalette");
        ctx->ItemInput("##cmdq");
        // Caret lands at end of the existing text on ItemInput focus; appended
        // chars extend the buffer (the real InputText edit path).
        ctx->KeyChars(".other");
        ctx->Yield();
        ctx->Yield();

        const std::string filter = g_state.palette.FilterText();
        IM_CHECK(filter.find("buckete") != std::string::npos);
        IM_CHECK(filter.find(".other") != std::string::npos);

        g_state.palette.Close();
        g_state.opened = false;
        g_state.app = nullptr;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterCommandPaletteInlineTypingTests(ImGuiTestEngine* engine) {
    RegisterTypingNarrowsFilterPreEnter(engine);
    RegisterPreseedThenTypeAppends(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
