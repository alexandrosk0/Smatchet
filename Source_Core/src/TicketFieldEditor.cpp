#include "TicketFieldEditor.h"
#include "UiPerfMonitor.h"

#include "AppController.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerGridFieldDisplay.h"
#include "TrackerLabelsEditor.h"
#include "SmatchetFieldIconRender.h"
#include "SmatchetFieldRender.h"
#include "TrackerFieldValueUtils.h"
#include "TrackerFieldValueParser.h"
#include "TrackerFieldPayload.h"
#include "MarkdownConvert.h"
#include "Logger.h"
#include "JiraClient.h"
#include "CompactDateFormat.h"
#include "StringUtil.h"
#include "SmatchetImGuiFonts.h"

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
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <sstream>

namespace {

using namespace TrackerFieldValueUtils;

std::string GetCurrentJiraDateTimeString() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmLocal{};
    std::tm tmUtc{};
#if defined(_WIN32)
    localtime_s(&tmLocal, &tt);
    gmtime_s(&tmUtc, &tt);
#else
    localtime_r(&tt, &tmLocal);
    gmtime_r(&tt, &tmUtc);
#endif

    int localMin = tmLocal.tm_hour * 60 + tmLocal.tm_min;
    int utcMin = tmUtc.tm_hour * 60 + tmUtc.tm_min;

    if (tmLocal.tm_yday > tmUtc.tm_yday || (tmLocal.tm_yday == 0 && tmUtc.tm_yday > 300)) {
        localMin += 24 * 60;
    } else if (tmLocal.tm_yday < tmUtc.tm_yday || (tmUtc.tm_yday == 0 && tmLocal.tm_yday > 300)) {
        utcMin += 24 * 60;
    }
    const int offsetSec = (localMin - utcMin) * 60;

    ParsedJiraDateTime p;
    p.Year = tmLocal.tm_year + 1900;
    p.Month = tmLocal.tm_mon + 1;
    p.Day = tmLocal.tm_mday;
    p.Hour = tmLocal.tm_hour;
    p.Minute = tmLocal.tm_min;
    p.Second = tmLocal.tm_sec;
    p.HasWallTime = true;
    p.OffsetSec = offsetSec;
    p.HasTimeZoneSuffix = true;
    p.TimeZoneWasZ = (offsetSec == 0);

    return FormatJiraDateOrDateTimeForApi(false, p);
}

struct ActiveWorklogDialogState {
    std::string IssueId;
    char TimeSpent[64] = "";
    char TimeRemaining[64] = "";
    std::string DateStarted;
    char WorkDescription[1024] = "";
    std::string OriginalEstimate;
    std::string TotalTimeSpent;
    std::string TotalTimeRemaining;
    bool Initialized = false;
    std::string ErrorMsg;
    bool JustOpened = false;
    bool TimeRemainingManuallyEdited = false;
};

static ActiveWorklogDialogState s_ActiveWorklogState;

/// Source format of the original rich payload (`OriginalRichValue`). Determines which converter
/// seeds the Markdown buffer on open and which target format is expected by the payload layer.
enum class LongTextRichKind { None, Adf, Html };

/// Singleton state for the long-text / ADF field modal editor. Decoupled from `SpreadsheetState`
/// so the modal survives the originating cell scrolling out of view: the cell triggers
/// `JustOpened`, then the top-level `RenderLongTextModal` owns the lifecycle.
struct ActiveLongTextEditorState {
    std::string IssueId;
    TrackerField Field;
    std::string FieldLabel;
    /// Stripped display text of the field at the moment the modal opened — for "did the user actually
    /// change anything?" detection on save.
    std::string OriginalStrippedValue;
    /// The Markdown that initially seeded the buffer (after converting from `OriginalRichValue`).
    /// Used so save can detect "no change" against the actual editor surface, not the stripped text.
    std::string OriginalMarkdown;
    /// Original rich payload (ADF JSON or HTML) at modal-open time. Empty when the cache had no
    /// rich value (legacy ticket pre-PR-B). v2 PR-E will pass this to the offline-replay merge.
    std::string OriginalRichValue;
    LongTextRichKind RichKind = LongTextRichKind::None;
    /// True when HtmlSubsetToMarkdown tripped the fallback (unknown tags) — modal shows a banner
    /// and falls back to editing the raw HTML directly.
    bool RawMode = false;
    /// ADF node types that AdfToMarkdown skipped because they aren't representable in our
    /// Markdown subset (panels, mentions, smart links, ...). Surface as a soft warning.
    std::vector<std::string> DroppedAdfNodeTypes;

    /// Generously sized for descriptions; truncates beyond this. v2.1 candidate: resize callback.
    static constexpr size_t kBufferSize = 64 * 1024;
    std::vector<char> Buffer;
    bool Active = false;
    bool JustOpened = false;
    /// Three-way preview mode. EditOnly hides the rendered pane; Split shows editor + preview
    /// with a draggable splitter; PreviewOnly hides the editor (read-only render).
    /// Ctrl+P cycles Edit -> Split -> Preview -> Edit.
    enum class PreviewModeT { EditOnly, Split, PreviewOnly };
    PreviewModeT PreviewMode = PreviewModeT::EditOnly;
    /// Editor's fraction of the horizontal pane area in Split mode. Clamped to [0.15, 0.85]
    /// while dragging the splitter so neither pane collapses.
    float SplitterRatio = 0.5f;
    /// Last frame's scroll positions for editor / preview child windows. Used to detect which
    /// pane the user scrolled this frame and mirror the ratio to the other in Split mode.
    float LastEditorScrollY = 0.0f;
    float LastPreviewScrollY = 0.0f;
    /// PR-5 round-trip preview cache. The preview pane renders the result of
    /// Markdown -> ADF/HTML -> Markdown so any conversion loss surfaces before
    /// save. Recomputed lazily when the buffer changes (direct string compare;
    /// 64 KB buffers compare in tens of microseconds — fine inline).
    std::string LastRoundTripInput;
    std::string LastRoundTripOutput;
    /// True when the last round-trip dropped ADF nodes (Adf path) or tripped the
    /// HtmlSubsetToMarkdown fallback (Html path) — drives the "(lossy)" caption.
    bool LastRoundTripLossy = false;
    /// Debounce: set when the buffer diverges from LastRoundTripInput; conversion fires once
    /// RoundTripFireAt is reached so rapid keystrokes don't re-convert on every frame.
    bool RoundTripPending = false;
    std::chrono::steady_clock::time_point RoundTripFireAt{};
};

static ActiveLongTextEditorState s_ActiveLongTextState;

static constexpr const char* kLongTextModalPopupId = "EditLongTextModal";

/// Determine whether a stored rich payload looks like ADF JSON or HTML. Returns LongTextRichKind::None
/// when the input is empty or unrecognizable (caller falls back to the stripped text).
static LongTextRichKind ClassifyRichValue(const std::string& rich) {
    if (rich.empty()) return LongTextRichKind::None;
    // Cheap leading-whitespace skip.
    size_t i = 0;
    while (i < rich.size() && (rich[i] == ' ' || rich[i] == '\t' || rich[i] == '\n' || rich[i] == '\r')) ++i;
    if (i >= rich.size()) return LongTextRichKind::None;
    if (rich[i] == '{') {
        try {
            auto parsed = nlohmann::json::parse(rich, nullptr, false);
            if (parsed.is_object() && parsed.value("type", std::string()) == "doc") {
                return LongTextRichKind::Adf;
            }
        } catch (...) {
            // fall through
        }
    }
    if (rich[i] == '<') {
        return LongTextRichKind::Html;
    }
    return LongTextRichKind::None;
}

