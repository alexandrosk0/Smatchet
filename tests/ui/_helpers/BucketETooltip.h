// BucketETooltip.h — shared bucket-E helper for content-identity tooltip
// assertions (plan b8-bucket-e-coverage L3 / tooling backlog #101).
//
// PROBLEM THIS SOLVES
// -------------------
// Smatchet's live host process runs a single ImGuiContext shared by every
// panel. Tooltips are emitted into pooled windows named "##Tooltip_NN"
// (ImGui 1.92: BeginTooltipEx uses the "##Tooltip_%02d" template). A generic
// "is any tooltip open?" probe therefore CANNOT tell "my cell's tooltip"
// apart from "some other widget's tooltip" — which is exactly why variant 4 of
// callstack_tooltip_hover.test.cpp was disabled (see that file's header). The
// fix its own comment named: "tooltip-content-identity (parse tooltip drawlist
// for a sentinel string)."
//
// WHY THE PLAN'S LITERAL WORDING IS NOT FEASIBLE — AND WHAT WE DO INSTEAD
// ----------------------------------------------------------------------
// The plan says "walk the tooltip window's DrawList CmdBuffer for a text
// command containing `sentinel`". Verified against ThirdParty ImGui 1.92.8
// (.fetchcontent-src/imgui-src/imgui.h): an `ImDrawCmd` stores ONLY rasterised
// geometry — ClipRect, TexRef, Vtx/Idx offsets, ElemCount. ImGui::Text* paths
// tessellate each glyph into vertex quads; the SOURCE STRING IS NOT RETAINED in
// the draw command. There is also no text-capture API in this Test Engine
// version (imgui_te_context.h has CaptureScreenshot* but no ItemReadAsText /
// CaptureText). So a literal "find the text command containing the sentinel"
// walk is impossible: the bytes simply aren't there to find.
//
// Robust mechanism actually used: a DrawList `UserCallback` sentinel marker
// backed by CALLER-OWNED persistent storage.
// `ImDrawList::AddCallback(cb, userdata, /*size=*/0)` (ImGui 1.92) inserts an
// `ImDrawCmd` whose `UserCallback != nullptr` and stores `userdata` DIRECTLY in
// `UserCallbackData` (the size==0 "no indirection" branch — verified in
// imgui_draw.cpp::AddCallback). We deliberately avoid the size>0 copy branch
// because there `UserCallbackData` is left NULL at submit time and only
// resolved during Render() into a per-frame scratch buffer (_CallbacksDataBuf)
// that is fragile to introspect post-hoc. With size==0 the payload pointer is
// readable from the CmdBuffer the instant it is planted, for as long as the
// pointed-to payload object stays alive — so the producer keeps its payload in
// a TU-stable slot (a static/global, see EmitTooltipContentMarker overloads),
// NOT on the stack.
//
// The producer plants ONE such marker inside the SAME BeginTooltip/EndTooltip
// block that renders the real content; this helper walks every visible
// "##Tooltip_NN" window's CmdBuffer for that marker and substring-matches the
// referenced sentinel. Because the marker lives in the tooltip window's own
// drawlist, a tooltip from any OTHER window (which never planted a marker) is
// correctly ignored — that is the content-identity property variant 4 needs.
//
// ImGui rebuilds each window's CmdBuffer every frame, so the marker is present
// only on frames where the marked tooltip is actually open and the producer
// re-planted it. That is the desired semantics: TooltipContentMatches answers
// "is the marked tooltip open RIGHT NOW with this content" — read it on the
// frame after the hover settles (the same cadence variants 1-3 use for their
// TU flag). It is deterministic (no glyph rasterisation guesswork, no pixel
// diff). The rendered tooltip body is still the PRODUCTION path
// (DrawColoredCallstackText); the marker is an out-of-band sidecar carrying
// only the assertion sentinel — it does not alter what the user sees.
//
// Header-only by design: the callback is a static free function with internal
// linkage, the marker payload is a fixed-size POD, and all queries run inside
// the test TU. No CMake source-list change, no extra link unit.

#pragma once

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "imgui.h"
#include "imgui_internal.h" // ImGuiContext, ImGuiWindow, FindWindowByName-adjacent access

#include "imgui_te_context.h"

#include <cstring>

