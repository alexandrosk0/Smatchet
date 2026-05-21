#include "MarkdownPreviewRender.h"

#include "CodeColorView.h"
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

// Bring the public type aliases into the anonymous namespace so existing
// internal code can keep using `StyledRun`, `MARK_BOLD`, etc. unchanged.
using MarkdownPreviewRender::PlanWord;
using MarkdownPreviewRender::PreviewPlan;
using MarkdownPreviewRender::StyledRun;
using MarkdownPreviewRender::TableCellData;
using MarkdownPreviewRender::TableRowData;
constexpr std::uint8_t MARK_BOLD = MarkdownPreviewRender::kMarkBold;
constexpr std::uint8_t MARK_ITALIC = MarkdownPreviewRender::kMarkItalic;
constexpr std::uint8_t MARK_CODE = MarkdownPreviewRender::kMarkCode;
constexpr std::uint8_t MARK_STRIKE = MarkdownPreviewRender::kMarkStrike;
constexpr std::uint8_t MARK_LINK = MarkdownPreviewRender::kMarkLink;

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
    /// Monotonic counter for code-block child windows so each block gets a
    /// unique ImGui ID (the literal "##mdpreview_code" would collide across
    /// multiple blocks in the same render and share scroll state).
    int codeBlockNextId = 0;
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
    /// When non-null, parse-side callbacks APPEND plan blocks to this instead
    /// of calling ImGui directly. The corresponding `RenderPlan` then walks
    /// the plan and emits ImGui. Pillar 1 opt #1 — cache machinery.
    /// (Font cache lives on RenderState now — parse doesn't measure glyphs.)
    PreviewPlan* outPlan = nullptr;
};

/// Append a paragraph or heading block to the plan. Drains the in-progress
/// `runs` accumulator. Called from PreviewFlushBlock's plan-build path.
static void PlanAppendFlush(PreviewState& s) {
    if (s.runs.empty() && s.headingLevel == 0) {
        return;
    }
    PreviewPlan::Block b;
    if (s.headingLevel > 0) {
        b.kind = PreviewPlan::Block::kHeading;
        b.headingLevel = s.headingLevel;
        // Force bold on every run inside the heading. Inline em/code/links
        // keep their italic/mono/underline overlays via the other mark bits.
        // Bake this in at build time so RenderPlan doesn't need the runtime
        // mutation pass.
        for (StyledRun& r : s.runs) {
            r.marks |= MARK_BOLD;
        }
    } else {
        b.kind = PreviewPlan::Block::kPara;
    }
    b.runs = std::move(s.runs);
    s.outPlan->blocks.push_back(std::move(b));
    s.runs.clear();
    s.headingLevel = 0;
}

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

// PreviewPickFont / ResolveFonts (parse-side variants) retired with Opt 1
// refactor — parse no longer measures glyphs. PickFontRS / ResolveFontsRS
// in the RenderState-side handle the same role for the render pass.