static void OpenLongTextEditor(const std::string& issueId, const TrackerField& field,
                               const std::string& label, const std::string& currentStrippedValue,
                               const std::string& currentRichValue) {
    s_ActiveLongTextState.IssueId = issueId;
    s_ActiveLongTextState.Field = field;
    s_ActiveLongTextState.FieldLabel = label.empty() ? field.Id : label;
    s_ActiveLongTextState.OriginalStrippedValue = currentStrippedValue;
    s_ActiveLongTextState.OriginalRichValue = currentRichValue;
    s_ActiveLongTextState.RichKind = ClassifyRichValue(currentRichValue);
    s_ActiveLongTextState.RawMode = false;
    s_ActiveLongTextState.DroppedAdfNodeTypes.clear();

    /// Compute the Markdown seed from the rich value when present; otherwise fall back to the
    /// stripped text (legacy tickets pre-PR-B, or fields where the backend never returned rich).
    std::string seed;
    switch (s_ActiveLongTextState.RichKind) {
        case LongTextRichKind::Adf: {
            try {
                const auto adf = nlohmann::json::parse(currentRichValue);
                seed = MarkdownConvert::AdfToMarkdown(adf, &s_ActiveLongTextState.DroppedAdfNodeTypes);
            } catch (...) {
                seed = currentStrippedValue;
            }
            break;
        }
        case LongTextRichKind::Html: {
            bool fellBack = false;
            seed = MarkdownConvert::HtmlSubsetToMarkdown(currentRichValue, &fellBack);
            if (fellBack) {
                s_ActiveLongTextState.RawMode = true;
                seed = currentRichValue; // edit the raw HTML directly so nothing is lost.
            }
            break;
        }
        case LongTextRichKind::None:
            seed = currentStrippedValue;
            break;
    }
    s_ActiveLongTextState.OriginalMarkdown = seed;

    s_ActiveLongTextState.Buffer.assign(ActiveLongTextEditorState::kBufferSize, '\0');
    const size_t copyLen = (std::min)(seed.size(), ActiveLongTextEditorState::kBufferSize - 1);
    std::memcpy(s_ActiveLongTextState.Buffer.data(), seed.data(), copyLen);
    s_ActiveLongTextState.Buffer[copyLen] = '\0';
    s_ActiveLongTextState.Active = true;
    s_ActiveLongTextState.JustOpened = true;
}

static void CloseLongTextEditor() {
    s_ActiveLongTextState = ActiveLongTextEditorState{};
}

// =====================================================================================
// Markdown preview — walks a Markdown string with md4c and emits ImGui draw calls so the
// modal can show the rendered output next to the editor. Block-level only; inline marks
// (bold/em/code/links) collapse to their text content. v2 tradeoff: rendering inline
// styles inside a paragraph requires per-glyph push/pop fonts which we don't have.
// =====================================================================================
namespace {

// Inline mark bits — combined per-run via OR. Bold + Italic together select the
// BoldItalic font; Code overrides body font with Mono. Link/Strike are visual
// overlays drawn after the text.
constexpr uint8_t MARK_BOLD   = 1 << 0;
constexpr uint8_t MARK_ITALIC = 1 << 1;
constexpr uint8_t MARK_CODE   = 1 << 2;
constexpr uint8_t MARK_STRIKE = 1 << 3;
constexpr uint8_t MARK_LINK   = 1 << 4;

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
};

/// Append `text` of length `size` as either an extension of the trailing run
/// (when style matches) or a fresh styled run. Coalescing keeps `runs` short.
/// Routes to `activeRuns` when set (table-cell capture) else `runs`.
static void PreviewAppendRunBytes(PreviewState& s, const char* text, size_t size) {
    if (size == 0) return;
    std::vector<StyledRun>& target = s.activeRuns ? *s.activeRuns : s.runs;
    if (!target.empty() && target.back().marks == s.currentMarks &&
        target.back().href == s.currentHref) {
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
    if (marks & MARK_CODE) return f.Mono ? f.Mono : f.Regular;
    const bool bold = (marks & MARK_BOLD) != 0;
    const bool italic = (marks & MARK_ITALIC) != 0;
    if (bold && italic) return f.BoldItalic ? f.BoldItalic : (f.Bold ? f.Bold : f.Regular);
    if (bold)           return f.Bold ? f.Bold : f.Regular;
    if (italic)         return f.Italic ? f.Italic : f.Regular;
    return f.Regular;
}

/// Walks `runs` word by word and emits ImGui text segments with manual word
/// wrap against `wrapWidth`. Whitespace between words is collapsed to a single
/// space (matches HTML/Markdown rendering rules); '\n' inside a run forces a
/// hard line break. Links are rendered with an underline overlay and clickable
/// hit-rect; strike-through is a half-height line over the word.
static void PreviewRenderRuns(const std::vector<StyledRun>& runs) {
    if (runs.empty()) return;

    struct InlineWord {
        std::string text;          // empty when forceBreak=true
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
                    if (d == ' ' || d == '\t' || d == '\n' || d == '\r') break;
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

    const float wrapWidth = ImGui::GetContentRegionAvail().x;
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
        const float wordW = ImGui::CalcTextSize(w.text.c_str(),
                                                w.text.c_str() + w.text.size()).x;
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
                dl->AddRectFilled(ImVec2(spacePos.x, spacePos.y),
                                  ImVec2(spacePos.x + spaceW, spacePos.y + lineH),
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
            const bool nextIsCode = (wi + 1 < words.size())
                                        && !words[wi + 1].forceBreak
                                        && (words[wi + 1].marks & MARK_CODE) != 0;
            const float rl = prevWasCode ? 0.0f : codeBgRound;
            const float rr = nextIsCode  ? 0.0f : codeBgRound;
            const ImVec2 r0(wordPos.x - (prevWasCode ? 0.0f : codeBgPadX), wordPos.y);
            const ImVec2 r1(wordPos.x + wordW + (nextIsCode ? 0.0f : codeBgPadX),
                            wordPos.y + lineH);
            const ImDrawFlags flags = (rl > 0.0f ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersNone)
                                    | (rr > 0.0f ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
            dl->AddRectFilled(r0, r1, codeBgCol, std::max(rl, rr), flags);
        }
        ImGui::TextUnformatted(w.text.c_str(), w.text.c_str() + w.text.size());
        if (isLink) {
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            dl->AddLine(ImVec2(wordPos.x, wordPos.y + lineH - 1.0f),
                        ImVec2(wordPos.x + wordW, wordPos.y + lineH - 1.0f), col);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!w.href.empty()) ImGui::SetTooltip("%s", w.href.c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !w.href.empty()) {
#if defined(_WIN32)
                    ShellExecuteA(nullptr, "open", w.href.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
                }
            }
        }
        if (w.marks & MARK_STRIKE) {
            const float midY = wordPos.y + lineH * 0.5f;
            dl->AddLine(ImVec2(wordPos.x, midY), ImVec2(wordPos.x + wordW, midY),
                        ImGui::GetColorU32(ImGuiCol_Text));
        }
        ImGui::PopFont();

        curX += wordW;
        firstOnLine = false;
        prevWasCode = isCode;
    }
}

static void PreviewFlushBlock(PreviewState& s) {
    if (s.runs.empty() && s.headingLevel == 0) return;
    if (s.headingLevel > 0) {
        // Heading sizing: scale the bitmap font for h1–h3 since we don't bake
        // separate atlas sizes (Phase 4 budget). Bitmap rescale is mildly blurry
        // at non-1.0 ratios but cheap; users perceive size differentiation.
        // Tint only h1/h2 in cyan — at h3+ the size + bold weight already make
        // the heading stand out, so the color is doing redundant work.
        float scale = 1.0f;
        if (s.headingLevel == 1) scale = 1.6f;
        else if (s.headingLevel == 2) scale = 1.35f;
        else if (s.headingLevel == 3) scale = 1.15f;

        // Force bold on every run inside the heading. Inline em/code/links keep
        // their italic/mono/underline overlays via the other mark bits.
        for (StyledRun& r : s.runs) r.marks |= MARK_BOLD;

        const bool tinted = (s.headingLevel <= 2);
        const float prevScale = ImGui::GetCurrentWindow()->FontWindowScale;
        if (scale != 1.0f) ImGui::SetWindowFontScale(scale);
        if (tinted) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.95f, 1.0f, 1.0f));
        PreviewRenderRuns(s.runs);
        if (tinted) ImGui::PopStyleColor();
        if (scale != 1.0f) ImGui::SetWindowFontScale(prevScale);
        if (s.headingLevel <= 2) ImGui::Separator();
    } else if (!s.runs.empty()) {
        PreviewRenderRuns(s.runs);
    }
    s.runs.clear();
    s.headingLevel = 0;
}

