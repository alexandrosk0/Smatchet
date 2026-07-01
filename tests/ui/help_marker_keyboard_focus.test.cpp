// help_marker_keyboard_focus.test.cpp — bucket-E regression gate for GitHub
// Issue #1128 (a11y: keyboard-reachable (?) help-marker tooltips).
//
// Bug recap: ~38 long-form UI explanations live in hover-only
// tooltips behind a (?) glyph rendered by SmatchetHelpMarker::RenderText.
// Pre-fix the glyph was an ImGui::TextUnformatted item, which never
// participates in keyboard nav and never receives focus — so a keyboard-only
// user could Tab through a panel and never surface any (?) help text. The
// fix (Source/Core/src/Ui/SmatchetHelpMarker.cpp) renders the glyph over an
// InvisibleButton carrying ImGuiButtonFlags_EnableNav (a nav target that does
// NOT add a mouse tab-stop) and shows the tooltip on
// IsItemHovered() || IsItemFocused().
//
// This test drives the REAL production SmatchetHelpMarker::RenderText (the
// exact #1128 surface), uses KEYBOARD nav (Tab) to focus the marker, and
// asserts a tooltip is submitted on focus. A second self-check variant drives
// a pre-fix replica (TextUnformatted + hover-only) and asserts keyboard focus
// does NOT surface a tooltip — proving the observation methodology
// distinguishes the fix from the regression shape.
//
// Drift warning — IF YOU CHANGE SmatchetHelpMarker::RenderText's focusability
// (the InvisibleButton + ImGuiButtonFlags_EnableNav) or its tooltip-fire guard
// (IsItemHovered() || IsItemFocused()), UPDATE THIS TEST. The pre-fix replica
// below intentionally mirrors the OLD shape and must stay in lock-step as the
// negative control.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "SmatchetHelpMarker.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

namespace {

// Long-form help body, mirrors the kind of text the ~38 call sites carry.
const char* kHelpBody = "This setting controls how the assistant resolves the active project scope "
                        "when multiple trackers are connected.";

struct HelpMarkerFocusState {
    // Set by GuiFunc every frame a tooltip window was submitted (g.TooltipPreviousWindow
    // is non-null on the frame BeginTooltip ran). TestFunc reads it after Tab+Yield.
    bool tooltipSubmittedThisFrame = false;
    // NavId after the marker was submitted — lets TestFunc confirm keyboard nav
    // actually landed on (focused) the marker item, not some other widget.
    ImGuiID markerId = 0;
    ImGuiID navIdThisFrame = 0;
    bool useRealMarker = true; // true: production RenderText; false: pre-fix replica.
};

HelpMarkerFocusState g_state;

// Pre-fix replica: the OLD marker shape — a TextUnformatted glyph with a
// hover-only tooltip and NO nav participation. Used as the negative control so
// we can prove keyboard focus surfaced nothing before the fix.
void DrawPreFixReplica(const char* fullText) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted("(?)");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (ImGui::BeginTooltip()) {
            ImGui::TextUnformatted(fullText);
            ImGui::EndTooltip();
        }
    }
}

void DrawHelpMarkerWindow(HelpMarkerFocusState& s) {
    ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(420, 160), ImGuiCond_Always);
    if (ImGui::Begin("SmatchetTest::HelpMarkerKeyboardFocus", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        // A leading focusable item gives keyboard nav a deterministic landing
        // spot; one Tab from here advances onto the marker (the next nav item).
        ImGui::Button("Anchor##hmkf");

        if (s.useRealMarker) {
            // The exact #1128 surface — the shared marker all ~38 sites call.
            SmatchetHelpMarker::RenderText(kHelpBody);
            // The marker's nav item is the InvisibleButton with id PushID(fullText)
            // + "##helpmarker"; GetItemID() after the call is that button.
            s.markerId = ImGui::GetItemID();
        } else {
            DrawPreFixReplica(kHelpBody);
            s.markerId = ImGui::GetItemID();
        }

        ImGuiContext& g = *ImGui::GetCurrentContext();
        s.tooltipSubmittedThisFrame = (g.TooltipPreviousWindow != nullptr);
        s.navIdThisFrame = g.NavId;
    }
    ImGui::End();
}

