#pragma once

// Pure rules behind the views sidebar search and the view-switch key hints
// (docs/audits/INPUT_UX_AUDIT.md #65; plans input-ux-w2-action-keys.md row 14 and input-ux-w2-one-matcher.md).
// ImGui-free, so the list order and the hint text are unit-tested on their own.

#include "Config/KeybindingsConfig.h"
#include "TextMatchPure.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace views {

/// One row the views sidebar lists: the view's index in the store and the bytes of its name that matched
/// the search (empty when nothing is typed).
struct ListedView {
    std::size_t Index = 0;
    std::vector<textmatch::ByteRange> NameRanges;
};

/// The storage ListViewsForSearch fills. A list keeps one across frames (next to its `textmatch::Scratch`),
/// so once the vectors have grown to the store's size, listing the views allocates nothing.
struct ViewListBuffers {
    std::vector<ListedView> Listed; // the result, in display order
    std::vector<textmatch::Result> Results;
    std::vector<std::vector<textmatch::ByteRange>> Ranges; // per view, in store order
    std::vector<long long> Rank;                           // per view, in store order
    std::vector<std::size_t> Order;                        // the listed views' store indices
};

/// The views the sidebar lists for `query`, in the order it shows them, written to `buf.Listed`. With
/// nothing typed (no query words), every view in store order. Otherwise the shared matcher's rule: rows
/// whose name holds every word as written come first and, when there is at least one, are the only rows
/// listed; else names that hold the words' letters in order. Best match first; equal ranks keep store
/// order, so Enter's "first listed" view is stable. `nameAt(i)` returns view i's name (a
/// `const std::string&`).
template <typename NameAt>
void ListViewsForSearch(std::size_t count, NameAt nameAt, const char* query, textmatch::Scratch& scratch,
                        ViewListBuffers& buf) {
    const textmatch::Query q = textmatch::MakeQuery(query);
    buf.Order.clear();
    if (q.Empty()) {
        for (std::size_t i = 0; i < count; ++i) {
            buf.Order.push_back(i);
        }
    } else {
        buf.Results.resize(count);
        buf.Rank.resize(count);
        if (buf.Ranges.size() < count) {
            buf.Ranges.resize(count);
        }
        bool anyExact = false;
        for (std::size_t i = 0; i < count; ++i) {
            const std::string& name = nameAt(i);
            buf.Results[i] =
                textmatch::Match(q, name.data(), name.size(), textmatch::Mode::Fuzzy, scratch, &buf.Ranges[i]);
            anyExact = anyExact || (buf.Results[i].Matched() && buf.Results[i].Substring);
            buf.Rank[i] = textmatch::RankKey(buf.Results[i]);
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (textmatch::RowListed(buf.Results[i], anyExact)) {
                buf.Order.push_back(i);
            }
        }
        const std::vector<long long>& rank = buf.Rank;
        std::stable_sort(buf.Order.begin(), buf.Order.end(),
                         [&rank](std::size_t a, std::size_t b) { return rank[a] > rank[b]; });
    }
    // resize + assign keep each row's range storage from the frame before.
    buf.Listed.resize(buf.Order.size());
    for (std::size_t k = 0; k < buf.Order.size(); ++k) {
        ListedView& row = buf.Listed[k];
        row.Index = buf.Order[k];
        if (q.Empty()) {
            row.NameRanges.clear();
        } else {
            row.NameRanges.assign(buf.Ranges[row.Index].begin(), buf.Ranges[row.Index].end());
        }
    }
}

/// The same list returned by value, for a one-off caller (and the unit tests).
template <typename NameAt>
std::vector<ListedView> ListViewsForSearch(std::size_t count, NameAt nameAt, const char* query,
                                           textmatch::Scratch& scratch) {
    ViewListBuffers buf;
    ListViewsForSearch(count, nameAt, query, scratch, buf);
    return std::move(buf.Listed);
}

/// `lead` followed by one line per bound view-switch key, read from `bindings` so a rebind shows at once:
/// "<nextLabel> <key>" for view.next and "<prevLabel> <key>" for view.previous. An unbound (or disabled)
/// command adds no line. Labels are passed in already localized.
inline std::string ViewSwitchKeysTooltip(const std::string& lead, const std::vector<Keybinding>& bindings,
                                         const std::string& nextLabel, const std::string& prevLabel) {
    std::string tip = lead;
    const std::string nextKey = BoundHotkeyDisplay(bindings, "view.next");
    const std::string prevKey = BoundHotkeyDisplay(bindings, "view.previous");
    if (!nextKey.empty()) {
        tip += "\n" + nextLabel + " " + nextKey;
    }
    if (!prevKey.empty()) {
        tip += "\n" + prevLabel + " " + prevKey;
    }
    return tip;
}

} // namespace views
} // namespace smatchet