static int PreviewEnterBlock(MD_BLOCKTYPE type, void* detail, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    // Inside a nested table (rare, non-standard) we ignore everything until the
    // outer table closes — only the outermost table is rendered.
    if (s.tableDepth > 1) {
        if (type == MD_BLOCK_TABLE) ++s.tableDepth;
        return 0;
    }
    switch (type) {
        case MD_BLOCK_DOC: break;
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
            for (int i = 0; i < s.listDepth - 1; ++i) ImGui::Indent(16.0f);
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
            if (s.tableDepth == 1) s.tableInHeader = true;
            break;
        case MD_BLOCK_TBODY:
            if (s.tableDepth == 1) s.tableInHeader = false;
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
            break;
        case MD_BLOCK_P:
            // Inside a list item the buffer is already primed with the marker; don't flush.
            if (s.listDepth == 0) PreviewFlushBlock(s);
            break;
        default: break;
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
            const ImGuiTableFlags flags = ImGuiTableFlags_Borders
                                        | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_SizingStretchProp
                                        | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("##mdpreview_tbl", s.tableColCount, flags)) {
                for (TableRowData& row : s.tableRows) {
                    ImGui::TableNextRow();
                    const size_t cellMax = (std::min)(row.cells.size(),
                                                      static_cast<size_t>(s.tableColCount));
                    for (size_t ci = 0; ci < cellMax; ++ci) {
                        ImGui::TableNextColumn();
                        TableCellData& cell = row.cells[ci];
                        if (row.isHeader) {
                            for (StyledRun& r : cell.runs) r.marks |= MARK_BOLD;
                        }
                        PreviewRenderRuns(cell.runs);
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
    if (s.tableDepth > 1) return 0;
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
        case MD_BLOCK_DOC: PreviewFlushBlock(s); break;
        case MD_BLOCK_QUOTE:
            PreviewFlushBlock(s);
            ImGui::PopStyleColor();
            ImGui::Unindent(16.0f);
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            PreviewFlushBlock(s);
            if (!s.listStack.empty()) s.listStack.pop_back();
            if (!s.orderedCounters.empty()) s.orderedCounters.pop_back();
            if (s.listDepth > 0) --s.listDepth;
            break;
        case MD_BLOCK_LI:
            PreviewFlushBlock(s);
            for (int i = 0; i < s.listDepth - 1; ++i) ImGui::Unindent(16.0f);
            break;
        case MD_BLOCK_CODE: {
            // Render the accumulated code-block text in a child with monospace styling.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
            const float h = ImGui::GetTextLineHeightWithSpacing() *
                            static_cast<float>(1 + std::count(s.codeBuffer.begin(), s.codeBuffer.end(), '\n'));
            ImGui::BeginChild("##mdpreview_code", ImVec2(-FLT_MIN, std::min(h + 12.0f, 240.0f)),
                              true, ImGuiWindowFlags_HorizontalScrollbar);
            const SmatchetPreviewFonts& fonts = SmatchetGetPreviewFonts();
            if (fonts.Mono) ImGui::PushFont(fonts.Mono);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.6f, 1.0f));
            ImGui::TextUnformatted(s.codeBuffer.c_str());
            ImGui::PopStyleColor();
            if (fonts.Mono) ImGui::PopFont();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            s.codeBuffer.clear();
            if (s.codeDepth > 0) --s.codeDepth;
            break;
        }
        case MD_BLOCK_H:
        case MD_BLOCK_P:
            PreviewFlushBlock(s);
            break;
        default: break;
    }
    return 0;
}

static int PreviewEnterSpan(MD_SPANTYPE type, void* detail, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    if (s.tableDepth > 1) return 0;
    switch (type) {
        case MD_SPAN_STRONG: s.currentMarks |= MARK_BOLD; break;
        case MD_SPAN_EM:     s.currentMarks |= MARK_ITALIC; break;
        case MD_SPAN_CODE:   s.currentMarks |= MARK_CODE; break;
        case MD_SPAN_DEL:    s.currentMarks |= MARK_STRIKE; break;
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
        default: break;
    }
    return 0;
}

static int PreviewLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    if (s.tableDepth > 1) return 0;
    switch (type) {
        case MD_SPAN_STRONG: s.currentMarks &= ~MARK_BOLD; break;
        case MD_SPAN_EM:     s.currentMarks &= ~MARK_ITALIC; break;
        case MD_SPAN_CODE:   s.currentMarks &= ~MARK_CODE; break;
        case MD_SPAN_DEL:    s.currentMarks &= ~MARK_STRIKE; break;
        case MD_SPAN_A:
            s.currentMarks &= ~MARK_LINK;
            s.currentHref.clear();
            break;
        case MD_SPAN_IMG:
            if (s.imgSpanDepth > 0) --s.imgSpanDepth;
            PreviewAppendRunString(s, s.imgAltAccum.empty() ? std::string("[image]") : s.imgAltAccum);
            s.imgAltAccum.clear();
            break;
        default: break;
    }
    return 0;
}

static int PreviewText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud) {
    auto& s = *static_cast<PreviewState*>(ud);
    if (s.tableDepth > 1) return 0;
    if (type == MD_TEXT_NULLCHAR) return 0;
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

static void RenderMarkdownPreview(const std::string& md) {
    PreviewState state;
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MarkdownConvert::Md4cParserFlags();
    parser.enter_block = PreviewEnterBlock;
    parser.leave_block = PreviewLeaveBlock;
    parser.enter_span  = PreviewEnterSpan;
    parser.leave_span  = PreviewLeaveSpan;
    parser.text        = PreviewText;
    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &state);
    PreviewFlushBlock(state);
}

} // namespace

struct EditCbUser {
    SpreadsheetState* state;
};

/** Collapse ImGui's initial select-all when a grid text cell opens. `EventActivated` can arrive the frame after
 *  `EditJustStarted` is cleared; use `PendingGridInputTextDeselect` until we see activation or full-range selection. */
static int InputTextCallback_ClearSelectOnEditOpen(ImGuiInputTextCallbackData* data) {
    auto* u = static_cast<EditCbUser*>(data->UserData);
    if (!u || !u->state) {
        return 0;
    }
    SpreadsheetState* st = u->state;
    if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways) {
        return 0;
    }
    if (!st->PendingGridInputTextDeselect) {
        return 0;
    }
    const bool fullRange =
        data->BufTextLen > 0 && data->SelectionStart == 0 && data->SelectionEnd == data->BufTextLen;
    if (!data->EventActivated && !fullRange) {
        return 0;
    }
    const int end = data->BufTextLen;
    data->SetSelection(end, end);
    st->PendingGridInputTextDeselect = false;
    return 0;
}

struct DurationCallbackWrapperData {
    ImGuiInputTextCallback OriginalCallback;
    void* OriginalUserData;
    bool* NeedRepositionAndFocus;
};

static int DurationInputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* wrapper = static_cast<DurationCallbackWrapperData*>(data->UserData);
    if (!wrapper) return 0;

    if (wrapper->NeedRepositionAndFocus && *(wrapper->NeedRepositionAndFocus)) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
            data->CursorPos = data->BufTextLen;
            data->SelectionStart = data->BufTextLen;
            data->SelectionEnd = data->BufTextLen;
            *(wrapper->NeedRepositionAndFocus) = false;
        }
    }

    if (wrapper->OriginalCallback) {
        data->UserData = wrapper->OriginalUserData;
        int res = wrapper->OriginalCallback(data);
        data->UserData = wrapper;
        return res;
    }
    return 0;
}