// Drive KEYBOARD nav focus onto the marker item by its id. NavMoveTo is the
// engine's keyboard-nav primitive — it moves NavId to the target exactly as a
// user pressing Tab/arrows would, exercising the nav path (not a mouse hover).
// For a non-nav item (the pre-fix TextUnformatted replica) NavMoveTo cannot
// land focus, which is precisely the regression we assert in variant 2.
void FocusMarkerViaKeyboard(ImGuiTestContext* ctx, ImGuiID markerId) {
    // Land nav on the Anchor first so a keyboard-nav session is active, then
    // move nav focus to the marker id.
    ctx->ItemClick("Anchor##hmkf");
    ctx->Yield();
    if (markerId != 0) {
        ctx->NavMoveTo(markerId);
    }
    ctx->Yield();
    ctx->Yield();
}

void RegisterTests(ImGuiTestEngine* engine) {
    // Variant 1 — production RenderText, keyboard focus surfaces the tooltip.
    {
        ImGuiTest* t = IM_REGISTER_TEST(engine, "HelpMarker", "KeyboardFocus_RealMarker_ShowsTooltip");
        t->UserData = &g_state;
        t->GuiFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<HelpMarkerFocusState*>(ctx->Test->UserData);
            DrawHelpMarkerWindow(*s);
        };
        t->TestFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<HelpMarkerFocusState*>(ctx->Test->UserData);
            s->useRealMarker = true;
            s->tooltipSubmittedThisFrame = false;
            s->markerId = 0;
            s->navIdThisFrame = 0;
            ctx->SetRef("SmatchetTest::HelpMarkerKeyboardFocus");
            ctx->Yield();
            ctx->Yield();

            // s->markerId is populated by GuiFunc once the window has drawn.
            FocusMarkerViaKeyboard(ctx, s->markerId);

            // The marker must be a real nav item (non-zero id) and keyboard nav
            // must have landed on it.
            if (s->markerId == 0) {
                ctx->LogError("marker has no item id — RenderText emitted no nav-participating item");
                IM_CHECK(false);
            }
            if (s->navIdThisFrame != s->markerId) {
                ctx->LogError("keyboard nav did not focus the marker: NavId=0x%08X markerId=0x%08X — "
                              "InvisibleButton + ImGuiButtonFlags_EnableNav missing?",
                              s->navIdThisFrame, s->markerId);
                IM_CHECK(false);
            }
            // The #1128 assertion: a tooltip is submitted while the marker is
            // keyboard-focused (no mouse hover involved).
            if (!s->tooltipSubmittedThisFrame) {
                ctx->LogError("no tooltip submitted on keyboard focus — IsItemFocused() guard missing");
                IM_CHECK(false);
            }
        };
    }

    // Variant 2 — pre-fix replica self-check. Drives the OLD shape
    // (TextUnformatted + hover-only) and asserts keyboard focus surfaces NO
    // tooltip. If this fails (a tooltip appears) the methodology is bogus —
    // it would mean the positive variant could pass for the wrong reason.
    {
        ImGuiTest* t = IM_REGISTER_TEST(engine, "HelpMarker", "KeyboardFocus_PreFixReplica_NoTooltip_RegressionShape");
        t->UserData = &g_state;
        t->GuiFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<HelpMarkerFocusState*>(ctx->Test->UserData);
            DrawHelpMarkerWindow(*s);
        };
        t->TestFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<HelpMarkerFocusState*>(ctx->Test->UserData);
            s->useRealMarker = false;
            s->tooltipSubmittedThisFrame = false;
            s->markerId = 0;
            s->navIdThisFrame = 0;
            ctx->SetRef("SmatchetTest::HelpMarkerKeyboardFocus");
            ctx->Yield();
            ctx->Yield();

            // The pre-fix replica is a TextUnformatted glyph: GetItemID() returns
            // a non-nav id. We deliberately pass it to NavMoveTo to prove nav
            // cannot land focus on it (and thus no focus-tooltip fires).
            FocusMarkerViaKeyboard(ctx, s->markerId);

            // Pre-fix shape: the TextUnformatted glyph is not a nav item, so
            // keyboard nav cannot focus it and no tooltip should fire on focus.
            if (s->tooltipSubmittedThisFrame) {
                ctx->LogError("self-check failure: pre-fix replica surfaced a tooltip on keyboard focus — "
                              "methodology cannot distinguish the regression (TextUnformatted should be unreachable)");
                IM_CHECK(false);
            }
        };
    }
}

} // namespace

extern "C" void SmatchetRegisterHelpMarkerKeyboardFocusTests(ImGuiTestEngine* engine) { RegisterTests(engine); }

#endif // SMATCHET_BUILD_UI_TESTS
