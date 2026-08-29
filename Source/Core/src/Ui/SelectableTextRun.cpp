// SelectableTextRun — see SelectableTextRun.h for the contract.

#include "SelectableTextRun.h"

#include "SelectableTextRun_detail.h"

#include "SmatchetLocalizedImGui.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SelectableText {

namespace {

std::unordered_map<ImGuiID, Context>& ContextMap() {
    static std::unordered_map<ImGuiID, Context> s_map;
    return s_map;
}

// Lazy per-segment cumulative-width compute. For MVP we recompute every
// hit-test; the result is small (one segment is one styled word in
// MarkdownPreviewRender's word loop). If bucket-C shows it matters, promote to
// a cache keyed by (font, size, textPtr) on the Segment itself.
float CharOffsetToPx(const Segment& s, int charOffset) {
    if (!s.font || s.textOwned.empty() || charOffset <= 0) {
        return 0.0f;
    }
    const int len = static_cast<int>(s.textOwned.size());
    if (charOffset >= len) {
        return s.totalWidth;
    }
    const char* slice = s.textOwned.data();
    return s.font->CalcTextSizeA(s.fontSize, FLT_MAX, 0.0f, slice, slice + charOffset).x;
}

// Binary-search char offset within a segment for a given local pixel x.
int HitTestCharOffset(const Segment& s, float localX) {
    if (!s.font || s.textOwned.empty() || localX <= 0.0f) {
        return 0;
    }
    const int len = static_cast<int>(s.textOwned.size());
    int lo = 0;
    int hi = len;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        const float px = CharOffsetToPx(s, mid);
        if (px < localX) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    // Snap to the nearer side so the cursor doesn't jitter at half-glyph boundaries.
    if (lo > 0) {
        const float pxL = CharOffsetToPx(s, lo - 1);
        const float pxR = CharOffsetToPx(s, lo);
        if (localX - pxL < pxR - localX) {
            return lo - 1;
        }
    }
    return lo;
}

// Normalise selection endpoints so (aSeg, aChar) <= (bSeg, bChar) in doc-order.
void NormaliseSelection(int& aSeg, int& aChar, int& bSeg, int& bChar) {
    if (aSeg > bSeg || (aSeg == bSeg && aChar > bChar)) {
        std::swap(aSeg, bSeg);
        std::swap(aChar, bChar);
    }
}

// Draw the selection overlay rects when the persisted endpoints are still
// in-range; otherwise silently clear the stale selection. Factored out of End()
// to keep that function under the branch cap. Behaviour is byte-identical to the
// inline block it replaced.
void DrawSelectionOverlay(Context& ctx) {
    if (detail::SelectionEndpointsInRange(ctx)) {
        int aSeg = ctx.selStartSeg;
        int aChar = ctx.selStartChar;
        int bSeg = ctx.selEndSeg;
        int bChar = ctx.selEndChar;
        NormaliseSelection(aSeg, aChar, bSeg, bChar);

        const ImU32 selCol = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = aSeg; i <= bSeg; ++i) {
            const Segment& s = ctx.segments[i];
            const int segLen = static_cast<int>(s.textOwned.size());
            const detail::SegmentSpan span = detail::ComputeSegmentSpan(i, aSeg, aChar, bSeg, bChar, segLen);
            const float xFrom = s.pos.x + CharOffsetToPx(s, span.from);
            const float xTo = s.pos.x + CharOffsetToPx(s, span.to);
            if (xTo > xFrom) {
                dl->AddRectFilled(ImVec2(xFrom, s.pos.y), ImVec2(xTo, s.pos.y + s.lineH), selCol);
            }
        }
    } else if (ctx.hasSelection) {
        // Endpoints stale — clear silently.
        ctx.hasSelection = false;
        ctx.selStartSeg = -1;
        ctx.selEndSeg = -1;
    }
}

} // namespace

Context& Begin(const char* strId) {
    const ImGuiID id = ImGui::GetID(strId);
    Context& ctx = ContextMap()[id];
    // Reset per-frame state. Selection state persists.
    ctx.segments.clear();
    ctx.nextDocOrder = 0;
    ctx.currentBlockId = 0;
    ctx.hoveredSeg = -1;
    ctx.hoveredChar = 0;
    return ctx;
}