bool DrawDurationFieldWithSuggestions(const char* label, char* buf, size_t bufSize,
                                      ImGuiInputTextFlags flags = 0,
                                      ImGuiInputTextCallback callback = nullptr,
                                      void* callbackUserData = nullptr,
                                      bool* outManuallyEdited = nullptr,
                                      bool forceOpenPopup = false) {
    bool submitted = false;
    ImGui::PushID(label);

    // Resolve per-widget state using ImGuiStorage
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID needFocusKey = ImGui::GetID("##needRepositionAndFocus");
    ImGuiID lastActiveIdKey = ImGui::GetID("##lastActiveId");

    bool needRepositionAndFocus = storage->GetInt(needFocusKey, 0) != 0;
    ImGuiID lastActiveId = static_cast<ImGuiID>(storage->GetInt(lastActiveIdKey, 0));
    ImGuiID selectedFromPopupKey = ImGui::GetID("##valueSelectedFromPopup");
    bool valueSelectedFromPopup = storage->GetInt(selectedFromPopupKey, 0) != 0;

    float totalWidth = ImGui::GetContentRegionAvail().x;
    float inputWidth = totalWidth - 26.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    ImGui::SetNextItemWidth(inputWidth);

    if (forceOpenPopup) {
        ImGui::OpenPopup("duration_suggestions");
    }

    if (needRepositionAndFocus) {
        ImGui::SetKeyboardFocusHere();
    }

    DurationCallbackWrapperData wrapperData;
    wrapperData.OriginalCallback = callback;
    wrapperData.OriginalUserData = callbackUserData;
    wrapperData.NeedRepositionAndFocus = &needRepositionAndFocus;

    bool deactivated = false;
    if (ImGui::InputText("##duration_input", buf, bufSize, flags | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways, DurationInputTextCallback, &wrapperData)) {
        submitted = true;
        if (outManuallyEdited) {
            *outManuallyEdited = true;
        }
    }
    deactivated = ImGui::IsItemDeactivatedAfterEdit();
    if (!deactivated && ImGui::IsItemDeactivated() && valueSelectedFromPopup) {
        deactivated = true;
        valueSelectedFromPopup = false;
    }

    ImGuiID currentId = ImGui::GetID("##duration_input");

    bool shouldOpen = false;
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        shouldOpen = true;
    }
    if (ImGui::IsItemActive()) {
        if (lastActiveId != currentId) {
            lastActiveId = currentId;
            if (buf[0] == '\0') {
                shouldOpen = true;
            }
        }
    } else {
        if (lastActiveId == currentId) {
            lastActiveId = 0;
        }
    }
    if (shouldOpen) {
        ImGui::OpenPopup("duration_suggestions");
    }

    ImGui::SameLine();
    bool buttonClicked = ImGui::Button("▼", ImVec2(24.0f, 0.0f));
    bool buttonHovered = ImGui::IsItemHovered();
    if (buttonClicked) {
        ImGui::OpenPopup("duration_suggestions");
    }
    ImGui::PopStyleVar();

    bool popupIsOpen = ImGui::IsPopupOpen("duration_suggestions");

    // Suggestions popup
    if (ImGui::BeginPopup("duration_suggestions")) {
        std::vector<std::string> suggestions = LoadDurationSuggestions();

        for (size_t i = 0; i < suggestions.size(); ++i) {
            const auto& item = suggestions[i];
            if (ImGui::Selectable(item.c_str())) {
                std::strncpy(buf, item.c_str(), bufSize - 1);
                buf[bufSize - 1] = '\0';
                if (outManuallyEdited) {
                    *outManuallyEdited = true;
                }
                needRepositionAndFocus = true;
                valueSelectedFromPopup = true;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }

    // Resolve per-widget state using ImGuiStorage
    ImGuiID popupOpenKey = ImGui::GetID("##popupWasOpen");
    bool popupWasOpen = storage->GetInt(popupOpenKey, 0) != 0;

    bool popupJustClosed = popupWasOpen && !popupIsOpen;

    bool finalDeactivated = false;
    if (deactivated) {
        if (!buttonHovered && !popupIsOpen) {
            finalDeactivated = true;
        }
    }
    if (popupJustClosed && !needRepositionAndFocus) {
        finalDeactivated = true;
    }

    storage->SetInt(popupOpenKey, popupIsOpen ? 1 : 0);
    storage->SetInt(selectedFromPopupKey, valueSelectedFromPopup ? 1 : 0);
    // Store updated state back to ImGuiStorage
    storage->SetInt(needFocusKey, needRepositionAndFocus ? 1 : 0);
    storage->SetInt(lastActiveIdKey, static_cast<int>(lastActiveId));

    ImGui::PopID();
    return submitted || finalDeactivated;
}

void QueueEdit(const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values,
               std::vector<PendingFieldEdit>& pendingEdits) {
    PendingFieldEdit edit;
    edit.IssueId = issueId;
    edit.Field = field;
    edit.Values = values;
    pendingEdits.push_back(std::move(edit));
}

std::string EncodeCascadingSelection(const std::string& parentId, const std::string& childId) {
    return parentId + "\x1f" + childId;
}

void RenderTextEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                      const std::string& currentValue,
                      SpreadsheetState& state, std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled,
                      float availWidth) {
    // Widget-cell unique ID is provided by the CellIdScope pushed in RenderFieldCell
    // (ticket.id + field.Id on the ImGui ID stack). Literal short labels below stay collision-free.

    // ADF / long-text fields (Jira description/environment, Plane description, custom textarea/wiki-renderer
    // fields) are too constrained by the inline 512-byte single-line InputText. Route them through the modal
    // editor singleton: the cell stays in display mode, the modal owns the lifecycle. v1: plain-text round-trip.
    // v2 (see RICH_TEXT_EDITING_V2_PLAN.md) layers Markdown <-> ADF/HTML fidelity on top.
    if (TrackerFieldPayload::FieldUsesAdfDocument(field)) {
        if (state.IsEditingField(ticket.id, field.Id)) {
            if (state.EditJustStarted) {
                const std::string& label = !field.Name.empty() ? field.Name : field.Id;
                const std::string richValue = ticket.GetFieldRichValue(field.Id);
                OpenLongTextEditor(ticket.id, field, label, currentValue, richValue);
                // Do NOT call ImGui::OpenPopup here — we are inside a table cell.
                // ImGui requires OpenPopup and BeginPopupModal at the same window depth.
                // RenderLongTextModal (called after EndTable) sees JustOpened=true and
                // calls OpenPopup from the stable top-level location.
            }
            // Hand the lifecycle to the modal singleton; the cell falls through to the read-only preview below.
            state.ClearEditing();
        }
        // fall through to the display branch
    } else if (state.IsEditingField(ticket.id, field.Id)) {
        const bool editJustStarted = state.EditJustStarted;
        const bool isDuration = IsTimeDurationField(field.Id);
        bool submitted = false;

        if (isDuration) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (editJustStarted) {
                ImGui::SetKeyboardFocusHere();
            }
            EditCbUser cbUser{&state};
            submitted = DrawDurationFieldWithSuggestions(
                "##textedit_duration", state.EditBuffer, sizeof(state.EditBuffer),
                ImGuiInputTextFlags_CallbackAlways,
                InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&cbUser),
                nullptr, editJustStarted
            );
        } else {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (editJustStarted) {
                ImGui::SetKeyboardFocusHere();
            }
            EditCbUser cbUser{&state};
            submitted = ImGui::InputText("##textedit", state.EditBuffer, sizeof(state.EditBuffer),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                         InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&cbUser));
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            state.ClearEditing();
        } else if (submitted || (!editJustStarted && ImGui::IsItemDeactivatedAfterEdit())) {
            QueueEdit(ticket.id, field, {state.EditBuffer}, pendingEdits);
            state.ClearEditing();
        } else if (editJustStarted) {
            state.EditJustStarted = false;
        }
        return;
    }

    const std::string valueForDisplay = app.ResolveDisplayValue(field.Id, &field, currentValue);
    const bool hasNewlineInValue = std::any_of(valueForDisplay.begin(), valueForDisplay.end(),
                                               [](char c) { return c == '\n' || c == '\r'; });
    std::string singleLine = valueForDisplay;
    auto nlIt = std::find_if(singleLine.begin(), singleLine.end(), [](char c) { return c == '\n' || c == '\r'; });
    if (nlIt != singleLine.end()) {
        singleLine.erase(nlIt, singleLine.end());
    }
    const std::string& display = singleLine;
    const float regionAvail = (availWidth > 0.0f) ? availWidth : ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable(display.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(0)) {
            state.StartEditingField(ticket.id, field.Id, currentValue);
        }
    }
    const ImVec2 textSize = ImGui::CalcTextSize(display.c_str());
    const bool horizontallyClipped = (regionAvail > 0.0f && textSize.x > regionAvail + 1.0f);
    if (tooltipsEnabled && (hasNewlineInValue || horizontallyClipped) && ImGui::IsItemHovered()) {
        const std::string* rawTip = IsTrackerDateOrDateTimeField(field.Id, &field) ? &currentValue : nullptr;
        const std::string& tipSource = (rawTip && !rawTip->empty()) ? *rawTip : valueForDisplay;
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
        ImGui::TextUnformatted(tipSource.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void RenderSingleSelectEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                              const std::string& currentValue, std::vector<PendingFieldEdit>& pendingEdits,
                              bool tooltipsEnabled) {
    const float cellAvail = ImGui::GetContentRegionAvail().x;
    SmatchetLoadedIconTexture overlayIcon{};
    std::string overlayLoadErr;
    const bool haveOverlayIcon =
        SmatchetFieldIconRender::TryGetInlineFieldIconTexture(app, field, currentValue, overlayIcon, overlayLoadErr);
    (void)overlayLoadErr;

    const std::string currentId = ResolveOptionId(field, currentValue);
    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
    // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    const char* previewCStr =
        haveOverlayIcon ? " " : (preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str());
    ImGui::SetNextItemWidth(cellAvail);
    const bool comboOpened = ImGui::BeginCombo("##singleselect", previewCStr, ImGuiComboFlags_NoArrowButton);
    const ImVec2 comboMin = ImGui::GetItemRectMin();
    const ImVec2 comboMax = ImGui::GetItemRectMax();
    if (comboOpened) {
        const bool selectedNone = currentId.empty();
        if (ImGui::Selectable("<clear>", selectedNone)) {
            QueueEdit(ticket.id, field, {}, pendingEdits);
        }

        for (const auto& option : field.AllowedValueOptions) {
            const bool isSelected = (option.Id == currentId);
            if (ImGui::Selectable(option.Value.c_str(), isSelected)) {
                QueueEdit(ticket.id, field, {option.Id}, pendingEdits);
            }
        }
        ImGui::EndCombo();
    }

    if (haveOverlayIcon && overlayIcon.Texture != nullptr && overlayIcon.Width > 0 && overlayIcon.Height > 0) {
        const float maxEdge = ImGui::GetFrameHeight();
        const float iw = static_cast<float>(overlayIcon.Width);
        const float ih = static_cast<float>(overlayIcon.Height);
        const float scale = maxEdge / (std::max)(iw, ih);
        const float dw = iw * scale;
        const float dh = ih * scale;
        const float rowH = comboMax.y - comboMin.y;
        const ImVec2 overlayP0(comboMin.x + 4.0f, comboMin.y + ((rowH - dh) * 0.5f));
        const ImVec2 overlayP1(overlayP0.x + dw, overlayP0.y + dh);
        ImGui::GetWindowDrawList()->AddImage(overlayIcon.Texture->GetTexRef(), overlayP0, overlayP1, ImVec2(0.0f, 0.0f),
                                             ImVec2(1.0f, 1.0f));
    }
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        const ImVec2 psz = ImGui::CalcTextSize(previewCStr);
        const bool previewClipped = (comboAvailBefore > 0.0f && psz.x > comboAvailBefore + 1.0f);
        if (previewClipped) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            ImGui::TextUnformatted(previewCStr);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

void RenderMultiSelectEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                             const std::string& currentValue, SpreadsheetState& state,
                             std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled) {
    std::vector<std::string> selectedIds = ResolveCurrentSelectionIds(field, currentValue);
    std::unordered_set<std::string> selectedSet(selectedIds.begin(), selectedIds.end());
    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
    // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##multiselect", preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
        const std::string editorKey = ticket.id + "::" + field.Id;
        if (state.MultiSelectActiveKey != editorKey) {
            state.MultiSelectActiveKey = editorKey;
            state.MultiSelectSearchBuf[0] = '\0';
        }

        if (ImGui::Selectable("<clear all>", selectedSet.empty())) {
            QueueEdit(ticket.id, field, {}, pendingEdits);
        }
        ImGui::Separator();

        const std::string searchHint = field.Id == "components" ? "Search components" : "Search options";
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##MultiSelectSearch", searchHint.c_str(), state.MultiSelectSearchBuf, sizeof(state.MultiSelectSearchBuf));
        ImGui::Separator();

        const std::string filterLower = ToLowerAsciiCopy(TrimCopy(state.MultiSelectSearchBuf));
        bool drewAny = false;
        for (const auto& option : field.AllowedValueOptions) {
            const std::string optionId = option.Id.empty() ? option.Value : option.Id;
            bool checked = (selectedSet.find(optionId) != selectedSet.end());
            const bool matchesFilter =
                filterLower.empty() ||
                ToLowerAsciiCopy(option.Value).find(filterLower) != std::string::npos ||
                ToLowerAsciiCopy(option.SecondaryValue).find(filterLower) != std::string::npos ||
                ToLowerAsciiCopy(optionId).find(filterLower) != std::string::npos;
            if (!checked && !matchesFilter) {
                continue;
            }
            drewAny = true;
            // Per-option PushID disambiguates checkboxes whose visible labels could collide
            // (e.g. duplicate option.Value across customfields); pop matches at the end of body.
            ImGui::PushID(optionId.c_str());
            if (option.Disabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Checkbox(option.Value.c_str(), &checked)) {
                if (checked) {
                    selectedSet.insert(optionId);
                } else {
                    selectedSet.erase(optionId);
                }
                std::vector<std::string> updated(selectedSet.begin(), selectedSet.end());
                std::sort(updated.begin(), updated.end());
                QueueEdit(ticket.id, field, updated, pendingEdits);
            }
            if (option.Disabled) {
                ImGui::EndDisabled();
            }
            if (!option.SecondaryValue.empty() && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                ImGui::TextUnformatted(option.SecondaryValue.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        if (!drewAny) {
            ImGui::TextDisabled(filterLower.empty() ? "(no options)" : "(no matching options)");
        }
        ImGui::EndCombo();
    }
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        const ImVec2 psz = ImGui::CalcTextSize(preview.c_str());
        const bool previewClipped = (comboAvailBefore > 0.0f && psz.x > comboAvailBefore + 1.0f);
        if (previewClipped) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            ImGui::TextUnformatted(preview.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

void RenderCascadingSelectEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                                 const std::string& currentValue,
                                 std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled) {
    std::string parentId;
    std::string childId;
    TryResolveCascadingSelection(field, currentValue, parentId, childId);
    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);

    // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##cascadeselect", preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
        if (ImGui::Selectable("<clear>", parentId.empty() && childId.empty())) {
            QueueEdit(ticket.id, field, {}, pendingEdits);
        }
        ImGui::Separator();
        for (const auto& parent : field.AllowedValueOptions) {
            if (parent.Children.empty()) {
                const bool selected = (parent.Id == parentId && childId.empty());
                if (ImGui::Selectable(parent.Value.c_str(), selected)) {
                    QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, std::string())}, pendingEdits);
                }
                continue;
            }

            if (ImGui::BeginMenu(parent.Value.c_str())) {
                const bool parentOnlySelected = (parent.Id == parentId && childId.empty());
                if (ImGui::Selectable("<parent only>", parentOnlySelected)) {
                    QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, std::string())}, pendingEdits);
                }
                ImGui::Separator();
                for (const auto& child : parent.Children) {
                    const bool selected = (parent.Id == parentId && child.Id == childId);
                    if (ImGui::Selectable(child.Value.c_str(), selected)) {
                        QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, child.Id)}, pendingEdits);
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndCombo();
    }
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        const ImVec2 psz = ImGui::CalcTextSize(preview.c_str());
        const bool previewClipped = (comboAvailBefore > 0.0f && psz.x > comboAvailBefore + 1.0f);
        if (previewClipped) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            ImGui::TextUnformatted(preview.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

} // namespace

