#include "MarkdownPreviewRender.h"

#include "CppSyntaxHighlight.h"
#include "MarkdownConvert.h"
#include "SelectableTextRun.h"
#include "SmatchetImGuiFonts.h"
#include "Logger.h"

extern "C" {
#include "md4c.h"
}

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Inline mark bits — combined per-run via OR. Bold + Italic together select the
// BoldItalic font; Code overrides body font with Mono. Link/Strike are visual
// overlays drawn after the text.
constexpr uint8_t MARK_BOLD = 1 << 0;
constexpr uint8_t MARK_ITALIC = 1 << 1;
constexpr uint8_t MARK_CODE = 1 << 2;
constexpr uint8_t MARK_STRIKE = 1 << 3;
constexpr uint8_t MARK_LINK = 1 << 4;

struct StyledRun {
    std::string text;
    uint8_t marks = 0;
    std::string href;
};

struct TableCellData {
    std::vector<StyledRun> runs;
};

struct TableRowData {
    std::vector<TableCellData> cells;
    bool isHeader = false;
};

struct PreviewState {
    /// Inline runs accumulated for the current block. Flushed by PreviewFlushBlock.
    std::vector<StyledRun> runs;
    /// When non-null, run-text appends are redirected here instead of `runs` —
    /// used to capture inline content per table cell.
    std::vector<StyledRun>* activeRuns = nullptr;
    /// Active mark bits, set/cleared as md4c reports span enter/leave.
    uint8_t currentMarks = 0;
    /// Active link href while currentMarks has MARK_LINK.
    std::string currentHref;
    /// Verbatim accumulator for fenced / indented code blocks (separate from `runs`
    /// so newlines and whitespace inside code render byte-for-byte).
    std::string codeBuffer;
    /// Language tag from `MD_BLOCK_CODE_DETAIL::lang` ("cpp", "c++", "python", ...).
    /// Empty for indented code blocks or fences with no info string.
    std::string codeLang;
    /// Stack of list kinds: '-' for bullet, '1' for ordered.
    std::vector<char> listStack;
    /// Per-ordered-list counter so we render "1. " "2. " ...
    std::vector<int> orderedCounters;
    /// Indent level (each list nesting bumps this by one).
    int listDepth = 0;
    /// True while inside a fenced code block — append text directly with newlines preserved.
    int codeDepth = 0;
    /// True while the block is a heading; flushes with bigger text.
    int headingLevel = 0;
    /// Table accumulation. tableDepth>0 means we're inside a table block; nested
    /// tables aren't standard so we render only the outermost. Cell contents flow
    /// into `tableCellRuns` while activeRuns points at it.
    int tableDepth = 0;
    bool tableInHeader = false;
    int tableColCount = 0;
    int tableNextId = 0;
    std::vector<TableRowData> tableRows;
    std::vector<StyledRun> tableCellRuns;
    int imgSpanDepth = 0;
    std::string imgAltAccum;
#ifndef NDEBUG
    bool debugLoggedMdTextHtml = false;
#endif
    /// Render mode — gates the three tooltip-only behaviour changes (heading scaling,
    /// inline code-block child, link click handler).
    MarkdownPreviewRender::Mode mode = MarkdownPreviewRender::Mode::Full;
    /// When true, links use cyan + underline + cursor + URL tooltip and fire
    /// ShellExecuteA on click. Off in Tooltip mode — the tooltip dismisses before
    /// mouse-up so the click never lands.
    bool clickableLinks = true;
    /// <=0 falls back to ImGui::GetContentRegionAvail().x. Lets callers pin a wrap
    /// width independent of the surrounding window (tooltip option).
    float fixedWrapWidth = 0.0f;
    /// Active SelectableText context. Non-null → prose `ImGui::TextUnformatted`
    /// calls inside `PreviewRenderRuns` route through `SelectableText::TextRun`
    /// so glyphs are drag-selectable + Ctrl+C-copyable. Code blocks + tables
    /// remain non-selectable in MVP (handled by a follow-up).
    SelectableText::Context* selCtx = nullptr;
};

