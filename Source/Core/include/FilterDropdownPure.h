#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

// ImGui-free decisions of the shared filter dropdown (FilterDropdown.h, docs/plans/shared-dropdown-recents.md):
// which rows are visible for a filter, where the Recent section goes, which part of a label matched, and
// how the keyboard highlight moves. Split out so they are unit-tested on the desktop test build
// (tests/Core/FilterDropdownPure.test.cpp); the ImGui body only draws the rows this model returns.
namespace smatchet {
namespace dropdown {
namespace pure {

/// What the model needs to know about one option. Pointers must outlive the call; null reads as "".
struct ItemView {
    const char* Id = "";
    const char* Label = "";
    const char* Secondary = nullptr;
    bool Checked = false;  // the current value (single-select) or a ticked value (multi-select)
    bool Disabled = false; // shown, never picked
};

inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/// ASCII-case-insensitive substring search with no allocation. `needleLower` must already be lower-case.
/// Returns the byte offset of the first match, or std::string::npos. An empty needle matches at 0.
inline std::size_t FindCaseInsensitive(const char* haystack, const std::string& needleLower) {
    if (needleLower.empty()) {
        return 0;
    }
    if (haystack == nullptr) {
        return std::string::npos;
    }
    const std::size_t hayLen = std::strlen(haystack);
    const std::size_t needleLen = needleLower.size();
    if (needleLen > hayLen) {
        return std::string::npos;
    }
    for (std::size_t start = 0; start + needleLen <= hayLen; ++start) {
        std::size_t k = 0;
        while (k < needleLen && LowerAscii(haystack[start + k]) == needleLower[k]) {
            ++k;
        }
        if (k == needleLen) {
            return start;
        }
    }
    return std::string::npos;
}

/// True if two strings are equal ignoring ASCII case.
inline bool EqualsCaseInsensitive(const char* a, const std::string& bLower) {
    if (a == nullptr) {
        return bLower.empty();
    }
    return std::strlen(a) == bLower.size() && FindCaseInsensitive(a, bLower) == 0;
}

/// An option passes the filter when its label, secondary text or id contains it. An empty filter passes
/// everything. The rule every picker filters by.
inline bool ItemMatches(const ItemView& item, const std::string& filterLower) {
    if (filterLower.empty()) {
        return true;
    }
    return FindCaseInsensitive(item.Label, filterLower) != std::string::npos ||
           FindCaseInsensitive(item.Secondary, filterLower) != std::string::npos ||
           FindCaseInsensitive(item.Id, filterLower) != std::string::npos;
}

/// How well an option that passes the filter answers it: 0 its label starts with the filter, 1 its label
/// contains it, 2 only its secondary text or id does. Enter acts on the best-ranked option, so typing a
/// name never picks an earlier option whose opaque id or description happens to contain the same letters.
inline int MatchRank(const ItemView& item, const std::string& filterLower) {
    const std::size_t at = FindCaseInsensitive(item.Label, filterLower);
    if (at == 0) {
        return 0;
    }
    return at != std::string::npos ? 1 : 2;
}

/// Trims ASCII whitespace from both ends and lower-cases ASCII letters: the form every filter compare uses.
inline std::string NormalizeFilter(const char* raw) {
    if (raw == nullptr) {
        return {};
    }
    const char* begin = raw;
    const char* end = raw + std::strlen(raw);
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (begin < end && isSpace(*begin)) {
        ++begin;
    }
    while (end > begin && isSpace(*(end - 1))) {
        --end;
    }
    std::string out(begin, end);
    for (char& c : out) {
        c = LowerAscii(c);
    }
    return out;
}

/// Trims ASCII whitespace from both ends, keeping case (the text a create row would add).
inline std::string TrimFilter(const char* raw) {
    if (raw == nullptr) {
        return {};
    }
    const char* begin = raw;
    const char* end = raw + std::strlen(raw);
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (begin < end && isSpace(*begin)) {
        ++begin;
    }
    while (end > begin && isSpace(*(end - 1))) {
        --end;
    }
    return std::string(begin, end);
}

enum class RowKind {
    HeaderRecent, // "Recent" section title
    HeaderAll,    // "All" section title, only under a Recent section
    Clear,        // the clear-selection row
    Item,         // an option (ItemIndex) or, when ItemIndex < 0, a recent id that is not in the list (RecentIndex)
    Create,       // 'Add "<filter>"'
};

struct Row {
    RowKind Kind = RowKind::Item;
    int ItemIndex = -1;   // index into the items; -1 for a recent id outside the list
    int RecentIndex = -1; // index into the recents; -1 outside the Recent section
};

struct ModelInput {
    const ItemView* Items = nullptr;
    int Count = 0;
    std::string FilterLower;                           // NormalizeFilter(buffer)
    const std::vector<std::string>* Recents = nullptr; // newest first; null = no Recent section
    int RecentsMinOptions = 10;                        // the Recent section needs MORE options than this
    bool RecentsShowUnknown = false;                   // also list recent ids that are not in Items
    bool KeepCheckedVisible = false;                   // checked options stay listed when the filter hides them
    bool AllowCreate = false;                          // offer 'Add "<filter>"' when no label equals the filter
    bool ShowClear = false;                            // a clear row above the list (unfiltered only)
};

inline int FindItemById(const ItemView* items, int count, const std::string& id) {
    for (int i = 0; i < count; ++i) {
        const char* itemId = items[i].Id != nullptr ? items[i].Id : "";
        if (id == itemId) {
            return i;
        }
    }
    return -1;
}

/// True if 'Add "<filter>"' should be offered: the filter is not blank and no option's label equals it
/// (ignoring ASCII case).
inline bool ShouldOfferCreate(const ItemView* items, int count, const std::string& filterLower) {
    if (filterLower.empty()) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (EqualsCaseInsensitive(items[i].Label, filterLower)) {
            return false;
        }
    }
    return true;
}

/// The visible rows, top to bottom.
///   No filter: [Clear] [Recent header, recent rows, All header] every option.
///   Filter:    matching options (plus checked ones when KeepCheckedVisible) [Create].
/// The Recent section appears only with no filter, more than RecentsMinOptions options, and at least one
/// recent id that resolves to an option (or any recent id when RecentsShowUnknown). Recent options are
/// listed again in their normal place below.
inline void BuildRows(const ModelInput& in, std::vector<Row>& out) {
    out.clear();
    const int count = in.Items != nullptr ? in.Count : 0;
    if (!in.FilterLower.empty()) {
        for (int i = 0; i < count; ++i) {
            if (ItemMatches(in.Items[i], in.FilterLower) || (in.KeepCheckedVisible && in.Items[i].Checked)) {
                Row row;
                row.Kind = RowKind::Item;
                row.ItemIndex = i;
                out.push_back(row);
            }
        }
        if (in.AllowCreate && ShouldOfferCreate(in.Items, count, in.FilterLower)) {
            Row row;
            row.Kind = RowKind::Create;
            out.push_back(row);
        }
        return;
    }
    if (in.ShowClear) {
        Row row;
        row.Kind = RowKind::Clear;
        out.push_back(row);
    }
    if (in.Recents != nullptr && count > in.RecentsMinOptions) {
        const std::size_t headerAt = out.size();
        const int recentCount = static_cast<int>(in.Recents->size());
        for (int r = 0; r < recentCount; ++r) {
            const int itemIndex = FindItemById(in.Items, count, (*in.Recents)[static_cast<std::size_t>(r)]);
            if (itemIndex < 0 && !in.RecentsShowUnknown) {
                continue;
            }
            Row row;
            row.Kind = RowKind::Item;
            row.ItemIndex = itemIndex;
            row.RecentIndex = r;
            out.push_back(row);
        }
        if (out.size() > headerAt) {
            Row header;
            header.Kind = RowKind::HeaderRecent;
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(headerAt), header);
            Row all;
            all.Kind = RowKind::HeaderAll;
            out.push_back(all);
        }
    }
    for (int i = 0; i < count; ++i) {
        Row row;
        row.Kind = RowKind::Item;
        row.ItemIndex = i;
        out.push_back(row);
    }
}

/// A row the highlight may rest on and Enter may act on: not a header, not a disabled option.
inline bool IsSelectableRow(const Row& row, const ItemView* items, int count) {
    if (row.Kind == RowKind::HeaderRecent || row.Kind == RowKind::HeaderAll) {
        return false;
    }
    if (row.Kind == RowKind::Item && row.ItemIndex >= 0 && row.ItemIndex < count) {
        return !items[row.ItemIndex].Disabled;
    }
    return true;
}

/// Moves the highlight `delta` selectable rows (negative = up), stopping at the first / last selectable
/// row. From no highlight (-1) a move lands on the first selectable row going down, the last going up.
/// Returns -1 only when no row is selectable.
inline int MoveHighlight(const std::vector<Row>& rows, const ItemView* items, int count, int current, int delta) {
    const int n = static_cast<int>(rows.size());
    int first = -1;
    int last = -1;
    for (int i = 0; i < n; ++i) {
        if (IsSelectableRow(rows[static_cast<std::size_t>(i)], items, count)) {
            if (first < 0) {
                first = i;
            }
            last = i;
        }
    }
    if (first < 0) {
        return -1;
    }
    if (current < 0 || current >= n) {
        return delta >= 0 ? first : last;
    }
    int pos = current;
    const int step = delta >= 0 ? 1 : -1;
    int remaining = delta >= 0 ? delta : -delta;
    while (remaining > 0) {
        int next = pos + step;
        while (next >= 0 && next < n && !IsSelectableRow(rows[static_cast<std::size_t>(next)], items, count)) {
            next += step;
        }
        if (next < 0 || next >= n) {
            break;
        }
        pos = next;
        --remaining;
    }
    if (!IsSelectableRow(rows[static_cast<std::size_t>(pos)], items, count)) {
        return delta >= 0 ? first : last;
    }
    return pos;
}

/// Where the highlight starts for a freshly built row list.
///   No filter: the checked option in the main list (skipping its copy in the Recent section) when
///   `preferChecked`, else the first option row.
///   Filter: the best match (MatchRank; the first one among equals), so Enter acts on the option whose
///   label answers the filter and never on a ticked option that is only listed because
///   KeepCheckedVisible keeps it; with no match, the Create row; with neither, the first option row.
/// Never the clear row: Enter in a picker whose options have not arrived must not clear the field.
inline int DefaultHighlight(const std::vector<Row>& rows, const ItemView* items, int count, bool preferChecked,
                            const std::string& filterLower) {
    const int n = static_cast<int>(rows.size());
    if (preferChecked) {
        for (int i = 0; i < n; ++i) {
            const Row& row = rows[static_cast<std::size_t>(i)];
            if (row.Kind == RowKind::Item && row.RecentIndex < 0 && row.ItemIndex >= 0 && row.ItemIndex < count &&
                items[row.ItemIndex].Checked && !items[row.ItemIndex].Disabled) {
                return i;
            }
        }
    }
    int firstOption = -1;
    int createRow = -1;
    int bestMatch = -1;
    int bestRank = 3;
    for (int i = 0; i < n && bestRank > 0; ++i) {
        const Row& row = rows[static_cast<std::size_t>(i)];
        if (row.Kind == RowKind::Create) {
            createRow = i;
            continue;
        }
        if (row.Kind != RowKind::Item || !IsSelectableRow(row, items, count)) {
            continue;
        }
        const bool known = row.ItemIndex >= 0 && row.ItemIndex < count;
        if (!known || filterLower.empty()) {
            return i;
        }
        if (!ItemMatches(items[row.ItemIndex], filterLower)) {
            if (firstOption < 0) {
                firstOption = i;
            }
            continue;
        }
        const int rank = MatchRank(items[row.ItemIndex], filterLower);
        if (rank < bestRank) {
            bestRank = rank;
            bestMatch = i;
        }
    }
    if (bestMatch >= 0) {
        return bestMatch;
    }
    return createRow >= 0 ? createRow : firstOption;
}

/// True if the list holds at least one option or create row (a lone clear row does not count): when it
/// does not, the picker shows its empty-state text.
inline bool HasOptionRows(const std::vector<Row>& rows) {
    for (const Row& row : rows) {
        if (row.Kind == RowKind::Item || row.Kind == RowKind::Create) {
            return true;
        }
    }
    return false;
}

/// What a row is, apart from where it sits, so a highlight can follow its row when the option list is
/// rebuilt under an open popup (a fetch landing, live status transitions replacing the learned list).
struct RowIdentity {
    bool Valid = false;
    RowKind Kind = RowKind::Item;
    bool InRecent = false; // the copy in the Recent section, not the one in the full list
    std::string Id;        // option id (or the recent id for a recent row outside the options)
    std::string Label;     // option label: tells apart two options that share an id
};

inline const char* RowLabel(const Row& row, const ItemView* items, int count) {
    const bool known = row.Kind == RowKind::Item && row.ItemIndex >= 0 && row.ItemIndex < count;
    return known && items[row.ItemIndex].Label != nullptr ? items[row.ItemIndex].Label : "";
}

inline const char* RowId(const Row& row, const ItemView* items, int count, const std::vector<std::string>* recents) {
    if (row.Kind != RowKind::Item) {
        return "";
    }
    if (row.ItemIndex >= 0 && row.ItemIndex < count) {
        return items[row.ItemIndex].Id != nullptr ? items[row.ItemIndex].Id : "";
    }
    if (recents != nullptr && row.RecentIndex >= 0 && row.RecentIndex < static_cast<int>(recents->size())) {
        return (*recents)[static_cast<std::size_t>(row.RecentIndex)].c_str();
    }
    return "";
}

/// Fills `out` with the identity of row `index` (not Valid when `index` is no row). Reuses out's strings.
inline void IdentityOfRow(const std::vector<Row>& rows, const ItemView* items, int count,
                          const std::vector<std::string>* recents, int index, RowIdentity& out) {
    out.Valid = index >= 0 && index < static_cast<int>(rows.size());
    if (!out.Valid) {
        out.Id.clear();
        out.Label.clear();
        return;
    }
    const Row& row = rows[static_cast<std::size_t>(index)];
    out.Kind = row.Kind;
    out.InRecent = row.Kind == RowKind::Item && row.RecentIndex >= 0;
    out.Id = RowId(row, items, count, recents);
    out.Label = RowLabel(row, items, count);
}

inline bool RowHasIdentity(const Row& row, const ItemView* items, int count, const std::vector<std::string>* recents,
                           const RowIdentity& identity) {
    if (row.Kind != identity.Kind) {
        return false;
    }
    if (row.Kind != RowKind::Item) {
        return true;
    }
    return (row.RecentIndex >= 0) == identity.InRecent && identity.Id == RowId(row, items, count, recents) &&
           identity.Label == RowLabel(row, items, count);
}

/// The index of the first row with that identity, or -1 when the list no longer has it.
inline int FindRowByIdentity(const std::vector<Row>& rows, const ItemView* items, int count,
                             const std::vector<std::string>* recents, const RowIdentity& identity) {
    if (!identity.Valid) {
        return -1;
    }
    const int n = static_cast<int>(rows.size());
    for (int i = 0; i < n; ++i) {
        if (RowHasIdentity(rows[static_cast<std::size_t>(i)], items, count, recents, identity)) {
            return i;
        }
    }
    return -1;
}

/// Where a highlight that was on `identity` at row `current` belongs in a rebuilt list: `current` when
/// that row still is the same thing (so two options sharing an id do not pull the highlight back to the
/// first of them), else the first row with that identity, else -1.
inline int ResolveHighlight(const std::vector<Row>& rows, const ItemView* items, int count,
                            const std::vector<std::string>* recents, const RowIdentity& identity, int current) {
    if (!identity.Valid) {
        return -1;
    }
    if (current >= 0 && current < static_cast<int>(rows.size()) &&
        RowHasIdentity(rows[static_cast<std::size_t>(current)], items, count, recents, identity)) {
        return current;
    }
    return FindRowByIdentity(rows, items, count, recents, identity);
}

/// The scroll offset that brings row `index` fully into a viewport of `viewHeight` showing rows of
/// `rowHeight`, changing `scrollY` as little as possible.
inline float ScrollToRevealRow(int index, float rowHeight, float viewHeight, float scrollY) {
    if (index < 0 || rowHeight <= 0.0f) {
        return scrollY;
    }
    const float top = static_cast<float>(index) * rowHeight;
    const float bottom = top + rowHeight;
    if (top < scrollY) {
        return top;
    }
    if (bottom > scrollY + viewHeight) {
        return bottom - viewHeight;
    }
    return scrollY;
}

} // namespace pure
} // namespace dropdown
} // namespace smatchet