namespace {
/// RAII helper that pushes ticket.id + field-id onto the ImGui ID stack at cell entry, popping on
/// scope exit. Every widget inside RenderFieldCell (and the editors it dispatches to) inherits a
/// cell-unique ID without each call site re-allocating a "##TextCell_<ticket>_<field>" string.
/// Saves an estimated 4-6 std::string allocations per cell per frame across ~200 visible rows ×
/// ~30 columns.
struct CellIdScope {
    // Pushes ticket.id then fieldId — or the column index when fieldId is empty.
    // PushID("") hashes to the same value for every empty-FieldId column, which
    // would collapse synthetic / errored-catalog rows onto a single ImGui ID
    // and leak edit-state and popups between cells. The columnIndex fallback
    // keeps each cell distinct.
    CellIdScope(const char* ticketId, const char* fieldId, int columnIndex) {
        ImGui::PushID(ticketId);
        if (fieldId != nullptr && fieldId[0] != '\0') {
            ImGui::PushID(fieldId);
        } else {
            ImGui::PushID(columnIndex);
        }
    }
    ~CellIdScope() {
        ImGui::PopID();
        ImGui::PopID();
    }
    CellIdScope(const CellIdScope&) = delete;
    CellIdScope& operator=(const CellIdScope&) = delete;
};
} // namespace