/// Append `text` of length `size` as either an extension of the trailing run
/// (when style matches) or a fresh styled run. Coalescing keeps `runs` short.
/// Routes to `activeRuns` when set (table-cell capture) else `runs`.
static void PreviewAppendRunBytes(PreviewState& s, const char* text, size_t size) {
    if (size == 0)
        return;
    std::vector<StyledRun>& target = s.activeRuns ? *s.activeRuns : s.runs;
    if (!target.empty() && target.back().marks == s.currentMarks && target.back().href == s.currentHref) {
        target.back().text.append(text, size);
        return;
    }
    StyledRun r;
    r.text.assign(text, size);
    r.marks = s.currentMarks;
    r.href = s.currentHref;
    target.push_back(std::move(r));
}

static void PreviewAppendRunString(PreviewState& s, const std::string& text) {
    PreviewAppendRunBytes(s, text.data(), text.size());
}

/// Map mark bits to an ImFont. CODE wins over bold/italic combos because the
/// monospace font already has its own visual weight and inline `code` spans
/// rarely care about being bold/italic.
static ImFont* PreviewPickFont(uint8_t marks) {
    const SmatchetPreviewFonts& f = SmatchetGetPreviewFonts();
    if (marks & MARK_CODE)
        return f.Mono ? f.Mono : f.Regular;
    const bool bold = (marks & MARK_BOLD) != 0;
    const bool italic = (marks & MARK_ITALIC) != 0;
    if (bold && italic)
        return f.BoldItalic ? f.BoldItalic : (f.Bold ? f.Bold : f.Regular);
    if (bold)
        return f.Bold ? f.Bold : f.Regular;
    if (italic)
        return f.Italic ? f.Italic : f.Regular;
    return f.Regular;
}

