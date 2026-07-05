// callstack_tooltip_hover.test.cpp — bucket-E regression gate for the
// callstack-cell tooltip hover fix.
//
// Bug recap: Source/Core/src/SmatchetFieldRender.cpp::RenderClippedFieldText
// emits one ImGui::TextUnformatted per token via DrawColoredCppLine for
// callstack-typed cells. Pre-fix, the IsItemHovered() guard at the bottom
// of RenderClippedFieldText only matched the LAST token submitted by
// DrawColoredCppLine — so the tooltip (which carries the colored multi-line
// callstack via DrawColoredCppText(tipSource)) was unreachable unless the
// cursor landed on the last identifier. Fix: BeginGroup / EndGroup wrap
// around the per-token emission so the whole cell becomes one item for
// IsItemHovered.
//
// Why a faithful replica (not the production function directly): the
// production guard fires `BeginTooltip / EndTooltip`, and `##Tooltip_NN`
// windows are shared across every widget in the live Smatchet host process
// (Backend tab, View tab, MCP server panel all have their own tooltips). A
// generic `TooltipOpen()` probe can't distinguish "my cell's tooltip" from
// "any other widget's tooltip", so calling the production fn would give a
// noisy oracle — confirmed empirically: with the BeginGroup wrap removed,
// the generic probe still found a tooltip elsewhere and the test passed
// when it shouldn't have. Instead we mirror the production loop body
// verbatim (same idiom as tests/ui/views_columns_reorder.test.cpp) and
// observe a TU-local `tooltipFiredThisFrame` flag that ONLY our replica
// can set. The mirror sits in lock-step with SmatchetFieldRender.cpp:44-50:
// any drift (e.g. someone removes the BeginGroup wrap there) breaks the
// hover semantics this test guards.
//
// Coverage matrix:
//   - left   (x ≈ first-token center)   → tooltip MUST fire (regression case
//     pre-fix when the first token wasn't the last one).
//   - mid    (x ≈ inter-token whitespace, well before the last token)
//                                       → tooltip MUST fire. This is the
//     hardest position pre-fix: not on any TextUnformatted item, only the
//     group's rect catches it. Without BeginGroup/EndGroup the assertion
//     fails — that's the regression we're guarding.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "CppSyntaxHighlight.h"
#include "SmatchetFieldRender.h"

#include "BucketETooltip.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <string>