void EndBlock(Context& ctx) { ++ctx.currentBlockId; }

void RegisterSegment(Context& ctx, const char* begin, const char* end, ImVec2 screenPos, float lineH, ImFont* font,
                     float totalWidth, void* hrefOpaque) {
    if (begin == nullptr || end == nullptr || end <= begin || font == nullptr) {
        return;
    }
    Segment seg;
    seg.docOrder = ctx.nextDocOrder++;
    seg.blockId = ctx.currentBlockId;
    seg.pos = screenPos;
    seg.lineH = lineH;
    seg.textOwned.assign(begin, end);
    seg.font = font;
    seg.fontSize = ImGui::GetFontSize();
    seg.href = hrefOpaque;
    seg.totalWidth = totalWidth;
    ctx.segments.push_back(std::move(seg));
}

// Pointer hit-testing + drag-selection state machine for End(). Updates the hovered segment/char,
// sets the text cursor, and initiates/extends/ends the drag selection. Split out for
// function-size compliance.
static void HandleSelectionPointerInput(Context& ctx, bool hovered, const ImVec2& mouse, bool mouseDown) {
    // Hit-test the mouse against registered segments — needed for the char
    // offset within the segment under the cursor. Tracks hover for the
    // GetHoveredHref accessor too.
    ctx.hoveredSeg = detail::HitTestSegmentIndex(ctx.segments, mouse.x, mouse.y);
    ctx.hoveredChar = 0;
    if (ctx.hoveredSeg >= 0) {
        const Segment& s = ctx.segments[ctx.hoveredSeg];
        ctx.hoveredChar = HitTestCharOffset(s, mouse.x - s.pos.x);
    }
    if (hovered && ctx.hoveredSeg >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

    // Selection initiation: ButtonBehavior with PressedOnClick + LeftMouseButton
    // sets `held` true on the same frame the press lands. `ImGui::IsItemActive()`
    // returns true while the press is held — used for the drag-extend loop.
    const bool justPressed = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ctx.hoveredSeg >= 0;
    if (justPressed) {
        ctx.dragging = true;
        ctx.hasSelection = true;
        ctx.selStartSeg = ctx.hoveredSeg;
        ctx.selStartChar = ctx.hoveredChar;
        ctx.selEndSeg = ctx.hoveredSeg;
        ctx.selEndChar = ctx.hoveredChar;
    } else if (ctx.dragging && mouseDown && ctx.hoveredSeg >= 0) {
        ctx.selEndSeg = ctx.hoveredSeg;
        ctx.selEndChar = ctx.hoveredChar;
    }
    if (!mouseDown) {
        ctx.dragging = false;
    }
}

// Keyboard shortcuts for select-all and copy, plus the right-click context menu, for the End path.
// Shortcuts are gated on hover so they do not collide with global bindings. Split out for
// function-size compliance.
static void HandleSelectionKeyboardAndMenu(Context& ctx, bool hovered, int segCount, const ImGuiIO& io) {
    const bool ctrl = io.KeyCtrl;
    if (hovered && ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false) && segCount > 0) {
        // Select all — span from start of first segment to end of last.
        detail::SelectAllSegments(ctx, segCount);
        ctx.dragging = false;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && HasSelection(ctx)) {
        const std::string sel = GetSelectedText(ctx);
        if (!sel.empty()) {
            ImGui::SetClipboardText(sel.c_str());
        }
    }

    // Right-click context menu. ButtonBehavior above set up the hitId; opening
    // the popup with that same ID makes BeginPopupContextItem(NULL, RightButton)
    // pick it up. Inside the popup we offer Copy + Select All.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##selectable_text_menu");
    }
    if (ImGui::BeginPopup("##selectable_text_menu")) {
        const bool canCopy = HasSelection(ctx);
        // Explicit wrapper calls (no TU-wide alias): this TU renders user text runs, so only
        // these two fixed menu labels route through localization.
        if (SmatchetLocalizedImGui::MenuItem("Copy", "Ctrl+C", false, canCopy)) {
            const std::string sel = GetSelectedText(ctx);
            if (!sel.empty()) {
                ImGui::SetClipboardText(sel.c_str());
            }
        }
        if (SmatchetLocalizedImGui::MenuItem("Select all", "Ctrl+A", false, segCount > 0)) {
            detail::SelectAllSegments(ctx, segCount);
        }
        ImGui::EndPopup();
    }
}