/// Walks `runs` word by word and emits ImGui text segments with manual word
/// wrap against `wrapWidth`. Whitespace between words is collapsed to a single
/// space (matches HTML/Markdown rendering rules); '\n' inside a run forces a
/// hard line break. Links are rendered with an underline overlay and clickable
/// hit-rect; strike-through is a half-height line over the word.
static void PreviewRenderRuns(const std::vector<StyledRun>& runs, const PreviewState& s) {
    if (runs.empty())
        return;

    struct InlineWord {
        std::string text; // empty when forceBreak=true
        uint8_t marks = 0;
        std::string href;
        bool forceBreak = false;
    };
    std::vector<InlineWord> words;
    words.reserve(runs.size() * 4);
    for (const StyledRun& run : runs) {
        size_t i = 0;
        const std::string& t = run.text;
        while (i < t.size()) {
            unsigned char c = static_cast<unsigned char>(t[i]);
            if (c == '\n') {
                InlineWord w;
                w.forceBreak = true;
                words.push_back(std::move(w));
                ++i;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++i; // collapse runs of whitespace into a single inter-word gap
            } else {
                size_t j = i;
                while (j < t.size()) {
                    unsigned char d = static_cast<unsigned char>(t[j]);
                    if (d == ' ' || d == '\t' || d == '\n' || d == '\r')
                        break;
                    ++j;
                }
                InlineWord w;
                w.text.assign(t.data() + i, j - i);
                w.marks = run.marks;
                w.href = run.href;
                words.push_back(std::move(w));
                i = j;
            }
        }
    }

    const float wrapWidth = (s.fixedWrapWidth > 0.0f) ? s.fixedWrapWidth : ImGui::GetContentRegionAvail().x;
    // Space width with the regular font — close enough for inter-word spacing
    // regardless of which variant the surrounding word uses.
    ImGui::PushFont(PreviewPickFont(0));
    const float spaceW = ImGui::CalcTextSize(" ").x;
    ImGui::PopFont();

    bool firstOnLine = true;
    float curX = 0.0f;
    bool prevWasCode = false;
    // Inline-code background tint — sits behind the text, drawn first so glyphs
    // overlay it. Same shade for adjacent code words and the inter-word space
    // between them, so consecutive `inline code` words read as one continuous bar.
    const ImU32 codeBgCol = IM_COL32(48, 48, 60, 200);
    const float codeBgRound = 3.0f;
    const float codeBgPadX = 2.0f;

    for (size_t wi = 0; wi < words.size(); ++wi) {
        const InlineWord& w = words[wi];
        if (w.forceBreak) {
            if (firstOnLine) {
                // Empty line — emit a zero-width text so cursor advances vertically.
                ImGui::TextUnformatted("");
            }
            firstOnLine = true;
            curX = 0.0f;
            prevWasCode = false;
            continue;
        }

        ImFont* font = PreviewPickFont(w.marks);
        ImGui::PushFont(font);
        const float wordW = ImGui::CalcTextSize(w.text.c_str(), w.text.c_str() + w.text.size()).x;
        ImGui::PopFont();

        // Wrap if adding this word (with leading space if needed) would overflow.
        const float leadingW = firstOnLine ? 0.0f : spaceW;
        if (!firstOnLine && curX + leadingW + wordW > wrapWidth) {
            firstOnLine = true;
            curX = 0.0f;
            prevWasCode = false;
        }

        const bool isCode = (w.marks & MARK_CODE) != 0;

        if (!firstOnLine) {
            ImGui::SameLine(0.0f, 0.0f);
            // When both the previous and current word are inline-code, the gap
            // between them is part of the same `code` span — paint the bg under
            // the space too so the highlighting reads as continuous.
            if (prevWasCode && isCode) {
                const ImVec2 spacePos = ImGui::GetCursorScreenPos();
                const float lineH = ImGui::GetTextLineHeight();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(ImVec2(spacePos.x, spacePos.y), ImVec2(spacePos.x + spaceW, spacePos.y + lineH),
                                  codeBgCol, 0.0f);
            }
            ImGui::TextUnformatted(" ");
            ImGui::SameLine(0.0f, 0.0f);
            curX += spaceW;
        }

        ImGui::PushFont(font);
        const bool isLink = (w.marks & MARK_LINK) != 0;
        if (isLink) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.78f, 1.0f, 1.0f));
        }
        const ImVec2 wordPos = ImGui::GetCursorScreenPos();
        const float lineH = ImGui::GetTextLineHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Inline-code background — drawn before the text so glyphs render on top.
        // Padding extends slightly left/right of the word; rounding only on outer
        // corners (left rounded if no preceding code word, right if no following).
        if (isCode) {
            const bool nextIsCode =
                (wi + 1 < words.size()) && !words[wi + 1].forceBreak && (words[wi + 1].marks & MARK_CODE) != 0;
            const float rl = prevWasCode ? 0.0f : codeBgRound;
            const float rr = nextIsCode ? 0.0f : codeBgRound;
            const ImVec2 r0(wordPos.x - (prevWasCode ? 0.0f : codeBgPadX), wordPos.y);
            const ImVec2 r1(wordPos.x + wordW + (nextIsCode ? 0.0f : codeBgPadX), wordPos.y + lineH);
            const ImDrawFlags flags = (rl > 0.0f ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersNone) |
                                      (rr > 0.0f ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
            dl->AddRectFilled(r0, r1, codeBgCol, std::max(rl, rr), flags);
        }
        ImGui::TextUnformatted(w.text.c_str(), w.text.c_str() + w.text.size());
        if (s.selCtx) {
            // Record the just-rendered word so SelectableText::End() can hit-test
            // the mouse against it + paint the selection overlay + service
            // Ctrl+C. href is threaded through as opaque void* — the caller's
            // existing link-click handler (ShellExecute below) stays
            // authoritative; the registered href is only used by
            // GetHoveredHref for future routing.
            SelectableText::RegisterSegment(
                *s.selCtx, w.text.data(), w.text.data() + w.text.size(), wordPos, lineH, font, wordW,
                isLink && !w.href.empty() ? const_cast<void*>(static_cast<const void*>(w.href.c_str())) : nullptr);
        }
        if (isLink) {
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            dl->AddLine(ImVec2(wordPos.x, wordPos.y + lineH - 1.0f),
                        ImVec2(wordPos.x + wordW, wordPos.y + lineH - 1.0f), col);
            ImGui::PopStyleColor();
            if (s.clickableLinks && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!w.href.empty())
                    ImGui::SetTooltip("%s", w.href.c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !w.href.empty()) {
#if defined(_WIN32)
                    ShellExecuteA(nullptr, "open", w.href.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
                }
            }
        }
        if (w.marks & MARK_STRIKE) {
            const float midY = wordPos.y + lineH * 0.5f;
            dl->AddLine(ImVec2(wordPos.x, midY), ImVec2(wordPos.x + wordW, midY), ImGui::GetColorU32(ImGuiCol_Text));
        }
        ImGui::PopFont();

        curX += wordW;
        firstOnLine = false;
        prevWasCode = isCode;
    }
}

