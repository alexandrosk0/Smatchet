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
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    // Hit-test the mouse against registered segments.
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

    // Mouse selection. Only initiate when the click lands on a segment so we
    // don't clear an existing selection on every background click.
    if (mouseClicked && ctx.hoveredSeg >= 0) {
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

    // Ctrl+C → copy selection.
    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && HasSelection(ctx)) {
        const std::string sel = GetSelectedText(ctx);
        if (!sel.empty()) {
            ImGui::SetClipboardText(sel.c_str());
        }
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
