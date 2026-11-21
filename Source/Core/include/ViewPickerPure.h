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

/// The views the sidebar lists for `query`, in the order it shows them. With nothing typed (no query
/// words), every view in store order. Otherwise the shared matcher's rule: rows whose name holds every
/// word as written come first and, when there is at least one, are the only rows listed; else names that
/// hold the words' letters in order. Best match first; equal ranks keep store order, so Enter's "first
/// listed" view is stable. `nameAt(i)` returns view i's name (a `const std::string&`).
template <typename NameAt>
std::vector<ListedView> ListViewsForSearch(std::size_t count, NameAt nameAt, const char* query,
                                           textmatch::Scratch& scratch) {
    std::vector<ListedView> out;
    const textmatch::Query q = textmatch::MakeQuery(query);
    if (q.Empty()) {
        out.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            out[i].Index = i;
        }
        return out;
    }
    std::vector<long long> rank;
    bool anyExact = false;
    std::vector<textmatch::ByteRange> ranges;
    std::vector<textmatch::Result> results(count);
    std::vector<std::vector<textmatch::ByteRange>> rangesByView(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& name = nameAt(i);
        results[i] = textmatch::Match(q, name.data(), name.size(), textmatch::Mode::Fuzzy, scratch, &ranges);
        if (results[i].Matched()) {
            anyExact = anyExact || results[i].Substring;
            rangesByView[i].swap(ranges);
        }
        ranges.clear();
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (textmatch::RowListed(results[i], anyExact)) {
            ListedView row;
            row.Index = i;
            row.NameRanges.swap(rangesByView[i]);
            out.push_back(std::move(row));
            rank.push_back(textmatch::RankKey(results[i]));
        }
    }
    std::vector<std::size_t> order(out.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&rank](std::size_t a, std::size_t b) { return rank[a] > rank[b]; });
    std::vector<ListedView> sorted;
    sorted.reserve(out.size());
    for (const std::size_t at : order) {
        sorted.push_back(std::move(out[at]));
    }
    return sorted;
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