void End(Context& ctx) {
    if (ctx.segments.empty()) {
        return;
    }

    // Compute the bounding box across all registered segments. The hit-test
    // ItemAdd below uses this rectangle to claim the area so the parent
    // window's drag-to-move doesn't fire when the user clicks inside the
    // selectable text. Width covers the widest line; height spans first
    // segment's top to last segment's bottom.
    const detail::BoundingBox bb = detail::ComputeBoundingBox(ctx.segments);
    const ImVec2 bbMin = bb.min;
    const ImVec2 bbMax = bb.max;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) {
        return;
    }

    // Stable per-window ID so ImGui's active-widget machinery routes input to
    // us. The string-id constant keeps the ID stable across frames inside the
    // same window; if the caller embeds multiple SelectableText regions in
    // one ImGui window they share input — acceptable for MVP (cross-region
    // selection isn't supported anyway).
    const ImGuiID hitId = window->GetID("##selectable_text_hitbox");
    const ImRect hitBb(bbMin, bbMax);

    // ItemAdd registers the hit-test rect on the window's draw list. Without
    // this, ImGui treats clicks inside the segments as "empty space" on the
    // parent window, which then drags the popup / modal. AllowOverlap lets
    // sibling widgets such as links inside the text still get hover events. We then call
    // ButtonBehavior below to actually claim input on press.
    ImGui::ItemAdd(hitBb, hitId, NULL, ImGuiItemFlags_AllowOverlap);
    bool hovered = false;
    bool held = false;
    ImGui::ButtonBehavior(hitBb, hitId, &hovered, &held,
                          ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                              ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_AllowOverlap);

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    HandleSelectionPointerInput(ctx, hovered, mouse, mouseDown);
    // Held flag is used implicitly via ButtonBehavior — also signal to ImGui
    // that we own the active state during a drag so window-drag stays off.
    (void)held;

    // Draw selection overlay. Selection invalidated if either endpoint refers
    // to a segment that no longer exists (e.g. content shrank between frames).
    const int segCount = static_cast<int>(ctx.segments.size());
    DrawSelectionOverlay(ctx);

    HandleSelectionKeyboardAndMenu(ctx, hovered, segCount, io);
}

bool HasSelection(const Context& ctx) {
    if (!ctx.hasSelection) {
        return false;
    }
    const int segCount = static_cast<int>(ctx.segments.size());
    if (ctx.selStartSeg < 0 || ctx.selStartSeg >= segCount || ctx.selEndSeg < 0 || ctx.selEndSeg >= segCount) {
        return false;
    }
    if (ctx.selStartSeg == ctx.selEndSeg && ctx.selStartChar == ctx.selEndChar) {
        return false;
    }
    return true;
}

std::string GetSelectedText(const Context& ctx) {
    std::string out;
    if (!HasSelection(ctx)) {
        return out;
    }
    int aSeg = ctx.selStartSeg;
    int aChar = ctx.selStartChar;
    int bSeg = ctx.selEndSeg;
    int bChar = ctx.selEndChar;
    NormaliseSelection(aSeg, aChar, bSeg, bChar);
    out.reserve(64);
    int prevBlockId = -1;
    for (int i = aSeg; i <= bSeg; ++i) {
        const Segment& s = ctx.segments[i];
        const int segLen = static_cast<int>(s.textOwned.size());
        const detail::SegmentSpan span = detail::ComputeSegmentSpan(i, aSeg, aChar, bSeg, bChar, segLen);
        // Block boundary → emit a newline before the next block's bytes (skip on
        // the very first segment so we don't lead with one).
        if (prevBlockId >= 0 && s.blockId != prevBlockId) {
            out.push_back('\n');
        }
        if (span.to > span.from) {
            out.append(s.textOwned.data() + span.from, static_cast<std::size_t>(span.to - span.from));
        }
        prevBlockId = s.blockId;
    }
    return out;
}

void* GetHoveredHref(const Context& ctx) {
    if (ctx.hoveredSeg < 0 || ctx.hoveredSeg >= static_cast<int>(ctx.segments.size())) {
        return nullptr;
    }
    return ctx.segments[ctx.hoveredSeg].href;
}

} // namespace SelectableText
