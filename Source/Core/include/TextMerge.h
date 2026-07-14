#pragma once
#include <string>

/// Line-based 3-way text merge for offline-edit replay (docs/plans/rich-text-editing-v2-remaining.md PR-E).
/// Takes three versions of a document as plain-text strings (typically Markdown produced by
/// MarkdownConvert):
///   base  — the document at the time the user started editing (stored alongside the offline edit)
///   mine  — the user's edited version
///   theirs — the document as it exists on the server right now
/// Returns a MergeResult where:
///   IsClean=true  → merged text contains the union of changes from `mine` and `theirs`
///   IsClean=false → one or more regions conflict; `Text` contains standard conflict markers:
///                   <<<<<<< mine  …  =======  …  >>>>>>> theirs
/// Algorithm: LCS-based diff3. Splits each text into lines, computes two diffs (base→mine,
/// base→theirs), walks the resulting hunk sequences and applies non-overlapping changes
/// directly; records conflict markers where both sides changed the same base region.
namespace TextMerge {

struct MergeResult {
    bool IsClean = true;
    /// Merged text. On conflict, contains standard <<<<<<< / ======= / >>>>>>> markers.
    std::string Text;
};

MergeResult ThreeWayMerge(const std::string& base, const std::string& mine, const std::string& theirs);

} // namespace TextMerge