static void PreviewFlushBlock(PreviewState& s) {
    if (s.runs.empty() && s.headingLevel == 0)
        return;
    if (s.headingLevel > 0) {
        // Heading sizing: scale the bitmap font for h1–h3 since we don't bake
        // separate atlas sizes (Phase 4 budget). Bitmap rescale is mildly blurry
        // at non-1.0 ratios but cheap; users perceive size differentiation.
        // Tint only h1/h2 in cyan — at h3+ the size + bold weight already make
        // the heading stand out, so the color is doing redundant work.
        float scale = 1.0f;
        if (s.headingLevel == 1)
            scale = 1.6f;
        else if (s.headingLevel == 2)
            scale = 1.35f;
        else if (s.headingLevel == 3)
            scale = 1.15f;

        // Force bold on every run inside the heading. Inline em/code/links keep
        // their italic/mono/underline overlays via the other mark bits.
        for (StyledRun& r : s.runs)
            r.marks |= MARK_BOLD;

        const bool tinted = (s.headingLevel <= 2);
        const bool isTooltip = (s.mode == MarkdownPreviewRender::Mode::Tooltip);
        const float prevScale = ImGui::GetCurrentWindow()->FontWindowScale;
        if (!isTooltip && scale != 1.0f)
            ImGui::SetWindowFontScale(scale);
        if (tinted)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.95f, 1.0f, 1.0f));
        PreviewRenderRuns(s.runs, s);
        if (tinted)
            ImGui::PopStyleColor();
        if (!isTooltip && scale != 1.0f)
            ImGui::SetWindowFontScale(prevScale);
        if (!isTooltip && s.headingLevel <= 2)
            ImGui::Separator();
    } else if (!s.runs.empty()) {
        PreviewRenderRuns(s.runs, s);
    }
    s.runs.clear();
    s.headingLevel = 0;
    // Selectable text — bump the block-boundary counter so Ctrl+C inserts `\n`
    // between selected segments that span block boundaries.
    if (s.selCtx) {
        SelectableText::EndBlock(*s.selCtx);
    }
}