void TicketFieldEditor::RenderFieldCell(AppController& app, const CachedTicket& ticket, const TicketGridColumn& column,
                                        int columnIndex, const TrackerField* field, const std::string& currentValue,
                                        float availWidth, bool tooltipsEnabled, bool allowEdits, SpreadsheetState& state,
                                        std::vector<PendingFieldEdit>& pendingEdits,
                                        TrackerGridFieldAsyncState& trackerGridAsync,
                                        const std::string& dateFormatOption, int thresholdDays) {
    SMATCHET_UI_PERF_SCOPE("RenderFieldCell");
    CellIdScope cellIds(ticket.id.c_str(), column.FieldId.c_str(), columnIndex);
    const bool handledByLua = app.TryLuaFieldDisplay(column.FieldId, ticket, currentValue, availWidth, field);
    if (handledByLua) {
        return;
    }

    if (SmatchetFieldIconRender::TryDrawFieldValueIcon(app, column.FieldId, field, currentValue, availWidth,
                                                       tooltipsEnabled, allowEdits)) {
        return;
    }

    auto renderPlainText = [&](bool disabled) {
        std::string display;
        if (column.IsDateLike) {
            display = DisplayValueForTrackerDateField(column.FieldId, field, currentValue,
                                                     dateFormatOption, thresholdDays);
        } else {
            display = app.ResolveDisplayValue(column.FieldId, field, currentValue);
        }
        if (disabled && display.empty()) {
            display = "-";
        }
        const std::string* tip = column.IsDateLike ? &currentValue : nullptr;
        RenderClippedFieldText(display, availWidth, tooltipsEnabled, disabled, tip);
    };

    switch (column.Plan) {
    case TicketGridColumn::RenderPlan::SpecialAttachment:
        if (field == nullptr || IsAttachmentFieldId(field->Id)) {
            TrackerGridFieldDisplay::RenderAttachmentsField(app, currentValue, availWidth, tooltipsEnabled);
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialWatchers:
        if (field == nullptr || TrackerGridFieldDisplay::IsWatchersColumnId(field->Id)) {
            TrackerGridFieldDisplay::RenderWatchersField(app, ticket.id, currentValue, availWidth, tooltipsEnabled,
                                                      trackerGridAsync);
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialVotes:
        if (field == nullptr || TrackerGridFieldDisplay::IsVotesColumnId(field->Id)) {
            TrackerGridFieldDisplay::RenderVotesField(app, ticket.id, currentValue, availWidth, tooltipsEnabled, trackerGridAsync);
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialWorklog:
        if (field == nullptr || TrackerGridFieldDisplay::IsWorklogColumnId(field->Id)) {
            TrackerGridFieldDisplay::RenderWorklogField(currentValue, availWidth, tooltipsEnabled);
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialProgress:
        if (TrackerGridFieldDisplay::TryRenderProgressJsonField(currentValue, availWidth)) {
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialIssueRestriction:
        if (TrackerGridFieldDisplay::TryRenderIssueRestrictionField(currentValue, availWidth, tooltipsEnabled)) {
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialTimeSpent: {
        std::string buttonText = currentValue.empty() ? "Log work" : currentValue;
        // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0)); // invisible background when normal
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));

        if (ImGui::Button(buttonText.c_str(), ImVec2(availWidth > 0.0f ? availWidth : -FLT_MIN, 0.0f))) {
            s_ActiveWorklogState.IssueId = ticket.id;
            s_ActiveWorklogState.TimeSpent[0] = '\0';
            s_ActiveWorklogState.OriginalEstimate = ticket.GetFieldValue("timeoriginalestimate");
            s_ActiveWorklogState.TotalTimeSpent = ticket.GetFieldValue("timespent");
            s_ActiveWorklogState.TotalTimeRemaining = ticket.GetFieldValue("timeestimate");
            std::strncpy(s_ActiveWorklogState.TimeRemaining, s_ActiveWorklogState.TotalTimeRemaining.c_str(), sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
            s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';

            s_ActiveWorklogState.DateStarted = GetCurrentJiraDateTimeString();

            s_ActiveWorklogState.WorkDescription[0] = '\0';
            s_ActiveWorklogState.ErrorMsg.clear();
            s_ActiveWorklogState.Initialized = true;
            s_ActiveWorklogState.JustOpened = true;
            s_ActiveWorklogState.TimeRemainingManuallyEdited = false;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (currentValue.empty()) {
                ImGui::TextUnformatted("No work logged yet. Click to log work.");
            } else {
                ImGui::Text("Total Time Spent: %s\nClick to log work / edit estimates.", currentValue.c_str());
            }
            ImGui::EndTooltip();
        }
        break;
    }
    case TicketGridColumn::RenderPlan::PlainText:
        renderPlainText(column.CatalogReadOnly);
        return;
    case TicketGridColumn::RenderPlan::Labels:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        TrackerLabelsEditor::RenderLabelsFieldEditor(
            app, ticket, *field, currentValue,
            [&](const std::string& issueId, const TrackerField& fld, const std::vector<std::string>& values) {
                QueueEdit(issueId, fld, values, pendingEdits);
            });
        return;
    case TicketGridColumn::RenderPlan::Cascading:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderCascadingSelectEditor(app, ticket, *field, currentValue, pendingEdits, tooltipsEnabled);
        return;
    case TicketGridColumn::RenderPlan::MultiSelect:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderMultiSelectEditor(app, ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled);
        return;
    case TicketGridColumn::RenderPlan::SingleSelect:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderSingleSelectEditor(app, ticket, *field, currentValue, pendingEdits, tooltipsEnabled);
        return;
    case TicketGridColumn::RenderPlan::DateTimeEditor:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        TrackerDateTimeFieldEditor::RenderDateTimeFieldEditor(
            ticket, *field, currentValue, state,
            [&](const std::string& issueId, const TrackerField& fld, const std::vector<std::string>& values) {
                QueueEdit(issueId, fld, values, pendingEdits);
            },
            dateFormatOption, thresholdDays);
        return;
    case TicketGridColumn::RenderPlan::TextEditor:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderTextEditor(app, ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled, availWidth);
        return;
    }

    if (s_ActiveWorklogState.Initialized && s_ActiveWorklogState.IssueId == ticket.id) {
        ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f), ImGuiCond_Always);
        if (s_ActiveWorklogState.JustOpened) {
            ImGui::OpenPopup("TimeTrackingPopup");
            s_ActiveWorklogState.JustOpened = false;
        }

        if (ImGui::BeginPopupModal("TimeTrackingPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Time tracking: %s", ticket.id.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            // Logged & Remaining progress bar
            long long spentSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeSpent);
            long long remSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeRemaining);
            long long newSpentSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);

            long long displaySpentSec = spentSec + newSpentSec;
            long long displayRemSec = remSec;
            if (newSpentSec > 0 && !s_ActiveWorklogState.TimeRemainingManuallyEdited) {
                displayRemSec = (std::max)(0LL, remSec - newSpentSec);
            } else if (s_ActiveWorklogState.TimeRemainingManuallyEdited) {
                displayRemSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeRemaining);
            }

            long long totalSec = displaySpentSec + displayRemSec;
            float fraction = 0.0f;
            if (totalSec > 0) {
                fraction = (float)displaySpentSec / (float)totalSec;
            }

            std::string loggedLabel = FormatWorkDurationFromSeconds(displaySpentSec);
            if (loggedLabel.empty()) loggedLabel = "0m";
            loggedLabel += " logged";

            std::string remainingLabel = FormatWorkDurationFromSeconds(displayRemSec);
            if (remainingLabel.empty()) remainingLabel = "0m";
            remainingLabel += " remaining";

            ImGui::TextUnformatted(loggedLabel.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(remainingLabel.c_str()).x);
            ImGui::TextUnformatted(remainingLabel.c_str());

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.12f, 0.45f, 0.88f, 1.00f)); // Jira blue
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 14.0f), "");
            ImGui::PopStyleColor();

            if (!s_ActiveWorklogState.OriginalEstimate.empty()) {
                ImGui::TextDisabled("The original estimate for this work item was %s.", s_ActiveWorklogState.OriginalEstimate.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Inputs
            ImGui::Text("Time spent *");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (DrawDurationFieldWithSuggestions("##WorklogTimeSpent", s_ActiveWorklogState.TimeSpent, sizeof(s_ActiveWorklogState.TimeSpent), 0, nullptr, nullptr, nullptr, false)) {
                if (!s_ActiveWorklogState.TimeRemainingManuallyEdited) {
                    long long spentVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);
                    long long remVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeRemaining);
                    if (spentVal > 0) {
                        long long newRem = (std::max)(0LL, remVal - spentVal);
                        std::string formattedRem = FormatWorkDurationFromSeconds(newRem);
                        std::strncpy(s_ActiveWorklogState.TimeRemaining, formattedRem.c_str(), sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
                        s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';
                    } else {
                        std::strncpy(s_ActiveWorklogState.TimeRemaining, s_ActiveWorklogState.TotalTimeRemaining.c_str(), sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
                        s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';
                    }
                }
            }
            ImGui::TextDisabled("Use the format: 2w 4d 6h 45m (w=weeks, d=days, h=hours, m=minutes)");

            ImGui::Spacing();
            ImGui::Text("Time remaining");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (DrawDurationFieldWithSuggestions("##WorklogTimeRemaining", s_ActiveWorklogState.TimeRemaining, sizeof(s_ActiveWorklogState.TimeRemaining), 0, nullptr, nullptr, &s_ActiveWorklogState.TimeRemainingManuallyEdited, false)) {
                // value changed!
            }

            ImGui::Spacing();
            ImGui::Text("Date started *");
            ImGui::SetNextItemWidth(-FLT_MIN);
            TrackerDateTimeFieldEditor::RenderGenericDatePicker("##WorklogDateStarted", s_ActiveWorklogState.DateStarted, true);

            ImGui::Spacing();
            ImGui::Text("Work description");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
            if (ImGui::Button("Templates ▼", ImVec2(90.0f, 0.0f))) {
                ImGui::OpenPopup("comment_templates_popup");
            }
            if (ImGui::BeginPopup("comment_templates_popup")) {
                std::vector<std::string> templates = LoadCommentTemplates();
                if (templates.empty()) {
                    ImGui::TextDisabled("No templates configured in Preferences.");
                } else {
                    for (const auto& item : templates) {
                        if (ImGui::Selectable(item.c_str())) {
                            std::strncpy(s_ActiveWorklogState.WorkDescription, item.c_str(), sizeof(s_ActiveWorklogState.WorkDescription) - 1);
                            s_ActiveWorklogState.WorkDescription[sizeof(s_ActiveWorklogState.WorkDescription) - 1] = '\0';
                        }
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::InputTextMultiline("##WorklogDesc", s_ActiveWorklogState.WorkDescription, sizeof(s_ActiveWorklogState.WorkDescription), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));

            if (!s_ActiveWorklogState.ErrorMsg.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", s_ActiveWorklogState.ErrorMsg.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Buttons
            if (ImGui::Button("Save", ImVec2(80, 0))) {
                s_ActiveWorklogState.ErrorMsg.clear();
                long long sVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);
                if (sVal <= 0) {
                    s_ActiveWorklogState.ErrorMsg = "Invalid Time spent format. Please use e.g. 2h 30m.";
                } else {
                    ParsedJiraDateTime dummyDt;
                    if (!TryParseJiraDateTime(s_ActiveWorklogState.DateStarted, dummyDt)) {
                        s_ActiveWorklogState.ErrorMsg = "Invalid Date started format.";
                    } else {
                        std::string adjEst = s_ActiveWorklogState.TimeRemainingManuallyEdited ? "new" : "auto";
                        std::string outErr;
                        if (app.SubmitWorklog(s_ActiveWorklogState.IssueId,
                                              s_ActiveWorklogState.TimeSpent,
                                              s_ActiveWorklogState.TimeRemaining,
                                              adjEst,
                                              s_ActiveWorklogState.WorkDescription,
                                              s_ActiveWorklogState.DateStarted,
                                              outErr)) {
                            ImGui::CloseCurrentPopup();
                            s_ActiveWorklogState.Initialized = false;
                        } else {
                            s_ActiveWorklogState.ErrorMsg = "Failed: " + outErr;
                        }
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                ImGui::CloseCurrentPopup();
                s_ActiveWorklogState.Initialized = false;
            }

            ImGui::EndPopup();
        } else {
            s_ActiveWorklogState.Initialized = false;
        }
    }
}

void TicketFieldEditor::RenderLongTextModal(std::vector<PendingFieldEdit>& pendingEdits) {
    if (!s_ActiveLongTextState.Active) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (s_ActiveLongTextState.JustOpened) {
        // OpenPopup must be called at the same window-depth as BeginPopupModal. Both live here,
        // outside the table, so ImGui can correctly anchor the popup to this window.
        ImGui::OpenPopup(kLongTextModalPopupId);
        const ImVec2 modalSize(viewport->Size.x * 0.6f, viewport->Size.y * 0.65f);
        ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }

    if (ImGui::BeginPopupModal(kLongTextModalPopupId, nullptr,
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Edit %s — %s", s_ActiveLongTextState.FieldLabel.c_str(),
                    s_ActiveLongTextState.IssueId.c_str());

        // Format-fidelity banners. RawMode is the strongest signal — fall back to editing
        // the source HTML directly so we don't destroy unrecognized markup. DroppedAdfNodeTypes
        // is informational: the rendered Markdown is missing some original constructs but the
        // user can still edit and save the rest cleanly.
        if (s_ActiveLongTextState.RawMode) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
            ImGui::TextWrapped("Editing raw HTML — the source contains tags this build doesn't yet "
                               "translate to Markdown. Save will store your edits verbatim.");
            ImGui::PopStyleColor();
        } else if (!s_ActiveLongTextState.DroppedAdfNodeTypes.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
            std::string list;
            for (size_t i = 0; i < s_ActiveLongTextState.DroppedAdfNodeTypes.size() && i < 5; ++i) {
                if (!list.empty()) list += ", ";
                list += s_ActiveLongTextState.DroppedAdfNodeTypes[i];
            }
            ImGui::TextWrapped("Note: this document contains constructs not in our Markdown subset (%s) — "
                               "saving will keep what you edit but those nodes are not shown.",
                               list.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        // Reserve room for the footer (status line + button row).
        const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() +
                              ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 inputSize(-FLT_MIN, ImGui::GetContentRegionAvail().y - footerH);

        if (s_ActiveLongTextState.JustOpened) {
            ImGui::SetKeyboardFocusHere();
            s_ActiveLongTextState.JustOpened = false;
        }

        // Modifier state — declared up here so the Ctrl+P preview-mode shortcut sees it
        // before we render the panes. Ctrl+Enter / Esc are checked again below for save/cancel.
        const bool ctrlDown = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);

        // Preview is disabled in raw-HTML mode (the buffer is HTML, not Markdown — md4c would
        // produce nonsense), so we force EditOnly there regardless of the user's last choice.
        using PreviewModeT = ActiveLongTextEditorState::PreviewModeT;
        const PreviewModeT effectiveMode = s_ActiveLongTextState.RawMode
            ? PreviewModeT::EditOnly
            : s_ActiveLongTextState.PreviewMode;

        // Ctrl+P cycles Edit -> Split -> Preview -> Edit. Captured here (not in the footer)
        // so it works with focus inside the textarea.
        if (!s_ActiveLongTextState.RawMode && ctrlDown && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
            switch (s_ActiveLongTextState.PreviewMode) {
                case PreviewModeT::EditOnly:    s_ActiveLongTextState.PreviewMode = PreviewModeT::Split; break;
                case PreviewModeT::Split:       s_ActiveLongTextState.PreviewMode = PreviewModeT::PreviewOnly; break;
                case PreviewModeT::PreviewOnly: s_ActiveLongTextState.PreviewMode = PreviewModeT::EditOnly; break;
            }
        }

        // Pane layout for Split mode: [editor] [splitter] [preview]. The splitter is a 6px
        // invisible button whose drag delta updates SplitterRatio. Clamped so neither pane
        // collapses below 15% of the available width.
        // Width semantics: in EditOnly / PreviewOnly the visible pane gets -FLT_MIN so ImGui
        // fills the popup the same way the original single-pane code did. Only Split needs
        // explicit pixel widths so the splitter sits between two real columns.
        const float splitterW = 6.0f;
        const float totalW = ImGui::GetContentRegionAvail().x;
        const float panesW = (effectiveMode == PreviewModeT::Split)
                                 ? (totalW - splitterW)
                                 : totalW;
        const float splitEditorW = panesW * s_ActiveLongTextState.SplitterRatio;
        const float splitPreviewW = panesW - splitEditorW;
        const float editorW = (effectiveMode == PreviewModeT::Split) ? splitEditorW : -FLT_MIN;
        const float previewW = (effectiveMode == PreviewModeT::Split) ? splitPreviewW : -FLT_MIN;

        const ImGuiID editorId = ImGui::GetID("##LongTextEditorBuf");

        if (effectiveMode != PreviewModeT::PreviewOnly) {
            ImGui::InputTextMultiline("##LongTextEditorBuf",
                                      s_ActiveLongTextState.Buffer.data(),
                                      s_ActiveLongTextState.Buffer.size(),
                                      ImVec2(editorW, inputSize.y),
                                      ImGuiInputTextFlags_AllowTabInput);
        }

        if (effectiveMode == PreviewModeT::Split) {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::InvisibleButton("##LongTextSplitter", ImVec2(splitterW, inputSize.y));
            const bool splitterHover = ImGui::IsItemHovered();
            const bool splitterActive = ImGui::IsItemActive();
            if (splitterHover || splitterActive) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (splitterActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                const float dx = ImGui::GetIO().MouseDelta.x;
                if (panesW > 0.0f) {
                    float r = s_ActiveLongTextState.SplitterRatio + dx / panesW;
                    if (r < 0.15f) r = 0.15f;
                    if (r > 0.85f) r = 0.85f;
                    s_ActiveLongTextState.SplitterRatio = r;
                }
            }
            // Visual hint for the splitter — a faint vertical bar, brighter on hover/drag.
            const ImVec2 sMin = ImGui::GetItemRectMin();
            const ImVec2 sMax = ImGui::GetItemRectMax();
            const ImU32 col = ImGui::GetColorU32(splitterActive ? ImGuiCol_SeparatorActive
                                                  : splitterHover ? ImGuiCol_SeparatorHovered
                                                                  : ImGuiCol_Separator);
            ImGui::GetWindowDrawList()->AddRectFilled(sMin, sMax, col);
            ImGui::SameLine(0.0f, 0.0f);
        }

        if (effectiveMode != PreviewModeT::EditOnly) {
            ImGui::BeginChild("##LongTextPreview",
                              ImVec2(previewW, inputSize.y),
                              true, ImGuiWindowFlags_HorizontalScrollbar);
            // Capture the preview window pointer while it's the current window — child names
            // are munged by BeginChild, so FindWindowByName won't match the literal id we passed.
            ::ImGuiWindow* previewWin = ::ImGui::GetCurrentWindow();
            const std::string md(s_ActiveLongTextState.Buffer.data());

            // PR-5: round-trip the buffer through the same converter that runs on
            // save (Markdown -> ADF / HTML -> Markdown), then render the result.
            // Any conversion loss surfaces here before the user clicks Save.
            // Debounced: arm a 100 ms timer on each buffer change; fire only when idle so
            // rapid keystrokes on 64 KB buffers don't re-convert every frame.
            if (md != s_ActiveLongTextState.LastRoundTripInput && !s_ActiveLongTextState.RoundTripPending) {
                s_ActiveLongTextState.RoundTripPending = true;
                s_ActiveLongTextState.RoundTripFireAt =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            }
            if (s_ActiveLongTextState.RoundTripPending &&
                std::chrono::steady_clock::now() >= s_ActiveLongTextState.RoundTripFireAt) {
                s_ActiveLongTextState.RoundTripPending = false;
                std::string rendered;
                bool lossy = false;
                try {
                    if (s_ActiveLongTextState.RichKind == LongTextRichKind::Html) {
                        const std::string html = MarkdownConvert::MarkdownToHtml(md);
                        bool fellBack = false;
                        rendered = MarkdownConvert::HtmlSubsetToMarkdown(html, &fellBack);
                        lossy = fellBack;
                    } else {
                        // Default to ADF for None / Adf — covers Jira description and
                        // generic ADF fields. New issues without a stored rich value
                        // (RichKind::None) still go through ADF since that is what
                        // the save path emits.
                        const nlohmann::json adf = MarkdownConvert::MarkdownToAdf(md);
                        std::vector<std::string> dropped;
                        rendered = MarkdownConvert::AdfToMarkdown(adf, &dropped);
                        lossy = !dropped.empty();
                    }
                } catch (...) {
                    // Converter blew up on the in-progress edit (very rare; usually
                    // mid-token). Fall back to the raw buffer so the preview keeps
                    // updating; not lossy because we did not transform anything.
                    rendered = md;
                    lossy = false;
                }
                s_ActiveLongTextState.LastRoundTripInput = md;
                s_ActiveLongTextState.LastRoundTripOutput = std::move(rendered);
                s_ActiveLongTextState.LastRoundTripLossy = lossy;
            }
            if (s_ActiveLongTextState.LastRoundTripLossy) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                ImGui::TextUnformatted("(lossy round-trip — some constructs will be dropped on save)");
                ImGui::PopStyleColor();
                ImGui::Separator();
            }
            RenderMarkdownPreview(s_ActiveLongTextState.LastRoundTripOutput);
            const float previewScroll = ImGui::GetScrollY();
            const float previewScrollMax = ImGui::GetScrollMaxY();
            ImGui::EndChild();

            // Scroll-sync: in Split mode, mirror whichever pane's scroll changed this frame
            // onto the other. Editor scroll comes from the InputTextMultiline child (looked
            // up by id). Editor wins ties (typing-driven scroll is the common case).
            if (effectiveMode == PreviewModeT::Split) {
                ::ImGuiWindow* editorWin = ::ImGui::FindWindowByID(editorId);
                const float editorScroll = editorWin ? editorWin->Scroll.y : 0.0f;
                const float editorScrollMax = editorWin ? editorWin->ScrollMax.y : 0.0f;

                const bool editorChanged =
                    std::fabs(editorScroll - s_ActiveLongTextState.LastEditorScrollY) > 0.5f;
                const bool previewChanged =
                    std::fabs(previewScroll - s_ActiveLongTextState.LastPreviewScrollY) > 0.5f;

                if (editorChanged && previewWin && editorScrollMax > 0.0f && previewScrollMax > 0.0f) {
                    const float r = editorScroll / editorScrollMax;
                    previewWin->Scroll.y = r * previewScrollMax;
                } else if (previewChanged && editorWin && editorScrollMax > 0.0f && previewScrollMax > 0.0f) {
                    const float r = previewScroll / previewScrollMax;
                    editorWin->Scroll.y = r * editorScrollMax;
                }

                s_ActiveLongTextState.LastEditorScrollY = editorWin ? editorWin->Scroll.y : 0.0f;
                s_ActiveLongTextState.LastPreviewScrollY = previewWin ? previewWin->Scroll.y : previewScroll;
            } else {
                s_ActiveLongTextState.LastPreviewScrollY = previewScroll;
            }
        }

        // Ctrl+Enter saves; Esc cancels. Both work even when the textarea is focused.
        // (ctrlDown declared above for the Ctrl+P shortcut.)
        const bool ctrlEnter = ctrlDown && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        const bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

        const char* footerHint = s_ActiveLongTextState.RawMode
            ? "Ctrl+Enter to save · Esc to cancel · raw HTML mode (Markdown disabled)"
            : "Ctrl+Enter to save · Esc to cancel · Ctrl+P preview mode · Markdown — **bold**, *em*, # heading, - list, ```code```";
        ImGui::TextDisabled("%s", footerHint);

        bool save = ctrlEnter;
        bool cancel = escPressed;
        if (ImGui::Button("Save", ImVec2(100, 0))) {
            save = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            cancel = true;
        }
        // Three-way segmented control: Edit / Split / Preview. Disabled in RawMode (Markdown
        // rendering is meaningless when the buffer is HTML). Ctrl+P cycles modes — see the
        // shortcut handler above the panes.
        ImGui::SameLine();
        if (s_ActiveLongTextState.RawMode) ImGui::BeginDisabled();
        auto segBtn = [](const char* label, ActiveLongTextEditorState::PreviewModeT mode) {
            const bool active = s_ActiveLongTextState.PreviewMode == mode;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::Button(label, ImVec2(80, 0))) {
                s_ActiveLongTextState.PreviewMode = mode;
            }
            if (active) {
                ImGui::PopStyleColor();
            }
        };
        segBtn("Edit", ActiveLongTextEditorState::PreviewModeT::EditOnly);
        ImGui::SameLine(0.0f, 2.0f);
        segBtn("Split", ActiveLongTextEditorState::PreviewModeT::Split);
        ImGui::SameLine(0.0f, 2.0f);
        segBtn("Preview", ActiveLongTextEditorState::PreviewModeT::PreviewOnly);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Ctrl+P to cycle Edit / Split / Preview");
        }
        if (s_ActiveLongTextState.RawMode) ImGui::EndDisabled();

        if (save) {
            const std::string newValue(s_ActiveLongTextState.Buffer.data());
            // Diff against the seeded markdown (or the raw HTML in RawMode) so we don't queue a
            // null edit that would still re-emit through the payload converter and reshape
            // formatting that hasn't actually changed.
            const std::string& seed = s_ActiveLongTextState.RawMode
                                          ? s_ActiveLongTextState.OriginalRichValue
                                          : s_ActiveLongTextState.OriginalMarkdown;
            if (newValue != seed) {
                PendingFieldEdit edit;
                edit.IssueId = s_ActiveLongTextState.IssueId;
                edit.Field = s_ActiveLongTextState.Field;
                edit.Values = {newValue};
                edit.Preformatted = s_ActiveLongTextState.RawMode;
                edit.OriginalRichValue = s_ActiveLongTextState.OriginalRichValue;
                pendingEdits.push_back(std::move(edit));
            }
            ImGui::CloseCurrentPopup();
            CloseLongTextEditor();
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
            CloseLongTextEditor();
        }

        ImGui::EndPopup();
    } else {
        // Popup was dismissed without our intervention (shouldn't normally happen for a modal,
        // but stay self-consistent if it does).
        CloseLongTextEditor();
    }
}


