#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SelectableText {
struct Context;
} // namespace SelectableText

/// Shared Markdown preview renderer. Walks a Markdown string with md4c and emits
/// ImGui draw calls. Used by the long-text editor modal (Full mode) and the
/// description tooltip path (Tooltip mode — drops heading scaling, code BeginChild,
/// and link click handling so nothing breaks when nested inside a tooltip window).
namespace MarkdownPreviewRender {

enum class Mode { Full, Tooltip };

// Inline mark bits — combined per-run via OR. Public so unit tests can
// inspect PreviewPlan::Block::runs without reverse-engineering the bitfield.
constexpr std::uint8_t kMarkBold = 1u << 0;
constexpr std::uint8_t kMarkItalic = 1u << 1;
constexpr std::uint8_t kMarkCode = 1u << 2;
constexpr std::uint8_t kMarkStrike = 1u << 3;
constexpr std::uint8_t kMarkLink = 1u << 4;

struct StyledRun {
    std::string text;
    std::uint8_t marks = 0;
    std::string href;
};

// Per-word atom emitted by the wrap loop. Opt B — pre-decomposed once per
// PreviewPlan::Block (lazy, on first RenderPlan walk) + cached. Eliminates
// per-frame `ImGui::CalcTextSize` + `PushFont/TextUnformatted/PopFont` work.
struct PlanWord {
    std::string text; // empty when forceBreak == true
    std::uint8_t marks = 0;
    std::string href;
    bool forceBreak = false; // '\n' inside the source — forces a hard wrap
    float widthPx = 0.0f;    // ImGui::CalcTextSize().x with the marks-resolved font
};

struct TableCellData {
    std::vector<StyledRun> runs;
};

struct TableRowData {
    std::vector<TableCellData> cells;
    bool isHeader = false;
};

/// Per-render plan — captures the md4c parse output (one entry per block) so
/// subsequent renders of the same content can skip the parser hot path.
/// Built by `BuildPlan`; replayed by `RenderPlan` or implicitly by `Render`
/// (single-shot wrapper). Plan does NOT carry per-Render runtime state
/// (mode, wrapWidth, selCtx) — those stay on Options at render time so the
/// same plan can be reused across modes and surfaces. Pillar 1 opt #1.
struct PreviewPlan {
    struct Block {
        enum Kind {
            kPara,          // styled prose runs flushed as a paragraph
            kHeading,       // styled prose runs flushed as an H1..H6
            kHr,            // horizontal rule
            kListItemBegin, // depth + pre-resolved marker text (counter baked in)
            kListItemEnd,   // matching unindent
            kQuoteBegin,    // indent + push green tint
            kQuoteEnd,      // pop tint + unindent
            kCode,          // codeBuffer + codeLang
            kTable,         // tableRows + tableColCount
        };
        Kind kind = kPara;
        // Sparse per-kind data. Empty vectors / strings cost only sizeof
        // overhead — no allocations until populated.
        std::vector<StyledRun> runs;         // kPara, kHeading
        int headingLevel = 0;                // kHeading
        std::string codeBuffer;              // kCode
        std::string codeLang;                // kCode
        std::vector<TableRowData> tableRows; // kTable
        int tableColCount = 0;               // kTable
        std::string listMarker;              // kListItemBegin (UTF-8, pre-counter-resolved)
        int listDepth = 0;                   // kListItemBegin
        // Opt B — pre-decomposed words + cached widths. mutable so RenderPlan
        // (const PreviewPlan&) can populate on first walk. Single-thread-UI
        // invariant makes the laundered const-correctness safe in practice.
        mutable std::vector<PlanWord> words; // kPara, kHeading
        mutable bool wordsBuilt = false;
    };
    std::vector<Block> blocks;
};

/// Custom deleter so callers can hold `unique_ptr<PreviewPlan>` without
/// needing the full struct definition (definition lives in the .cpp).
struct PreviewPlanDeleter {
    void operator()(PreviewPlan* p) const noexcept;
};

/// Heap-allocated PreviewPlan handle. unique_ptr so callers can stash plans
/// in containers without exposing the full struct.
using PreviewPlanPtr = std::unique_ptr<PreviewPlan, PreviewPlanDeleter>;

PreviewPlanPtr MakePlan();

/// 64-bit content hash over the raw markdown bytes. Stable across runs;
/// suitable for cache keys. Pure function — no global state.
std::uint64_t HashContent(const char* bytes, std::size_t len);
inline std::uint64_t HashContent(const std::string& md) { return HashContent(md.data(), md.size()); }

struct Options {
    Mode mode = Mode::Full;
    /// <=0 falls back to ImGui::GetContentRegionAvail().x at render time.
    float wrapWidth = 0.0f;
    /// Off in Tooltip mode — tooltip dismisses on mouse-up so the click never lands.
    bool clickableLinks = true;
    /// When non-empty AND `existingSelCtx == nullptr`, wraps the render in
    /// SelectableText::Begin(selectableId) / End() so prose is drag-selectable
    /// + Ctrl+C-copyable. Code blocks + tables remain non-selectable in MVP.
    /// See docs/design/archive/markdown-language-definition-for-textedit.md § Slice 4.
    const char* selectableId = nullptr;
    /// When non-null, prose runs register Segments into THIS Context instead
    /// of opening a fresh one. Used by the AI chat surface where one outer
    /// Context spans many sequential Render() calls (one per message) so
    /// drag-select crosses message boundaries. Overrides `selectableId`.
    SelectableText::Context* existingSelCtx = nullptr;
};

/// Parse `md` into `outPlan`. No ImGui calls. Idempotent — calling twice with
/// the same input yields the same plan content. Use this + `RenderPlan` when
/// the caller wants to cache plans across frames (e.g. AI chat history).
void BuildPlan(const std::string& md, PreviewPlan& outPlan);

/// Walk `plan` + emit ImGui draw calls per the runtime `opts`. Stateless
/// other than ImGui's own immediate-mode state.
void RenderPlan(const PreviewPlan& plan, const Options& opts);

/// One-shot helper — `BuildPlan` + `RenderPlan` against an internal temporary.
/// Use when caching isn't desired (tooltip preview, ad-hoc rendering).
void Render(const std::string& md, const Options& opts = Options());

/// Return true when an md4c fenced-code language tag should route to the C++
/// syntax tokenizer. Case-insensitive; covers common aliases. Inlined so the
/// pure-logic test rig can link this without pulling md4c / ImGui / fonts.
inline bool IsCppLikeLangTag(const std::string& lang) {
    if (lang.empty()) {
        return false;
    }
    std::string lower(lang.size(), '\0');
    std::transform(lang.begin(), lang.end(), lower.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return lower == "cpp" || lower == "c++" || lower == "cxx" || lower == "cc" || lower == "c" || lower == "hpp" ||
           lower == "h";
}

} // namespace MarkdownPreviewRender