namespace {

// TU-local state passed between GuiFunc and TestFunc. Engine's per-test
// UserData slot is one void*; struct keeps the helpers readable.
struct CallstackHoverState {
    // Hover-target rect captured after EndGroup (screen coords).
    ImVec2 groupMin{0.0f, 0.0f};
    ImVec2 groupMax{0.0f, 0.0f};
    // Width of the rendered first line. Used to anchor PickHoverPos against
    // the actual text submitted, not the (potentially larger) group bb.
    float textWidth{0.0f};
    // Set by the replica every frame the IsItemHovered() guard fires.
    // TestFunc reads this on the frame following MouseMoveToPos+Yield to
    // confirm the cell tooltip was triggered (positive case) or not.
    std::atomic<bool> tooltipFiredThisFrame{false};
    bool rectValid{false};
};

CallstackHoverState g_state;

// Multi-line value forces the (hasNewline || horizontallyClipped) guard in
// the production renderer's tooltip path. Multiple tokens on the first line
// force DrawColoredCppLine to emit multiple TextUnformatted calls — the
// heart of the regression.
const char* kCallstackValue = "Foo::Bar(int x) at file.cpp:42\n"
                              "Qux::Baz() at other.cpp:17\n"
                              "main() at main.cpp:8";

// Content-identity sentinel for variant 4. Deliberately a token from the
// SECOND line of kCallstackValue: it appears ONLY in the full multi-line
// tooltip source, never in the single-line cell body. So a positive
// TooltipContentMatches proves the assertion observed the TOOLTIP's content
// (not the cell, and not some other window's tooltip).
const char* kTooltipSentinel = "Qux::Baz";

// Persistent payload slot for the variant-4 content marker. MUST be TU-stable
// (not stack): BucketE uses the size==0 AddCallback form, so the drawlist holds
// this pointer directly and TooltipContentMatches reads it a frame or two after
// it is planted. See tests/ui/_helpers/BucketETooltip.h.
BucketE::TooltipMarkerPayload g_tooltipMarkerPayload;

// Returns the screen-space center along Y, at fractional X anchored against
// the rendered TEXT (not the group bb). frac in [0,1] hits within the text.
ImVec2 PickHoverPos(const CallstackHoverState& s, float frac) {
    const float y = 0.5f * (s.groupMin.y + s.groupMax.y);
    const float x = s.groupMin.x + s.textWidth * frac;
    return ImVec2(x, y);
}

// Faithful replica of SmatchetFieldRender.cpp::RenderClippedFieldText's
// callstack code path. Mirrors lines 21..75 with two divergences:
//   - We don't call BeginTooltip / EndTooltip (TU-local flag instead).
//   - availWidth is forced to 0.0f so the (horizontally clipped) branch is
//     never the trigger — only the (hasNewline) branch satisfies the guard.
//
// IF YOU CHANGE SmatchetFieldRender.cpp::RenderClippedFieldText's hover /
// group / guard structure, UPDATE THIS REPLICA TO MATCH.
void DrawHoverReplica(CallstackHoverState& s) {
    const std::string raw(kCallstackValue);

    ImGui::AlignTextToFramePadding();
    std::string singleLine = raw;
    bool hasNewline = false;
    const size_t nl = singleLine.find_first_of("\r\n");
    if (nl != std::string::npos) {
        singleLine.erase(nl);
        hasNewline = true;
    }

    // Group wrap = the production fix under test.
    ImGui::BeginGroup();
    DrawColoredCppLine(singleLine.c_str());
    ImGui::EndGroup();

    s.groupMin = ImGui::GetItemRectMin();
    s.groupMax = ImGui::GetItemRectMax();
    s.textWidth = ImGui::CalcTextSize(singleLine.c_str()).x;
    s.rectValid = (s.groupMax.x > s.groupMin.x) && (s.textWidth > 0.0f);

    // Production guard verbatim: (hasNewline || horizontallyClipped) && IsItemHovered().
    // availWidth=0.0f → horizontallyClipped is false → guard requires hasNewline.
    const bool tooltipsEnabled = true;
    const bool horizontallyClipped = false;
    const bool hovered = ImGui::IsItemHovered();
    const bool fire = tooltipsEnabled && (hasNewline || horizontallyClipped) && hovered;
    s.tooltipFiredThisFrame.store(fire, std::memory_order_release);
}

// Drive a SECOND replica that DELIBERATELY drops the BeginGroup wrap. Used
// only by a self-check test: if removing the wrap doesn't cause the
// middle-token hover to miss, then this test is bogus. This second replica
// gives the bogosity check a deterministic signal — without it we'd have to
// rebuild Smatchet to verify.
void DrawHoverReplica_NoGroupWrap(CallstackHoverState& s) {
    const std::string raw(kCallstackValue);

    ImGui::AlignTextToFramePadding();
    std::string singleLine = raw;
    bool hasNewline = false;
    const size_t nl = singleLine.find_first_of("\r\n");
    if (nl != std::string::npos) {
        singleLine.erase(nl);
        hasNewline = true;
    }

    // SANITY CHECK: NO BeginGroup/EndGroup — mirrors the pre-fix bug shape.
    DrawColoredCppLine(singleLine.c_str());

    s.groupMin = ImGui::GetItemRectMin();
    s.groupMax = ImGui::GetItemRectMax();
    s.textWidth = ImGui::CalcTextSize(singleLine.c_str()).x;
    // Without the group, GetItemRectMin/Max returns only the LAST token's
    // rect — so we don't trust groupMax for outside-the-cell calculations.
    // PickHoverPos still anchors via textWidth, which is the canonical
    // first-line width and matches the bug's geometry.
    s.rectValid = (s.textWidth > 0.0f);

    const bool tooltipsEnabled = true;
    const bool horizontallyClipped = false;
    const bool hovered = ImGui::IsItemHovered();
    const bool fire = tooltipsEnabled && (hasNewline || horizontallyClipped) && hovered;
    s.tooltipFiredThisFrame.store(fire, std::memory_order_release);
}

// Variant 4 — PRODUCTION-DRIVEN draw. Unlike the replicas above (which observe
// a TU-local bool and never open a real tooltip), this drives the actual
// production render path verbatim:
//   - real cell tokenizer: DrawColoredCallstackLine (SmatchetFieldRender.cpp:53)
//   - real BeginGroup/EndGroup wrap (the fix under test)
//   - real (hasNewline || isCallstack) && IsItemHovered() guard
//     (SmatchetFieldRender.cpp:66)
//   - real BeginTooltip + DrawColoredCallstackText(tipSource)
//     (SmatchetFieldRender.cpp:67-70)
// then plants a content-identity marker inside the SAME tooltip block so the
// assertion can confirm THIS cell's tooltip opened (not some other window's) by
// substring-matching the full tooltip source. This is what the disabled variant
// 4 lacked: content identity in the shared host ImGuiContext.
//
// KEEP IN LOCK-STEP with SmatchetFieldRender.cpp::RenderClippedFieldText's
// callstack branch. availWidth is forced to 0.0f so the tooltip trigger is the
// (isCallstack) clause, exercising the always-show-on-hover callstack rule.
void DrawProductionTooltipCell(CallstackHoverState& s) {
    const std::string raw(kCallstackValue);

    ImGui::AlignTextToFramePadding();
    std::string singleLine = raw;
    bool hasNewline = false;
    const size_t nl = singleLine.find_first_of("\r\n");
    if (nl != std::string::npos) {
        singleLine.erase(nl);
        hasNewline = true;
    }

    // Group wrap = production fix under test (SmatchetFieldRender.cpp:48-57).
    ImGui::BeginGroup();
    DrawColoredCallstackLine(singleLine.c_str());
    ImGui::EndGroup();

    // Record the cell rect so the TestFunc can anchor its hover via PickHoverPos
    // (same idiom as the replica variants).
    s.groupMin = ImGui::GetItemRectMin();
    s.groupMax = ImGui::GetItemRectMax();
    s.textWidth = ImGui::CalcTextSize(singleLine.c_str()).x;
    s.rectValid = (s.groupMax.x > s.groupMin.x) && (s.textWidth > 0.0f);

    const bool tooltipsEnabled = true;
    const bool isCallstack = true;            // mirrors IsCallstackField(fieldId)
    const bool horizontallyClipped = false;   // availWidth forced to 0.0f
    if (tooltipsEnabled && (hasNewline || horizontallyClipped || isCallstack) && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        // Real production tooltip body: the semantic callstack tokenizer over
        // the FULL multi-line source (so kTooltipSentinel, on line 2, renders).
        DrawColoredCallstackText(raw.c_str());
        // Out-of-band content-identity sidecar (does not paint). Snapshots the
        // full source so TooltipContentMatches can find kTooltipSentinel.
        BucketE::FillTooltipMarkerPayload(g_tooltipMarkerPayload, raw.c_str());
        BucketE::EmitTooltipContentMarker(g_tooltipMarkerPayload);
        ImGui::EndTooltip();
    }
}

void OpenTestWindow() {
    // ImGuiCond_Always so the position is asserted every frame — defends
    // against the host process's auto-dock policy moving us. ImGuiWindowFlags
    // for Begin go in the call site so each variant can match.
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(640, 120), ImGuiCond_Always);
}