static int PreviewEnterBlock(MD_BLOCKTYPE type, void* detail, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    // Inside a nested table (rare, non-standard) we ignore everything until the
    // outer table closes — only the outermost table is rendered.
    if (s.tableDepth > 1) {
        if (type == MD_BLOCK_TABLE)
            ++s.tableDepth;
        return 0;
    }
    switch (type) {
    case MD_BLOCK_DOC:
        break;
    case MD_BLOCK_QUOTE:
        PreviewFlushBlock(s);
        ImGui::Indent(16.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 0.7f, 1.0f));
        break;
    case MD_BLOCK_UL:
        PreviewFlushBlock(s);
        s.listStack.push_back('-');
        s.orderedCounters.push_back(0);
        ++s.listDepth;
        break;
    case MD_BLOCK_OL: {
        PreviewFlushBlock(s);
        auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        s.listStack.push_back('1');
        s.orderedCounters.push_back(d ? static_cast<int>(d->start) - 1 : 0);
        ++s.listDepth;
        break;
    }
    case MD_BLOCK_LI: {
        PreviewFlushBlock(s);
        for (int i = 0; i < s.listDepth - 1; ++i)
            ImGui::Indent(16.0f);
        auto* lid = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        const bool task = lid && lid->is_task;
        const bool done = task && (lid->task_mark == 'x' || lid->task_mark == 'X');
        // The marker is unstyled, so push it through the regular-mark code path.
        // Task markers use Unicode ☑ / ☐ (U+2611 / U+2610) which are already in
        // the atlas via the Geometric Shapes / Misc Symbols ranges loaded by
        // SmatchetImGuiFonts; falls back gracefully if a glyph is missing.
        std::string marker;
        if (!s.listStack.empty() && s.listStack.back() == '1') {
            ++s.orderedCounters.back();
            marker = std::to_string(s.orderedCounters.back());
            if (task) {
                marker += done ? ". \xE2\x98\x91 " : ". \xE2\x98\x90 ";
            } else {
                marker += ". ";
            }
        } else {
            if (task) {
                marker = done ? "\xE2\x98\x91 " : "\xE2\x98\x90 ";
            } else {
                marker = "\xE2\x80\xA2 "; // U+2022 bullet
            }
        }
        const uint8_t savedMarks = s.currentMarks;
        const std::string savedHref = s.currentHref;
        s.currentMarks = 0;
        s.currentHref.clear();
        PreviewAppendRunString(s, marker);
        s.currentMarks = savedMarks;
        s.currentHref = savedHref;
        break;
    }
    case MD_BLOCK_TABLE: {
        // Flush any pending paragraph above the table, then begin collecting
        // cell runs. Render happens on MD_BLOCK_TABLE leave.
        PreviewFlushBlock(s);
        if (s.tableDepth > 0) {
            ++s.tableDepth;
            break; // nested table — skipped
        }
        ++s.tableDepth;
        auto* d = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
        s.tableColCount = d ? static_cast<int>(d->col_count) : 0;
        s.tableInHeader = false;
        s.tableRows.clear();
        s.tableCellRuns.clear();
        break;
    }
    case MD_BLOCK_THEAD:
        if (s.tableDepth == 1)
            s.tableInHeader = true;
        break;
    case MD_BLOCK_TBODY:
        if (s.tableDepth == 1)
            s.tableInHeader = false;
        break;
    case MD_BLOCK_TR: {
        if (s.tableDepth == 1) {
            TableRowData row;
            row.isHeader = s.tableInHeader;
            s.tableRows.push_back(std::move(row));
        }
        break;
    }
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (s.tableDepth == 1) {
            s.tableCellRuns.clear();
            s.activeRuns = &s.tableCellRuns;
            if (type == MD_BLOCK_TH && !s.tableRows.empty()) {
                s.tableRows.back().isHeader = true;
            }
        }
        break;
    case MD_BLOCK_HR:
        PreviewFlushBlock(s);
        ImGui::Separator();
        break;
    case MD_BLOCK_H: {
        PreviewFlushBlock(s);
        auto* d = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        s.headingLevel = d ? static_cast<int>(d->level) : 1;
        break;
    }
    case MD_BLOCK_CODE:
        PreviewFlushBlock(s);
        ++s.codeDepth;
        s.codeLang.clear();
        if (detail != nullptr) {
            auto* cd = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            if (cd->lang.text != nullptr && cd->lang.size > 0) {
                s.codeLang.assign(cd->lang.text, cd->lang.size);
            }
        }
        break;
    case MD_BLOCK_P:
        // Inside a list item the buffer is already primed with the marker; don't flush.
        if (s.listDepth == 0)
            PreviewFlushBlock(s);
        break;
    default:
        break;
    }
    return 0;
}