// Parse-side flush — appends an accumulated paragraph/heading block to the
// active plan. No ImGui calls. The actual draw happens in `RenderPlan`.
static void PreviewFlushBlock(PreviewState& s) { PlanAppendFlush(s); }

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
    case MD_BLOCK_QUOTE: {
        PreviewFlushBlock(s);
        PreviewPlan::Block b;
        b.kind = PreviewPlan::Block::kQuoteBegin;
        s.outPlan->blocks.push_back(std::move(b));
        break;
    }
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
        // Resolve the marker at parse time so the counter is baked into the
        // plan (RenderPlan doesn't need to know about ordered-list state).
        auto* lid = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        const bool task = lid && lid->is_task;
        const bool done = task && (lid->task_mark == 'x' || lid->task_mark == 'X');
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
        // Emit a kListItemBegin block carrying the marker + depth. RenderPlan
        // does the indents + emits the marker as a leading styled run on the
        // next paragraph flush.
        PreviewPlan::Block b;
        b.kind = PreviewPlan::Block::kListItemBegin;
        b.listMarker = marker;
        b.listDepth = s.listDepth;
        s.outPlan->blocks.push_back(std::move(b));
        // Also prime the runs accumulator with the marker so the very next
        // paragraph flush includes it (matches the legacy emit shape).
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
    case MD_BLOCK_HR: {
        PreviewFlushBlock(s);
        PreviewPlan::Block b;
        b.kind = PreviewPlan::Block::kHr;
        s.outPlan->blocks.push_back(std::move(b));
        break;
    }
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
        // Snapshot the collected table into a plan block. Header-bold
        // marker mutation is baked in here so RenderPlan stays mutation-free.
        if (!s.tableRows.empty() && s.tableColCount > 0) {
            PreviewPlan::Block b;
            b.kind = PreviewPlan::Block::kTable;
            b.tableColCount = s.tableColCount;
            for (TableRowData& row : s.tableRows) {
                if (row.isHeader) {
                    for (TableCellData& cell : row.cells) {
                        for (StyledRun& r : cell.runs) {
                            r.marks |= MARK_BOLD;
                        }
                    }
                }
            }
            b.tableRows = std::move(s.tableRows);
            s.outPlan->blocks.push_back(std::move(b));
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
    case MD_BLOCK_QUOTE: {
        PreviewFlushBlock(s);
        PreviewPlan::Block b;
        b.kind = PreviewPlan::Block::kQuoteEnd;
        s.outPlan->blocks.push_back(std::move(b));
        break;
    }
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
    case MD_BLOCK_LI: {
        PreviewFlushBlock(s);
        PreviewPlan::Block b;
        b.kind = PreviewPlan::Block::kListItemEnd;
        b.listDepth = s.listDepth;
        s.outPlan->blocks.push_back(std::move(b));
        break;
    }
    case MD_BLOCK_CODE: {
        PreviewPlan::Block b;
        b.kind = PreviewPlan::Block::kCode;
        b.codeBuffer = std::move(s.codeBuffer);
        b.codeLang = std::move(s.codeLang);
        s.outPlan->blocks.push_back(std::move(b));
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

// Forward decl for the runtime state used by RenderPlanInternal. Distinct from
// PreviewState (which is parse-only) so the plan-build path never touches
// ImGui and the render path never touches md4c.
struct RenderState {
    MarkdownPreviewRender::Mode mode = MarkdownPreviewRender::Mode::Full;
    bool clickableLinks = true;
    float fixedWrapWidth = 0.0f;
    SelectableText::Context* selCtx = nullptr;
    // Per-Render font cache (mirrors PreviewState fields).
    ImFont* fontRegular = nullptr;
    ImFont* fontBold = nullptr;
    ImFont* fontItalic = nullptr;
    ImFont* fontBoldItalic = nullptr;
    ImFont* fontMono = nullptr;
    // Per-render unique-ID counters — bumped each time a code block / table
    // child window is opened so multiple blocks in one render don't share
    // BeginChild scroll state.
    int tableNextId = 0;
    int codeBlockNextId = 0;
};

static ImFont* PickFontRS(const RenderState& r, uint8_t marks) {
    if (marks & MARK_CODE) {
        return r.fontMono;
    }
    const bool bold = (marks & MARK_BOLD) != 0;
    const bool italic = (marks & MARK_ITALIC) != 0;
    if (bold && italic) {
        return r.fontBoldItalic;
    }
    if (bold) {
        return r.fontBold;
    }
    if (italic) {
        return r.fontItalic;
    }
    return r.fontRegular;
}

static void ResolveFontsRS(RenderState& r) {
    const SmatchetPreviewFonts& f = SmatchetGetPreviewFonts();
    r.fontRegular = f.Regular;
    r.fontBold = f.Bold ? f.Bold : f.Regular;
    r.fontItalic = f.Italic ? f.Italic : f.Regular;
    r.fontBoldItalic = f.BoldItalic ? f.BoldItalic : (f.Bold ? f.Bold : f.Regular);
    r.fontMono = f.Mono ? f.Mono : f.Regular;
}

// Render a paragraph or heading from cached StyledRuns. Mirrors the legacy
// PreviewRenderRuns but reads from RenderState (runtime) rather than the
// parse-time PreviewState.
// Pure decomposition — splits runs into PlanWords + measures widths. Pushes
// fonts as needed for `ImGui::CalcTextSize` but emits no glyphs / draws.
// Used by both the cached block path (b.words populated once) and the
// per-cell table path (local stack vector). Opt B.
static void DecomposeAndMeasure(const std::vector<StyledRun>& runs, const RenderState& r,
                                std::vector<PlanWord>& outWords) {
    outWords.clear();
    outWords.reserve(runs.size() * 4);
    for (const StyledRun& run : runs) {
        size_t i = 0;
        const std::string& t = run.text;
        while (i < t.size()) {
            unsigned char c = static_cast<unsigned char>(t[i]);
            if (c == '\n') {
                PlanWord w;
                w.forceBreak = true;
                outWords.push_back(std::move(w));
                ++i;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++i;
            } else {
                size_t j = i;
                while (j < t.size()) {
                    unsigned char d = static_cast<unsigned char>(t[j]);
                    if (d == ' ' || d == '\t' || d == '\n' || d == '\r')
                        break;
                    ++j;
                }
                PlanWord w;
                w.text.assign(t.data() + i, j - i);
                w.marks = run.marks;
                w.href = run.href;
                ImFont* f = PickFontRS(r, w.marks);
                ImGui::PushFont(f);
                w.widthPx = ImGui::CalcTextSize(w.text.data(), w.text.data() + w.text.size()).x;
                ImGui::PopFont();
                outWords.push_back(std::move(w));
                i = j;
            }
        }
    }
}

// Direct-AddText emit. Bypasses per-word ImGui::PushFont / TextUnformatted /
// PopFont / SameLine — all glyphs land via one `ImDrawList::AddText` call per
// word. Manual cursor (curX, curY) walks the wrap layout in screen-space.
// Layout space is claimed at the end via a single `ImGui::Dummy` so siblings
// flow correctly. Link hover/click uses our own `IsMouseHoveringRect` instead
// of `IsItemHovered` (no ItemAdd anywhere). Opt B.
static void EmitWordsRS(const std::vector<PlanWord>& words, const RenderState& r) {
    if (words.empty()) {
        return;
    }
    const float wrapWidth = (r.fixedWrapWidth > 0.0f) ? r.fixedWrapWidth : ImGui::GetContentRegionAvail().x;
    const float effectiveWrap = (wrapWidth > 0.0f) ? wrapWidth : 1.0f;

    // Space width with the regular font — close enough for inter-word spacing
    // regardless of the surrounding word's font variant.
    ImGui::PushFont(PickFontRS(r, 0));
    const float spaceW = ImGui::CalcTextSize(" ").x;
    const float lineH = ImGui::GetTextLineHeight();
    ImFont* spaceFont = ImGui::GetFont();
    ImGui::PopFont();

    const float fontSize = ImGui::GetFontSize();

    const ImVec2 startScreen = ImGui::GetCursorScreenPos();
    float curX = startScreen.x;
    float curY = startScreen.y;
    bool firstOnLine = true;
    bool prevWasCode = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 codeBgCol = IM_COL32(48, 48, 60, 200);
    const float codeBgRound = 3.0f;
    const float codeBgPadX = 2.0f;
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 linkCol = ImGui::GetColorU32(ImVec4(0.45f, 0.78f, 1.0f, 1.0f));

    for (size_t wi = 0; wi < words.size(); ++wi) {
        const PlanWord& w = words[wi];
        if (w.forceBreak) {
            curY += lineH;
            curX = startScreen.x;
            firstOnLine = true;
            prevWasCode = false;
            continue;
        }
        const float wordW = w.widthPx;
        const float leadingW = firstOnLine ? 0.0f : spaceW;
        if (!firstOnLine && (curX - startScreen.x) + leadingW + wordW > effectiveWrap) {
            curY += lineH;
            curX = startScreen.x;
            firstOnLine = true;
            prevWasCode = false;
        }
        const bool isCode = (w.marks & MARK_CODE) != 0;
        const bool isLink = (w.marks & MARK_LINK) != 0;

        if (!firstOnLine) {
            const ImVec2 spacePos(curX, curY);
            if (prevWasCode && isCode) {
                dl->AddRectFilled(spacePos, ImVec2(curX + spaceW, curY + lineH), codeBgCol, 0.0f);
            }
            if (r.selCtx) {
                const char* spaceLit = " ";
                SelectableText::RegisterSegment(*r.selCtx, spaceLit, spaceLit + 1, spacePos, lineH, spaceFont, spaceW,
                                                nullptr);
            }
            curX += spaceW;
        }

        const ImVec2 wordPos(curX, curY);
        ImFont* font = PickFontRS(r, w.marks);

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

        // Single ImDrawList::AddText — no ImGui::PushFont/TextUnformatted/PopFont/SameLine.
        const ImU32 glyphCol = isLink ? linkCol : textCol;
        dl->AddText(font, fontSize, wordPos, glyphCol, w.text.data(), w.text.data() + w.text.size());

        if (r.selCtx) {
            SelectableText::RegisterSegment(
                *r.selCtx, w.text.data(), w.text.data() + w.text.size(), wordPos, lineH, font, wordW,
                isLink && !w.href.empty() ? const_cast<void*>(static_cast<const void*>(w.href.c_str())) : nullptr);
        }

        if (isLink) {
            dl->AddLine(ImVec2(wordPos.x, wordPos.y + lineH - 1.0f),
                        ImVec2(wordPos.x + wordW, wordPos.y + lineH - 1.0f), textCol);
            if (r.clickableLinks) {
                const ImVec2 wMin(wordPos.x, wordPos.y);
                const ImVec2 wMax(wordPos.x + wordW, wordPos.y + lineH);
                if (ImGui::IsMouseHoveringRect(wMin, wMax)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (!w.href.empty()) {
                        ImGui::SetTooltip("%s", w.href.c_str());
                    }
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !w.href.empty()) {
#if defined(_WIN32)
                        ShellExecuteA(nullptr, "open", w.href.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
                    }
                }
            }
        }
        if (w.marks & MARK_STRIKE) {
            const float midY = wordPos.y + lineH * 0.5f;
            dl->AddLine(ImVec2(wordPos.x, midY), ImVec2(wordPos.x + wordW, midY), textCol);
        }

        curX += wordW;
        firstOnLine = false;
        prevWasCode = isCode;
    }

    // Claim the layout footprint with a single Dummy. Width = wrap width so
    // subsequent widgets don't try to fit on the same line; height = (lines)
    // * lineH from the start cursor.
    const float totalHeight = (curY + lineH) - startScreen.y;
    ImGui::Dummy(ImVec2(effectiveWrap, totalHeight));
}

// Cached path for kPara / kHeading — decompose once, emit many.
static void RenderBlockRuns(const PreviewPlan::Block& b, const RenderState& r) {
    if (!b.wordsBuilt) {
        DecomposeAndMeasure(b.runs, r, b.words);
        b.wordsBuilt = true;
    }
    EmitWordsRS(b.words, r);
}

// Uncached path for table cells — each cell's runs are separate; no shared cache.
static void RenderUncachedRuns(const std::vector<StyledRun>& runs, const RenderState& r) {
    if (runs.empty()) {
        return;
    }
    std::vector<PlanWord> tmpWords;
    DecomposeAndMeasure(runs, r, tmpWords);
    EmitWordsRS(tmpWords, r);
}

// Emit a single plan block as ImGui draw calls.
static void RenderPlanBlock(const PreviewPlan::Block& b, RenderState& r) {
    using BK = PreviewPlan::Block;
    switch (b.kind) {
    case BK::kPara:
        RenderBlockRuns(b, r);
        if (r.selCtx) {
            SelectableText::EndBlock(*r.selCtx);
        }
        break;
    case BK::kHeading: {
        float scale = 1.0f;
        if (b.headingLevel == 1)
            scale = 1.6f;
        else if (b.headingLevel == 2)
            scale = 1.35f;
        else if (b.headingLevel == 3)
            scale = 1.15f;
        const bool tinted = (b.headingLevel <= 2);
        const bool isTooltip = (r.mode == MarkdownPreviewRender::Mode::Tooltip);
        const float prevScale = ImGui::GetCurrentWindow()->FontWindowScale;
        if (!isTooltip && scale != 1.0f)
            ImGui::SetWindowFontScale(scale);
        if (tinted)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.95f, 1.0f, 1.0f));
        RenderBlockRuns(b, r);
        if (tinted)
            ImGui::PopStyleColor();
        if (!isTooltip && scale != 1.0f)
            ImGui::SetWindowFontScale(prevScale);
        if (!isTooltip && b.headingLevel <= 2)
            ImGui::Separator();
        if (r.selCtx) {
            SelectableText::EndBlock(*r.selCtx);
        }
        break;
    }
    case BK::kHr:
        ImGui::Separator();
        if (r.selCtx) {
            SelectableText::EndBlock(*r.selCtx);
        }
        break;
    case BK::kQuoteBegin:
        ImGui::Indent(16.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 0.7f, 1.0f));
        break;
    case BK::kQuoteEnd:
        ImGui::PopStyleColor();
        ImGui::Unindent(16.0f);
        break;
    case BK::kListItemBegin:
        for (int i = 0; i < b.listDepth - 1; ++i) {
            ImGui::Indent(16.0f);
        }
        break;
    case BK::kListItemEnd:
        for (int i = 0; i < b.listDepth - 1; ++i) {
            ImGui::Unindent(16.0f);
        }
        break;
    case BK::kCode: {
        const SmatchetPreviewFonts& fonts = SmatchetGetPreviewFonts();
        // Slice 2 of docs/design/code-syntax-coloring-and-tooltips.md — route
        // every markdown code block through CodeColorView. C++/C delegates to
        // the existing DrawColoredCppText (zero behaviour change on Smatchet's
        // dominant path); Plain falls back to the same flat-orange tint the
        // old code used; every other language now gets per-language colouring.
        const smatchet::code_color::CodeLang lang = smatchet::code_color::FromTag(b.codeLang);
        if (r.mode == MarkdownPreviewRender::Mode::Tooltip) {
            if (fonts.Mono)
                ImGui::PushFont(fonts.Mono);
            smatchet::code_color::DrawColoredCodeBlock(b.codeBuffer.c_str(), lang, b.codeLang);
            if (fonts.Mono)
                ImGui::PopFont();
        } else {
            ImGui::PushID(r.codeBlockNextId++);
            const float availW = ImGui::GetContentRegionAvail().x;
            const float btnW = 60.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availW - btnW);
            if (ImGui::SmallButton("Copy##codeblock")) {
                ImGui::SetClipboardText(b.codeBuffer.c_str());
            }
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
            const float h = ImGui::GetTextLineHeightWithSpacing() *
                            static_cast<float>(1 + std::count(b.codeBuffer.begin(), b.codeBuffer.end(), '\n'));
            ImGui::BeginChild("##mdpreview_code", ImVec2(-FLT_MIN, std::min(h + 12.0f, 240.0f)), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            if (fonts.Mono)
                ImGui::PushFont(fonts.Mono);
            smatchet::code_color::DrawColoredCodeBlock(b.codeBuffer.c_str(), lang, b.codeLang);
            if (fonts.Mono)
                ImGui::PopFont();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (r.selCtx) {
            SelectableText::EndBlock(*r.selCtx);
        }
        break;
    }
    case BK::kTable: {
        if (!b.tableRows.empty() && b.tableColCount > 0) {
            ImGui::PushID(r.tableNextId++);
            const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("##mdpreview_tbl", b.tableColCount, flags)) {
                for (const TableRowData& row : b.tableRows) {
                    ImGui::TableNextRow();
                    const size_t cellMax = (std::min)(row.cells.size(), static_cast<size_t>(b.tableColCount));
                    for (size_t ci = 0; ci < cellMax; ++ci) {
                        ImGui::TableNextColumn();
                        const TableCellData& cell = row.cells[ci];
                        RenderUncachedRuns(cell.runs, r);
                        if (r.selCtx) {
                            SelectableText::EndBlock(*r.selCtx);
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
        break;
    }
    }
}

} // namespace

namespace MarkdownPreviewRender {

void PreviewPlanDeleter::operator()(PreviewPlan* p) const noexcept { delete p; }

PreviewPlanPtr MakePlan() { return PreviewPlanPtr(new PreviewPlan(), PreviewPlanDeleter{}); }

// FNV-1a 64-bit. Stable cross-run; fine for in-process cache keying.
std::uint64_t HashContent(const char* bytes, std::size_t len) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i]));
        h *= 0x100000001b3ULL;
    }
    return h;
}