namespace BucketE {

// Magic header stamped into every marker payload so the CmdBuffer walk can
// distinguish OUR sentinel callbacks from any unrelated UserCallback a widget
// might legitimately register. Kept short + fixed so the payload is a POD that
// AddCallback can byte-copy.
struct TooltipMarkerPayload {
    char magic[8];    // "SMTCHET" + NUL — identity tag for the walk.
    char text[240];   // NUL-terminated sentinel-bearing content snapshot.
};

namespace detail {

// No-op at render time: the marker exists purely so its payload bytes live in
// the tooltip window's CmdBuffer for post-frame introspection. We never want it
// to actually paint anything, so the callback body does nothing.
inline void TooltipMarkerCallback(const ImDrawList*, const ImDrawCmd*) {}

// True iff `cmd` is one of our content markers (UserCallback identity + magic
// match on the directly-stored, size==0 payload pointer).
inline bool IsMarkerCmd(const ImDrawCmd& cmd, const TooltipMarkerPayload** outPayload) {
    if (cmd.UserCallback != &TooltipMarkerCallback) {
        return false;
    }
    if (cmd.UserCallbackData == nullptr) {
        return false;
    }
    const TooltipMarkerPayload* p = static_cast<const TooltipMarkerPayload*>(cmd.UserCallbackData);
    if (std::strncmp(p->magic, "SMTCHET", sizeof(p->magic)) != 0) {
        return false;
    }
    if (outPayload != nullptr) {
        *outPayload = p;
    }
    return true;
}

} // namespace detail

// Plant a content-identity marker into the CURRENT window's draw list, referring
// to a CALLER-OWNED, persistent payload. Call this EXACTLY ONCE inside the open
// BeginTooltip/EndTooltip block, right after rendering the real tooltip body.
//
// `payload` MUST outlive the walk that reads it (TooltipContentMatches runs a
// frame or two later) — keep it in a TU-static / global slot, never on the
// stack. Fill it via FillTooltipMarkerPayload below. We use the size==0 form of
// AddCallback so the pointer is stored directly and readable immediately; that
// is why the storage lifetime is the caller's responsibility.
inline void EmitTooltipContentMarker(const TooltipMarkerPayload& payload) {
    ImGui::GetWindowDrawList()->AddCallback(
        &detail::TooltipMarkerCallback, const_cast<TooltipMarkerPayload*>(&payload), /*userdata_size=*/0);
}

// Populate a caller-owned payload slot with the magic tag + a snapshot of
// `content` (the string whose sentinel substring the test will assert on,
// typically the full tooltip source text). Safe to call every frame.
inline void FillTooltipMarkerPayload(TooltipMarkerPayload& payload, const char* content) {
    std::memset(&payload, 0, sizeof(payload));
    std::strncpy(payload.magic, "SMTCHET", sizeof(payload.magic) - 1);
    if (content != nullptr) {
        std::strncpy(payload.text, content, sizeof(payload.text) - 1);
    }
}

// Returns true iff a currently-open tooltip window carries a content marker (see
// EmitTooltipContentMarker) whose copied text contains `sentinel` as a
// substring. Walks every active ImGui window whose name matches the pooled
// tooltip template "##Tooltip_NN" / "##Tooltip_DragDrop_NN" and inspects its
// draw-list CmdBuffer. A tooltip from any other window that did NOT plant a
// marker is ignored — this is the content-identity guarantee that lets a
// production-driven hover assertion run in the shared host ImGuiContext.
//
// `ctx` is accepted to match the planned signature and to anchor the call to a
// Test Engine context (the walk reads the live ImGuiContext the engine drives);
// it is otherwise not dereferenced.
inline bool TooltipContentMatches(ImGuiTestContext* ctx, const char* sentinel) {
    (void)ctx;
    if (sentinel == nullptr || sentinel[0] == '\0') {
        return false;
    }
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int wi = 0; wi < g.Windows.Size; ++wi) {
        ImGuiWindow* window = g.Windows[wi];
        if (window == nullptr || !window->Active || window->Hidden) {
            continue;
        }
        // Pooled tooltip windows are named "##Tooltip_NN" (and the drag-drop
        // variant "##Tooltip_DragDrop_NN"). Match the stable prefix so we never
        // peek into ordinary panels.
        if (std::strncmp(window->Name, "##Tooltip_", 10) != 0) {
            continue;
        }
        ImDrawList* dl = window->DrawList;
        if (dl == nullptr) {
            continue;
        }
        for (int ci = 0; ci < dl->CmdBuffer.Size; ++ci) {
            const TooltipMarkerPayload* payload = nullptr;
            if (!detail::IsMarkerCmd(dl->CmdBuffer[ci], &payload)) {
                continue;
            }
            if (std::strstr(payload->text, sentinel) != nullptr) {
                return true;
            }
        }
    }
    return false;
}

} // namespace BucketE

#endif // SMATCHET_BUILD_UI_TESTS