static int PreviewLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    if (type == MD_BLOCK_TABLE) {
        if (s.tableDepth > 1) {
            --s.tableDepth;
            return 0; // closing a nested skipped table
        }
        // Render the collected table.
        if (!s.tableRows.empty() && s.tableColCount > 0) {
            ImGui::PushID(s.tableNextId++);
            const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("##mdpreview_tbl", s.tableColCount, flags)) {
                for (TableRowData& row : s.tableRows) {
                    ImGui::TableNextRow();
                    const size_t cellMax = (std::min)(row.cells.size(), static_cast<size_t>(s.tableColCount));
                    for (size_t ci = 0; ci < cellMax; ++ci) {
                        ImGui::TableNextColumn();
                        TableCellData& cell = row.cells[ci];
                        if (row.isHeader) {
                            for (StyledRun& r : cell.runs)
                                r.marks |= MARK_BOLD;
                        }
                        PreviewRenderRuns(cell.runs, s);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
        s.tableRows.clear();
        s.tableCellRuns.clear();
        s.tableInHeader = false;
        s.tableColCount = 0;
        s.tableDepth = 0;
        s.activeRuns = nullptr;
        return 0;
    }
    // While inside a nested skipped table, ignore everything.
    if (s.tableDepth > 1)
        return 0;
    if (type == MD_BLOCK_TH || type == MD_BLOCK_TD) {
        if (s.tableDepth == 1) {
            if (!s.tableRows.empty()) {
                TableCellData cell;
                cell.runs = std::move(s.tableCellRuns);
                s.tableRows.back().cells.push_back(std::move(cell));
            }
            s.tableCellRuns.clear();
            s.activeRuns = nullptr;
        }
        return 0;
    }
    if (type == MD_BLOCK_TR || type == MD_BLOCK_THEAD || type == MD_BLOCK_TBODY) {
        return 0;
    }
    switch (type) {
    case MD_BLOCK_DOC:
        PreviewFlushBlock(s);
        break;
    case MD_BLOCK_QUOTE:
        PreviewFlushBlock(s);
        ImGui::PopStyleColor();
        ImGui::Unindent(16.0f);
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        PreviewFlushBlock(s);
        if (!s.listStack.empty())
            s.listStack.pop_back();
        if (!s.orderedCounters.empty())
            s.orderedCounters.pop_back();
        if (s.listDepth > 0)
            --s.listDepth;
        break;
    case MD_BLOCK_LI:
        PreviewFlushBlock(s);
        for (int i = 0; i < s.listDepth - 1; ++i)
            ImGui::Unindent(16.0f);
        break;
    case MD_BLOCK_CODE: {
        const SmatchetPreviewFonts& fonts = SmatchetGetPreviewFonts();
        const bool isCpp = MarkdownPreviewRender::IsCppLikeLangTag(s.codeLang);
        if (s.mode == MarkdownPreviewRender::Mode::Tooltip) {
            // Nested child windows inside a tooltip can blow the auto-sized
            // bounds — render inline with the monospace font + dark tint only.
            if (fonts.Mono)
                ImGui::PushFont(fonts.Mono);
            if (isCpp) {
                DrawColoredCppText(s.codeBuffer.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.6f, 1.0f));
                ImGui::TextUnformatted(s.codeBuffer.c_str());
                ImGui::PopStyleColor();
            }
            if (fonts.Mono)
                ImGui::PopFont();
        } else {
            // Render the accumulated code-block text in a child with monospace styling.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
            const float h = ImGui::GetTextLineHeightWithSpacing() *
                            static_cast<float>(1 + std::count(s.codeBuffer.begin(), s.codeBuffer.end(), '\n'));
            ImGui::BeginChild("##mdpreview_code", ImVec2(-FLT_MIN, std::min(h + 12.0f, 240.0f)), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            if (fonts.Mono)
                ImGui::PushFont(fonts.Mono);
            if (isCpp) {
                DrawColoredCppText(s.codeBuffer.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.6f, 1.0f));
                ImGui::TextUnformatted(s.codeBuffer.c_str());
                ImGui::PopStyleColor();
            }
            if (fonts.Mono)
                ImGui::PopFont();
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        s.codeBuffer.clear();
        s.codeLang.clear();
        if (s.codeDepth > 0)
            --s.codeDepth;
        break;
    }
    case MD_BLOCK_H:
    case MD_BLOCK_P:
        PreviewFlushBlock(s);
        break;
    default:
        break;
    }
    return 0;
}

static int PreviewEnterSpan(MD_SPANTYPE type, void* detail, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    if (s.tableDepth > 1)
        return 0;
    switch (type) {
    case MD_SPAN_STRONG:
        s.currentMarks |= MARK_BOLD;
        break;
    case MD_SPAN_EM:
        s.currentMarks |= MARK_ITALIC;
        break;
    case MD_SPAN_CODE:
        s.currentMarks |= MARK_CODE;
        break;
    case MD_SPAN_DEL:
        s.currentMarks |= MARK_STRIKE;
        break;
    case MD_SPAN_A: {
        s.currentMarks |= MARK_LINK;
        auto* d = static_cast<MD_SPAN_A_DETAIL*>(detail);
        if (d && d->href.text && d->href.size > 0) {
            s.currentHref.assign(d->href.text, d->href.size);
        } else {
            s.currentHref.clear();
        }
        break;
    }
    case MD_SPAN_IMG:
        ++s.imgSpanDepth;
        s.imgAltAccum.clear();
        break;
    default:
        break;
    }
    return 0;
}

static int PreviewLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    if (s.tableDepth > 1)
        return 0;
    switch (type) {
    case MD_SPAN_STRONG:
        s.currentMarks &= ~MARK_BOLD;
        break;
    case MD_SPAN_EM:
        s.currentMarks &= ~MARK_ITALIC;
        break;
    case MD_SPAN_CODE:
        s.currentMarks &= ~MARK_CODE;
        break;
    case MD_SPAN_DEL:
        s.currentMarks &= ~MARK_STRIKE;
        break;
    case MD_SPAN_A:
        s.currentMarks &= ~MARK_LINK;
        s.currentHref.clear();
        break;
    case MD_SPAN_IMG:
        if (s.imgSpanDepth > 0)
            --s.imgSpanDepth;
        PreviewAppendRunString(s, s.imgAltAccum.empty() ? std::string("[image]") : s.imgAltAccum);
        s.imgAltAccum.clear();
        break;
    default:
        break;
    }
    return 0;
}

static int PreviewText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    if (s.tableDepth > 1)
        return 0;
    if (type == MD_TEXT_NULLCHAR)
        return 0;
    if (type == MD_TEXT_HTML) {
#ifndef NDEBUG
        if (!s.debugLoggedMdTextHtml) {
            s.debugLoggedMdTextHtml = true;
            LOG_DEBUG("md4c: unexpected MD_TEXT_HTML under NOHTML (preview, first chunk size=%u)",
                      static_cast<unsigned>(size));
        }
#endif
        return 0;
    }
    if (s.imgSpanDepth > 0) {
        if (type == MD_TEXT_NORMAL || type == MD_TEXT_ENTITY || type == MD_TEXT_CODE) {
            s.imgAltAccum.append(text, size);
            return 0;
        }
        if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
            s.imgAltAccum += ' ';
            return 0;
        }
        return 0;
    }
    // Code blocks bypass the styled-run pipeline so whitespace and newlines stay
    // verbatim. Inline `code` spans (handled via MARK_CODE on currentMarks) still
    // go through the run path so they wrap with surrounding text.
    if (s.codeDepth > 0 && (s.currentMarks & MARK_CODE) == 0) {
        s.codeBuffer.append(text, size);
        return 0;
    }
    if (type == MD_TEXT_BR) {
        // Hard break — mid-paragraph forced wrap.
        PreviewAppendRunBytes(s, "\n", 1);
        return 0;
    }
    if (type == MD_TEXT_SOFTBR) {
        // Soft break — collapse to a space; matches HTML rendering.
        PreviewAppendRunBytes(s, " ", 1);
        return 0;
    }
    PreviewAppendRunBytes(s, text, size);
    return 0;
}

} // namespace