void BuildPlan(const std::string& md, PreviewPlan& outPlan) {
    outPlan.blocks.clear();
    PreviewState state;
    state.outPlan = &outPlan;
    state.runs.reserve(16);
    state.codeBuffer.reserve(256);
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MarkdownConvert::Md4cParserFlags();
    parser.enter_block = PreviewEnterBlock;
    parser.leave_block = PreviewLeaveBlock;
    parser.enter_span = PreviewEnterSpan;
    parser.leave_span = PreviewLeaveSpan;
    parser.text = PreviewText;
    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &state);
    // Final flush — any trailing in-progress block lands as a plan entry.
    PreviewFlushBlock(state);
}

void RenderPlan(const PreviewPlan& plan, const Options& opts) {
    RenderState r;
    r.mode = opts.mode;
    r.clickableLinks = opts.clickableLinks;
    r.fixedWrapWidth = opts.wrapWidth;
    ResolveFontsRS(r);

    // Selectable text overlay — three modes:
    //   (a) `existingSelCtx` non-null: register segments into the caller's
    //       Context, don't open/close one here.
    //   (b) `selectableId` non-empty: open + close our own Context inline.
    //   (c) Neither set: legacy non-selectable path.
    SelectableText::Context* selCtx = nullptr;
    bool ownsSelCtx = false;
    if (opts.existingSelCtx != nullptr) {
        selCtx = opts.existingSelCtx;
        r.selCtx = selCtx;
    } else if (opts.selectableId != nullptr && opts.selectableId[0] != '\0') {
        selCtx = &SelectableText::Begin(opts.selectableId);
        r.selCtx = selCtx;
        ownsSelCtx = true;
    }

    for (std::size_t i = 0; i < plan.blocks.size(); ++i) {
        RenderPlanBlock(plan.blocks[i], r);
    }

    if (ownsSelCtx) {
        SelectableText::End(*selCtx);
    }
}

void Render(const std::string& md, const Options& opts) {
    PreviewPlan plan;
    BuildPlan(md, plan);
    RenderPlan(plan, opts);
}

} // namespace MarkdownPreviewRender