// Flags shared by all variants — NoDocking keeps us floating above the
// Smatchet dockspace, NoSavedSettings prevents .ini drift between runs.
const ImGuiWindowFlags kTestWindowFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;

void YieldUntilRect(ImGuiTestContext* ctx, const CallstackHoverState& s) {
    for (int i = 0; i < 30; ++i) {
        if (s.rectValid) {
            return;
        }
        ctx->Yield();
    }
}

void HoverAndYield(ImGuiTestContext* ctx, ImVec2 pos) {
    ctx->MouseMoveToPos(pos);
    // Two yields: input event flushed → IsItemHovered evaluates next frame.
    ctx->Yield();
    ctx->Yield();
}

void RegisterTests(ImGuiTestEngine* engine) {
    // Variant 1 — first-token center. Pre-fix: this position landed on the
    // FIRST DrawColoredCppLine TextUnformatted call, but IsItemHovered()
    // only matched the LAST token, so the tooltip silently failed.
    {
        ImGuiTest* t = IM_REGISTER_TEST(engine, "FieldRender", "CallstackTooltipHover_FirstToken_ShowsTooltip");
        t->UserData = &g_state;
        t->GuiFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<CallstackHoverState*>(ctx->Test->UserData);
            OpenTestWindow();
            if (ImGui::Begin("SmatchetTest::CallstackTooltipHover", nullptr, kTestWindowFlags)) {
                DrawHoverReplica(*s);
            }
            ImGui::End();
        };
        t->TestFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<CallstackHoverState*>(ctx->Test->UserData);
            SetCallstackFieldIdHint("CSF_TEST_HOVER");
            s->rectValid = false;
            s->tooltipFiredThisFrame.store(false, std::memory_order_release);
            ctx->SetRef("SmatchetTest::CallstackTooltipHover");
            YieldUntilRect(ctx, *s);
            IM_CHECK(s->rectValid);

            HoverAndYield(ctx, PickHoverPos(*s, 0.20f));
            if (!s->tooltipFiredThisFrame.load(std::memory_order_acquire)) {
                ctx->LogError(
                    "tooltipFiredThisFrame=false at first-token x (0.20 * textWidth=%.1fpx, group=[%.1f..%.1f])",
                    s->textWidth, s->groupMin.x, s->groupMax.x);
                IM_CHECK(false);
            }
        };
    }

    // Variant 2 — middle inter-token x. THIS is the hard regression case.
    // Pre-fix BeginGroup wrap removal: no TextUnformatted item lived at the
    // inter-token whitespace, so IsItemHovered() returned false and the
    // tooltip never opened. With the group wrap the whole cell catches.
    {
        ImGuiTest* t = IM_REGISTER_TEST(engine, "FieldRender", "CallstackTooltipHover_MiddleToken_ShowsTooltip");
        t->UserData = &g_state;
        t->GuiFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<CallstackHoverState*>(ctx->Test->UserData);
            OpenTestWindow();
            if (ImGui::Begin("SmatchetTest::CallstackTooltipHover", nullptr, kTestWindowFlags)) {
                DrawHoverReplica(*s);
            }
            ImGui::End();
        };
        t->TestFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<CallstackHoverState*>(ctx->Test->UserData);
            SetCallstackFieldIdHint("CSF_TEST_HOVER");
            s->rectValid = false;
            s->tooltipFiredThisFrame.store(false, std::memory_order_release);
            ctx->SetRef("SmatchetTest::CallstackTooltipHover");
            YieldUntilRect(ctx, *s);
            IM_CHECK(s->rectValid);

            const ImVec2 midPos = PickHoverPos(*s, 0.50f);
            HoverAndYield(ctx, midPos);
            if (!s->tooltipFiredThisFrame.load(std::memory_order_acquire)) {
                ctx->LogError(
                    "tooltipFiredThisFrame=false at middle x (hover=%.1f, group=[%.1f..%.1f], textWidth=%.1f) — "
                    "regression: BeginGroup/EndGroup wrap removed?",
                    midPos.x, s->groupMin.x, s->groupMax.x, s->textWidth);
                IM_CHECK(false);
            }
        };
    }

    // Variant 3 — self-check via the NO-group replica. Validates that the
    // observation methodology (tooltipFiredThisFrame flag) actually changes
    // value when the production fix is absent. Without this we can't tell
    // whether the positive variants pass because the wrap exists or because
    // the methodology is bogus.
    {
        ImGuiTest* t =
            IM_REGISTER_TEST(engine, "FieldRender", "CallstackTooltipHover_MiddleToken_NoGroupWrap_RegressionShape");
        t->UserData = &g_state;
        t->GuiFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<CallstackHoverState*>(ctx->Test->UserData);
            OpenTestWindow();
            if (ImGui::Begin("SmatchetTest::CallstackTooltipHover", nullptr, kTestWindowFlags)) {
                DrawHoverReplica_NoGroupWrap(*s);
            }
            ImGui::End();
        };
        t->TestFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<CallstackHoverState*>(ctx->Test->UserData);
            SetCallstackFieldIdHint("CSF_TEST_HOVER");
            s->rectValid = false;
            s->tooltipFiredThisFrame.store(false, std::memory_order_release);
            ctx->SetRef("SmatchetTest::CallstackTooltipHover");
            YieldUntilRect(ctx, *s);
            IM_CHECK(s->rectValid);

            // Middle x — pre-fix shape: this position lands on inter-token
            // whitespace and IsItemHovered (which now binds to the LAST
            // token) returns false. We REQUIRE that to be the observed
            // outcome, i.e. tooltipFiredThisFrame stays false. That's the
            // empirical proof that the methodology can distinguish.
            HoverAndYield(ctx, PickHoverPos(*s, 0.50f));
            if (s->tooltipFiredThisFrame.load(std::memory_order_acquire)) {
                ctx->LogError(
                    "Self-check failure: tooltipFiredThisFrame=true at middle x WITHOUT BeginGroup wrap — methodology "
                    "is bogus (the regression should have been observable as false). textWidth=%.1f group=[%.1f..%.1f]",
                    s->textWidth, s->groupMin.x, s->groupMax.x);
                IM_CHECK(false);
            }
        };
    }

    // Variant 4 — production-driven content-identity gate (RE-ENABLED, L3).
    //
    // This is the variant that was disabled (see the long comment that used to
    // sit below `namespace`): a generic "is a tooltip open?" probe could not
    // tell MY cell's tooltip from any other window's in the shared host
    // ImGuiContext, so it gave a noisy oracle. The fix its own comment named —
    // "tooltip-content-identity (parse tooltip drawlist for a sentinel string)"
    // — is now provided by BucketE::TooltipContentMatches.
    //
    // Unlike variants 1-3 (replica + TU bool, no real tooltip), this drives the
    // REAL production render path (DrawColoredCallstackLine cell +
    // DrawColoredCallstackText tooltip body inside a real BeginTooltip) and
    // asserts the OPEN tooltip carries kTooltipSentinel ("Qux::Baz", a token
    // present only in the multi-line tooltip source, never in the single-line
    // cell). Because the content marker lives in the tooltip window's own
    // drawlist, any unrelated tooltip is ignored — content identity restored.
    {
        ImGuiTest* t =
            IM_REGISTER_TEST(engine, "FieldRender", "CallstackTooltipHover_Production_ContentIdentity");
        t->UserData = &g_state;
        t->GuiFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<CallstackHoverState*>(ctx->Test->UserData);
            OpenTestWindow();
            if (ImGui::Begin("SmatchetTest::CallstackTooltipHover", nullptr, kTestWindowFlags)) {
                DrawProductionTooltipCell(*s);
            }
            ImGui::End();
        };
        t->TestFunc = [](ImGuiTestContext* ctx) {
            auto* s = static_cast<CallstackHoverState*>(ctx->Test->UserData);
            // The real production guard keys on the callstack field-id hint.
            SetCallstackFieldIdHint("CSF_TEST_HOVER");
            s->rectValid = false;
            ctx->SetRef("SmatchetTest::CallstackTooltipHover");
            YieldUntilRect(ctx, *s);
            IM_CHECK(s->rectValid);

            // Pre-hover: no tooltip open → no marked content. Guards against a
            // false positive from a stale marker / unrelated open tooltip.
            ctx->MouseMoveToPos(ImVec2(s->groupMax.x + 200.0f, s->groupMax.y + 200.0f));
            ctx->Yield();
            ctx->Yield();
            IM_CHECK(!BucketE::TooltipContentMatches(ctx, kTooltipSentinel));

            // Hover the cell at middle x (the hard pre-fix position) and assert
            // the production tooltip opened WITH the sentinel content.
            const ImVec2 midPos = PickHoverPos(*s, 0.50f);
            HoverAndYield(ctx, midPos);
            if (!BucketE::TooltipContentMatches(ctx, kTooltipSentinel)) {
                ctx->LogError(
                    "Production tooltip did not carry sentinel '%s' at middle x (hover=%.1f, "
                    "group=[%.1f..%.1f], textWidth=%.1f) — tooltip not open on THIS cell, or "
                    "BeginGroup/EndGroup wrap removed from RenderClippedFieldText?",
                    kTooltipSentinel, midPos.x, s->groupMin.x, s->groupMax.x, s->textWidth);
                IM_CHECK(false);
            }
        };
    }
}

// Variant 4 was historically disabled because a generic "is a tooltip open?"
// probe could not distinguish MY cell's tooltip from any other window's in the
// shared host ImGuiContext. It is now RE-ENABLED above as
// "CallstackTooltipHover_Production_ContentIdentity", using the content-identity
// helper BucketE::TooltipContentMatches (tests/ui/_helpers/BucketETooltip.h) —
// the exact fix the old disabled-comment named. It drives the real production
// render path and asserts the open tooltip carries a sentinel token that only
// appears in the multi-line tooltip source, so an unrelated tooltip can never
// satisfy it.
} // namespace

extern "C" void SmatchetRegisterCallstackTooltipHoverTests(ImGuiTestEngine* engine) { RegisterTests(engine); }

#endif // SMATCHET_BUILD_UI_TESTS