namespace MarkdownPreviewRender {

void Render(const std::string& md, const Options& opts) {
    PreviewState state;
    state.mode = opts.mode;
    state.clickableLinks = opts.clickableLinks;
    state.fixedWrapWidth = opts.wrapWidth;
    // Selectable text overlay — three modes:
    //   (a) `existingSelCtx` non-null: register segments into the caller's
    //       Context, don't open/close one here. Used by the AI chat surface
    //       where one outer Begin/End spans many sequential Render() calls.
    //   (b) `selectableId` non-empty: open + close our own Context inline.
    //       Used by the description preview (one Render = one selection
    //       region).
    //   (c) Neither set: legacy non-selectable path.
    // Code blocks + tables remain non-selectable in this MVP (deferred per
    // the slice-4 plan).
    SelectableText::Context* selCtx = nullptr;
    bool ownsSelCtx = false;
    if (opts.existingSelCtx != nullptr) {
        selCtx = opts.existingSelCtx;
        state.selCtx = selCtx;
    } else if (opts.selectableId != nullptr && opts.selectableId[0] != '\0') {
        selCtx = &SelectableText::Begin(opts.selectableId);
        state.selCtx = selCtx;
        ownsSelCtx = true;
    }
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MarkdownConvert::Md4cParserFlags();
    parser.enter_block = PreviewEnterBlock;
    parser.leave_block = PreviewLeaveBlock;
    parser.enter_span = PreviewEnterSpan;
    parser.leave_span = PreviewLeaveSpan;
    parser.text = PreviewText;
    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &state);
    PreviewFlushBlock(state);
    if (ownsSelCtx) {
        SelectableText::End(*selCtx);
    }
}

} // namespace MarkdownPreviewRender
