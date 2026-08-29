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
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
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
    /// Active SelectableText context. Non-null → prose runs drawn inside
    /// `PreviewRenderRuns` are recorded via `SelectableText::RegisterSegment`
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

// Resolve the ordered/unordered/task list-item marker string at parse time so
// the counter is baked into the plan. Pure (no ImGui); mutates the ordered
// counter on `s` exactly as the inline switch case did.
static std::string PreviewResolveListMarker(PreviewState& s, void* detail) {
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
    return marker;
}

// MD_BLOCK_LI enter handling — emit a kListItemBegin block carrying the marker
// + depth, then prime the runs accumulator with the marker (matches the legacy
// emit shape). No ImGui calls.
static void PreviewEnterListItem(PreviewState& s, void* detail) {
    PreviewFlushBlock(s);
    const std::string marker = PreviewResolveListMarker(s, detail);
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
}

// MD_BLOCK_TABLE enter handling — flush any pending paragraph then begin
// collecting cell runs. Render happens on MD_BLOCK_TABLE leave. No ImGui calls.
static void PreviewEnterTable(PreviewState& s, void* detail) {
    PreviewFlushBlock(s);
    if (s.tableDepth > 0) {
        ++s.tableDepth;
        return; // nested table — skipped
    }
    ++s.tableDepth;
    auto* d = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
    s.tableColCount = d ? static_cast<int>(d->col_count) : 0;
    s.tableInHeader = false;
    s.tableRows.clear();
    s.tableCellRuns.clear();
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
    case MD_BLOCK_LI:
        PreviewEnterListItem(s, detail);
        break;
    case MD_BLOCK_TABLE:
        PreviewEnterTable(s, detail);
        break;
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

// Table block-leave handling: snapshot a finished table into a plan block (baking header-bold
// marks), close cells, and ignore everything inside a nested skipped table. Returns true when the
// type (or current skipped-table state) is fully handled, so PreviewLeaveBlock can early-return.
static bool PreviewTryLeaveTableBlock(PreviewState& s, MD_BLOCKTYPE type) {
    if (type == MD_BLOCK_TABLE) {
        if (s.tableDepth > 1) {
            --s.tableDepth;
            return true; // closing a nested skipped table
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
        return true;
    }
    // While inside a nested skipped table, ignore everything.
    if (s.tableDepth > 1)
        return true;
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
        return true;
    }
    if (type == MD_BLOCK_TR || type == MD_BLOCK_THEAD || type == MD_BLOCK_TBODY) {
        return true;
    }
    return false;
}

static int PreviewLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    if (PreviewTryLeaveTableBlock(s, type)) {
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

// Loop-invariant context shared across the per-word emit helpers. Bundles the
// draw list, palette, and metrics computed once at the top of EmitWordsRS.
struct EmitCtx {
    ImDrawList* dl;
    const RenderState* r;
    float lineH;
    float fontSize;
    ImU32 textCol;
    ImU32 linkCol;
    ImU32 codeBgCol;
    float codeBgRound;
    float codeBgPadX;
};

// Draw the inline-code rounded background rect behind a code word. Extracted
// from the EmitWordsRS per-word loop verbatim. ImGui-emitting.
static void EmitCodeBackground(const EmitCtx& ctx, const std::vector<PlanWord>& words, size_t wi, const ImVec2& wordPos,
                               float wordW, bool prevWasCode) {
    const bool nextIsCode =
        (wi + 1 < words.size()) && !words[wi + 1].forceBreak && (words[wi + 1].marks & MARK_CODE) != 0;
    const float rl = prevWasCode ? 0.0f : ctx.codeBgRound;
    const float rr = nextIsCode ? 0.0f : ctx.codeBgRound;
    const ImVec2 r0(wordPos.x - (prevWasCode ? 0.0f : ctx.codeBgPadX), wordPos.y);
    const ImVec2 r1(wordPos.x + wordW + (nextIsCode ? 0.0f : ctx.codeBgPadX), wordPos.y + ctx.lineH);
    const ImDrawFlags flags = (rl > 0.0f ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersNone) |
                              (rr > 0.0f ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
    ctx.dl->AddRectFilled(r0, r1, ctx.codeBgCol, std::max(rl, rr), flags);
}

// Emit one word's glyphs + link underline / hover-click + strike line. Extracted
// from the EmitWordsRS per-word loop verbatim. ImGui-emitting.
static void EmitWordGlyph(const EmitCtx& ctx, const PlanWord& w, const ImVec2& wordPos, float wordW, ImFont* font,
                          bool isLink) {
    // Single ImDrawList::AddText — no ImGui::PushFont/TextUnformatted/PopFont/SameLine.
    const ImU32 glyphCol = isLink ? ctx.linkCol : ctx.textCol;
    ctx.dl->AddText(font, ctx.fontSize, wordPos, glyphCol, w.text.data(), w.text.data() + w.text.size());

    if (ctx.r->selCtx) {
        SelectableText::RegisterSegment(
            *ctx.r->selCtx, w.text.data(), w.text.data() + w.text.size(), wordPos, ctx.lineH, font, wordW,
            isLink && !w.href.empty() ? const_cast<void*>(static_cast<const void*>(w.href.c_str())) : nullptr);
    }

    if (isLink) {
        ctx.dl->AddLine(ImVec2(wordPos.x, wordPos.y + ctx.lineH - 1.0f),
                        ImVec2(wordPos.x + wordW, wordPos.y + ctx.lineH - 1.0f), ctx.textCol);
        if (ctx.r->clickableLinks) {
            const ImVec2 wMin(wordPos.x, wordPos.y);
            const ImVec2 wMax(wordPos.x + wordW, wordPos.y + ctx.lineH);
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
        const float midY = wordPos.y + ctx.lineH * 0.5f;
        ctx.dl->AddLine(ImVec2(wordPos.x, midY), ImVec2(wordPos.x + wordW, midY), ctx.textCol);
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

    EmitCtx ctx;
    ctx.dl = ImGui::GetWindowDrawList();
    ctx.r = &r;
    ctx.lineH = lineH;
    ctx.fontSize = fontSize;
    ctx.textCol = ImGui::GetColorU32(ImGuiCol_Text);
    ctx.linkCol = ImGui::GetColorU32(ImVec4(0.45f, 0.78f, 1.0f, 1.0f));
    ctx.codeBgCol = IM_COL32(48, 48, 60, 200);
    ctx.codeBgRound = 3.0f;
    ctx.codeBgPadX = 2.0f;

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
                ctx.dl->AddRectFilled(spacePos, ImVec2(curX + spaceW, curY + lineH), ctx.codeBgCol, 0.0f);
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
            EmitCodeBackground(ctx, words, wi, wordPos, wordW, prevWasCode);
        }

        EmitWordGlyph(ctx, w, wordPos, wordW, font, isLink);

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

// Classify a code block's language from its fence tag. Pure (no ImGui).
// Fenced blocks like ```foo.py``` were previously classified
// Plain by FromTag (which expects bare language tags). Fall back to
// LangFromFilePath when FromTag yields Plain but the tag is non-empty, so
// path-style fence names get the right tokenizer.
static smatchet::code_color::CodeLang ClassifyCodeLang(const std::string& codeLang) {
    smatchet::code_color::CodeLang lang = smatchet::code_color::FromTag(codeLang);
    if (lang == smatchet::code_color::CodeLang::Plain && !codeLang.empty()) {
        const smatchet::code_color::CodeLang fromPath = smatchet::code_color::LangFromFilePath(codeLang);
        if (fromPath != smatchet::code_color::CodeLang::Plain) {
            lang = fromPath;
        }
    }
    return lang;
}

// Heading block — scale + tint + separator. ImGui-emitting.
static void RenderHeadingBlock(const PreviewPlan::Block& b, RenderState& r) {
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
}

// Record-only selection segments for an inline code block, one per source line,
// with hit-rects aligned to where DrawColoredCodeBlock painted each row. Kept at
// one segment per line rather than per glyph so the hot AI-history path stays
// within the Pillar 1 budget. RegisterSegment records rects only (emits no
// glyphs), so it overlays the coloured draw above. Each segment includes its
// trailing newline, when present, in the byte range so a multi-line drag-copy
// preserves line breaks instead of collapsing the block.
static void RegisterCodeBlockSegments(const PreviewPlan::Block& b, RenderState& r, const ImVec2& codeStart,
                                      ImFont* monoFont) {
    const char* const bufBegin = b.codeBuffer.c_str();
    const char* const bufEnd = bufBegin + b.codeBuffer.size();
    const char* lineBegin = bufBegin;
    int lineIdx = 0;
    // lineH is measured under the mono font (PushFont by the caller) so the hit-rect height
    // matches the glyphs' actual line advance, avoiding vertical drift when the mono
    // font's line height differs from the default.
    const float monoLineH = ImGui::GetTextLineHeightWithSpacing();
    while (lineBegin <= bufEnd) {
        const char* lineTextEnd = lineBegin;
        while (lineTextEnd < bufEnd && *lineTextEnd != '\n') {
            ++lineTextEnd;
        }
        // Segment byte range spans through the newline (if any) so copy keeps '\n'.
        const char* const segEnd = (lineTextEnd < bufEnd) ? lineTextEnd + 1 : lineTextEnd;
        const ImVec2 lineScreenPos(codeStart.x, codeStart.y + static_cast<float>(lineIdx) * monoLineH);
        const float lineWidth = ImGui::CalcTextSize(lineBegin, lineTextEnd).x;
        SelectableText::RegisterSegment(*r.selCtx, lineBegin, segEnd, lineScreenPos, monoLineH, monoFont, lineWidth,
                                        nullptr);
        if (lineTextEnd == bufEnd) {
            break;
        }
        lineBegin = lineTextEnd + 1;
        ++lineIdx;
    }
}

// Full (non-tooltip) inline code block: language badge + hover tooltip + Copy
// button + dark background + coloured glyphs + selection segments. ImGui-emitting.
static void RenderInlineCodeBlock(const PreviewPlan::Block& b, RenderState& r, const SmatchetPreviewFonts& fonts,
                                  smatchet::code_color::CodeLang lang) {
    ImGui::PushID(r.codeBlockNextId++);
    // Slice 4 of code-syntax-coloring-and-tooltips — ghosted
    // language badge at the row's left + hover tooltip surfacing the LD's
    // canonical name + detection origin. ImGui::TextDisabled to keep the
    // visual weight muted (the code itself owns the row). DelayNormal so
    // the tooltip doesn't fire while the user is scrolling past.
    const char* langLabel = smatchet::code_color::CanonicalName(lang);
    ImGui::TextDisabled("%s", langLabel);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(langLabel);
        if (b.codeLang.empty()) {
            ImGui::TextDisabled("(no language tag — rendering as plain text)");
        } else {
            ImGui::Text("(detected from markdown fence tag: `%s`)", b.codeLang.c_str());
        }
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    // Measure the Copy button width instead of hardcoding 60px.
    // Hardcoded width clipped the language badge when CanonicalName(lang) returned a longer
    // string (e.g. "TypeScript", "Objective-C"). Use the button's text-size + padding for
    // an exact reservation.
    // Clamp the right-align math so the cursor never
    // moves LEFT of where SameLine() put it. Without the clamp, narrow tooltips / parent
    // columns (availW < btnW) push the button on top of the language badge.
    const float availW = ImGui::GetContentRegionAvail().x;
    const float btnW =
        ImGui::CalcTextSize("Copy##codeblock", nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float curX = ImGui::GetCursorPosX();
    const float targetX = curX + availW - btnW;
    ImGui::SetCursorPosX(targetX > curX ? targetX : curX);
    if (ImGui::SmallButton("Copy##codeblock")) {
        ImGui::SetClipboardText(b.codeBuffer.c_str());
    }
    // B1 unified selection (code-syntax-coloring follow-up): render the
    // fenced block INLINE in the outer window — no nested BeginChild —
    // so it shares the prose SelectableText context and the outer
    // End()'s input routing makes code drag-selectable with zero special
    // handling. Trade-off (user-approved): the block loses its own
    // horizontal scrollbar + 240px height clamp; a very wide line now
    // extends into the outer page scroll.
    ImFont* const monoFont = fonts.Mono ? fonts.Mono : ImGui::GetFont();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const int lineCount = 1 + static_cast<int>(std::count(b.codeBuffer.begin(), b.codeBuffer.end(), '\n'));
    const ImVec2 codeStart = ImGui::GetCursorScreenPos();
    const float codeBgPad = 6.0f;
    const float codeBlockH = static_cast<float>(lineCount) * lineH;
    // Dark code-block background, drawn behind the glyphs (mirrors the
    // prose loop's inline-code dl->AddRectFilled). Replaces the removed
    // child's ImGuiCol_ChildBg. Span the full available width.
    ImDrawList* const codeDl = ImGui::GetWindowDrawList();
    const float codeAvailW = ImGui::GetContentRegionAvail().x;
    codeDl->AddRectFilled(ImVec2(codeStart.x - codeBgPad, codeStart.y - codeBgPad * 0.5f),
                          ImVec2(codeStart.x + codeAvailW, codeStart.y + codeBlockH + codeBgPad * 0.5f),
                          ImGui::GetColorU32(ImVec4(0.12f, 0.12f, 0.14f, 1.0f)), 3.0f);
    // Colored glyphs — DrawColoredCodeBlock owns its own cursor advance.
    // Keep the mono font pushed across the segment-registration loop so
    // line widths are measured under the same font the glyphs used.
    if (fonts.Mono)
        ImGui::PushFont(fonts.Mono);
    smatchet::code_color::DrawColoredCodeBlock(b.codeBuffer.c_str(), lang, b.codeLang);
    if (r.selCtx) {
        RegisterCodeBlockSegments(b, r, codeStart, monoFont);
    }
    if (fonts.Mono)
        ImGui::PopFont();
    ImGui::PopID();
}

// Code block dispatch — tooltip mode draws coloured glyphs only; full mode adds
// badge / Copy / selection chrome. ImGui-emitting.
static void RenderCodeBlock(const PreviewPlan::Block& b, RenderState& r) {
    const SmatchetPreviewFonts& fonts = SmatchetGetPreviewFonts();
    // Slice 2 of code-syntax-coloring-and-tooltips — route
    // every markdown code block through CodeColorView. C++/C delegates to
    // the existing DrawColoredCppText (zero behaviour change on Smatchet's
    // dominant path); Plain falls back to the same flat-orange tint the
    // old code used; every other language now gets per-language colouring.
    const smatchet::code_color::CodeLang lang = ClassifyCodeLang(b.codeLang);
    if (r.mode == MarkdownPreviewRender::Mode::Tooltip) {
        if (fonts.Mono)
            ImGui::PushFont(fonts.Mono);
        smatchet::code_color::DrawColoredCodeBlock(b.codeBuffer.c_str(), lang, b.codeLang);
        if (fonts.Mono)
            ImGui::PopFont();
    } else {
        RenderInlineCodeBlock(b, r, fonts, lang);
    }
    if (r.selCtx) {
        SelectableText::EndBlock(*r.selCtx);
    }
}

// Table block — borders/rowbg/stretch table, each cell its own uncached run set.
// ImGui-emitting.
static void RenderTableBlock(const PreviewPlan::Block& b, RenderState& r) {
    if (b.tableRows.empty() || b.tableColCount <= 0) {
        return;
    }
    ImGui::PushID(r.tableNextId++);
    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
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
    case BK::kHeading:
        RenderHeadingBlock(b, r);
        break;
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
    case BK::kCode:
        RenderCodeBlock(b, r);
        break;
    case BK::kTable:
        RenderTableBlock(b, r);
        break;
    }
}

} // namespace

namespace MarkdownPreviewRender {

void PreviewPlanDeleter::operator()(PreviewPlan* p) const noexcept { delete p; }

PreviewPlanPtr MakePlan() {
    return PreviewPlanPtr(new PreviewPlan(), // custom-deleter — make_unique inapplicable
                          PreviewPlanDeleter{});
}

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
