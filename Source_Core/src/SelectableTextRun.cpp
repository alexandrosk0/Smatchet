// SelectableTextRun — see SelectableTextRun.h for the contract.

#include "SelectableTextRun.h"

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

void TextRun(Context& ctx, const char* begin, const char* end, ImFont* font, ImU32 color, float /*wrapWidth*/,
             void* hrefOpaque) {
    if (begin == end || begin == nullptr || font == nullptr) {
        return;
    }

    ImGui::PushFont(font);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(begin, end);
    ImGui::PopStyleColor();

    const float width = ImGui::CalcTextSize(begin, end).x;
    ImGui::PopFont();

    Segment seg;
    seg.docOrder = ctx.nextDocOrder++;
    seg.blockId = ctx.currentBlockId;
    seg.pos = pos;
    seg.lineH = lineH;
    seg.textOwned.assign(begin, end);
    seg.font = font;
    seg.fontSize = ImGui::GetFontSize();
    seg.href = hrefOpaque;
    seg.totalWidth = width;
    ctx.segments.push_back(std::move(seg));
}

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

void End(Context& ctx) {
    if (ctx.segments.empty()) {
        return;
    }

    // Compute the bounding box across all registered segments. The hit-test
    // ItemAdd below uses this rectangle to claim the area so the parent
    // window's drag-to-move doesn't fire when the user clicks inside the
    // selectable text. Width covers the widest line; height spans first
    // segment's top to last segment's bottom.
    ImVec2 bbMin = ctx.segments[0].pos;
    ImVec2 bbMax(bbMin.x + ctx.segments[0].totalWidth, bbMin.y + ctx.segments[0].lineH);
    for (std::size_t i = 1; i < ctx.segments.size(); ++i) {
        const Segment& s = ctx.segments[i];
        if (s.pos.x < bbMin.x) {
            bbMin.x = s.pos.x;
        }
        if (s.pos.y < bbMin.y) {
            bbMin.y = s.pos.y;
        }
        const float rx = s.pos.x + s.totalWidth;
        const float ry = s.pos.y + s.lineH;
        if (rx > bbMax.x) {
            bbMax.x = rx;
        }
        if (ry > bbMax.y) {
            bbMax.y = ry;
        }
    }

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
    // sibling widgets (links inside the text) still get hover events; we set
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

    // Hit-test the mouse against registered segments — needed for the char
    // offset within the segment under the cursor. Tracks hover for the
    // GetHoveredHref accessor too.
    ctx.hoveredSeg = -1;
    ctx.hoveredChar = 0;
    for (int i = 0; i < static_cast<int>(ctx.segments.size()); ++i) {
        const Segment& s = ctx.segments[i];
        const bool yIn = (mouse.y >= s.pos.y && mouse.y <= s.pos.y + s.lineH);
        const bool xIn = (mouse.x >= s.pos.x && mouse.x <= s.pos.x + s.totalWidth);
        if (yIn && xIn) {
            ctx.hoveredSeg = i;
            ctx.hoveredChar = HitTestCharOffset(s, mouse.x - s.pos.x);
            break;
        }
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
    // Held flag is used implicitly via ButtonBehavior — also signal to ImGui
    // that we own the active state during a drag so window-drag stays off.
    (void)held;

    // Draw selection overlay. Selection invalidated if either endpoint refers
    // to a segment that no longer exists (e.g. content shrank between frames).
    const int segCount = static_cast<int>(ctx.segments.size());
    if (ctx.hasSelection && ctx.selStartSeg >= 0 && ctx.selStartSeg < segCount && ctx.selEndSeg >= 0 &&
        ctx.selEndSeg < segCount) {
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
            int from = (i == aSeg) ? aChar : 0;
            int to = (i == bSeg) ? bChar : segLen;
            if (from > to) {
                std::swap(from, to);
            }
            if (from < 0) {
                from = 0;
            }
            if (to > segLen) {
                to = segLen;
            }
            const float xFrom = s.pos.x + CharOffsetToPx(s, from);
            const float xTo = s.pos.x + CharOffsetToPx(s, to);
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

    // Keyboard shortcuts — gated on the hit area being hovered so they don't
    // collide with global Ctrl+A / Ctrl+C bindings outside the SelectableText
    // region.
    const bool ctrl = io.KeyCtrl;
    if (hovered && ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false) && segCount > 0) {
        // Select all — span from start of first segment to end of last.
        ctx.hasSelection = true;
        ctx.selStartSeg = 0;
        ctx.selStartChar = 0;
        ctx.selEndSeg = segCount - 1;
        ctx.selEndChar = static_cast<int>(ctx.segments[segCount - 1].textOwned.size());
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
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, canCopy)) {
            const std::string sel = GetSelectedText(ctx);
            if (!sel.empty()) {
                ImGui::SetClipboardText(sel.c_str());
            }
        }
        if (ImGui::MenuItem("Select all", "Ctrl+A", false, segCount > 0)) {
            ctx.hasSelection = true;
            ctx.selStartSeg = 0;
            ctx.selStartChar = 0;
            ctx.selEndSeg = segCount - 1;
            ctx.selEndChar = static_cast<int>(ctx.segments[segCount - 1].textOwned.size());
        }
        ImGui::EndPopup();
    }
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
        int from = (i == aSeg) ? aChar : 0;
        int to = (i == bSeg) ? bChar : segLen;
        if (from > to) {
            std::swap(from, to);
        }
        if (from < 0) {
            from = 0;
        }
        if (to > segLen) {
            to = segLen;
        }
        // Block boundary → emit a newline before the next block's bytes (skip on
        // the very first segment so we don't lead with one).
        if (prevBlockId >= 0 && s.blockId != prevBlockId) {
            out.push_back('\n');
        }
        if (to > from) {
            out.append(s.textOwned.data() + from, static_cast<std::size_t>(to - from));
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
