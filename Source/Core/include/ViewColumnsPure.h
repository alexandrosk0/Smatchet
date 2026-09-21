#pragma once

// Pure (ImGui-free) helpers around ViewDefinition::Columns — the single ordered source of
// truth for which grid columns exist and where. Replaces the former ColumnOrder + ColumnWidths
// + Fields triple: Fields is now DERIVED from Columns (FieldIdsFromColumns), never assigned
// directly, so the "Fields re-sorted alphabetically on save reshuffles the column tail" and
// "column reorder drops to the end because ColumnOrder went stale" failure classes become
// structurally unrepresentable rather than merely discouraged. See docs/plans/active/
// column-view-save-simplification.md for the full root-cause writeup this header fixes.
//
// Bucket-A testable (tests/Core/ViewColumnsPure.test.cpp) — no ImGui, no session state.

#include "Config/ConfigManager.h"
#include "StringUtil.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ViewColumn itself is declared in Config/ConfigManager.h (so ViewDefinition can hold a
// std::vector<ViewColumn> member without every TU that touches ViewDefinition pulling this
// header) — Width == 0.0f there means "unset, fall back to DefaultColumnWidthPx below";
// NormalizeViewDefinition always fills it, so a normalized view never carries a zero width.

/// The one width every column falls back to when it has no explicit stored value: 90px for
/// the synthetic "id" column, 180px for everything else. Single definition shared by the grid
/// setup (what width to request), the writeback (what "default" means for the compare), and
/// NormalizeViewDefinition (what to fill a zero width with) — previously three separate
/// 90.0f/180.0f literals that could (and did) drift.
inline float DefaultColumnWidthPx(const std::string& key) { return key == "id" ? 90.0f : 180.0f; }

/// The width a column would render at: its stored value if present and positive, else the
/// kind default. Missing-key and explicit-default-value compare equal — this is what makes a
/// fresh view (only "id" has a stored width) read as NOT dirty on first render.
inline float EffectiveColumnWidth(const ViewDefinition& view, const std::string& key) {
    for (const auto& col : view.Columns) {
        if (col.Key == key) {
            return col.Width > 0.0f ? col.Width : DefaultColumnWidthPx(key);
        }
    }
    return DefaultColumnWidthPx(key);
}

/// True when a captured width differs from what we asked ImGui to render meaningfully enough
/// to persist. `requested` is the width the grid setup pushed via TableSetupColumn this frame;
/// `given` is ImGui's WidthGiven read back after layout. On an untouched column these are
/// equal (or within layout-clamp noise), so this returns false on every frame with no user
/// interaction — the direct fix for the phantom "unsaved changes" strip on launch.
inline bool ShouldCaptureColumnWidth(float requested, float given, float tolPx) {
    return std::fabs(requested - given) > tolPx;
}

/// Column ids (the "field:<id>" part, in order, "id" excluded) derived from an ordered column
/// list — this is how ViewDefinition::Fields is produced now. Anything not "field:"-prefixed
/// (only "id" should ever reach here from a normalized Columns list) is skipped defensively.
inline std::vector<std::string> FieldIdsFromColumns(const std::vector<ViewColumn>& columns) {
    std::vector<std::string> ids;
    ids.reserve(columns.size());
    for (const auto& col : columns) {
        if (col.Key.compare(0, 6, "field:") == 0) {
            ids.push_back(col.Key.substr(6));
        }
    }
    return ids;
}

/// Rebuild an ordered column list from the pre-Columns on-disk shape (legacy `fields` +
/// `column_order` + `column_widths`), reproducing TicketGridColumnsBuilder::Build's ordering
/// EXACTLY: `columnOrder` keys first (canonicalized, deduped, dropping anything that doesn't
/// name a real field), then every remaining `fields`-derived key appended in `fields` order.
/// This is what makes the v2->v3 migration a no-op for every existing view on disk — nothing
/// moves on upgrade unless it was already reshuffled by the alphabetical-Fields save bug, in
/// which case this restores it to columnOrder's order (the last-known-good visual layout).
inline std::vector<ViewColumn> MigrateLegacyColumns(const std::vector<std::string>& fields,
                                                     const std::vector<std::string>& columnOrder,
                                                     const std::unordered_map<std::string, float>& widths) {
    std::vector<std::string> allKeys;
    allKeys.push_back("id");
    std::unordered_set<std::string> seenFieldIds;
    for (const auto& raw : fields) {
        const std::string id = CanonicalizeGridFieldId(TrimCopyAsciiWhitespace(raw));
        if (id.empty() || !seenFieldIds.insert(id).second) {
            continue;
        }
        allKeys.push_back("field:" + id);
    }
    const std::unordered_set<std::string> validKeys(allKeys.begin(), allKeys.end());

    std::vector<std::string> ordered;
    std::unordered_set<std::string> used;
    for (const auto& raw : columnOrder) {
        const std::string key = CanonicalGridColumnKey(raw);
        if (validKeys.find(key) == validKeys.end() || !used.insert(key).second) {
            continue;
        }
        ordered.push_back(key);
    }
    for (const auto& key : allKeys) {
        if (used.insert(key).second) {
            ordered.push_back(key);
        }
    }

    std::vector<ViewColumn> result;
    result.reserve(ordered.size());
    for (const auto& key : ordered) {
        const auto it = widths.find(key);
        result.push_back({key, it != widths.end() ? it->second : 0.0f});
    }
    return result;
}

/// Canonicalize + dedupe `view.Columns` (keeping first occurrence), guarantee `"id"` is
/// present (prepended if absent), fill every zero/negative width from DefaultColumnWidthPx,
/// regenerate `view.Fields` from the result, and prune `view.SortSpecs` entries whose key is
/// no longer a column. Idempotent: calling it twice in a row is a no-op the second time. This
/// is both the load-time migration step and the general "make a ViewDefinition internally
/// consistent" call — run it after any edit that touches Columns/Fields/SortSpecs.
inline void NormalizeViewDefinition(ViewDefinition& view) {
    std::vector<ViewColumn> normalized;
    std::unordered_set<std::string> seen;
    normalized.reserve(view.Columns.size() + 1);
    for (const auto& col : view.Columns) {
        const std::string key = CanonicalGridColumnKey(col.Key);
        if (key.empty() || !seen.insert(key).second) {
            continue;
        }
        const float width = col.Width > 0.0f ? col.Width : DefaultColumnWidthPx(key);
        normalized.push_back({key, width});
    }
    const bool hasId = std::any_of(normalized.begin(), normalized.end(),
                                   [](const ViewColumn& c) { return c.Key == "id"; });
    if (!hasId) {
        normalized.insert(normalized.begin(), ViewColumn{"id", DefaultColumnWidthPx("id")});
    }
    view.Columns = std::move(normalized);
    view.Fields = FieldIdsFromColumns(view.Columns);

    std::unordered_set<std::string> keySet;
    keySet.reserve(view.Columns.size());
    for (const auto& col : view.Columns) {
        keySet.insert(col.Key);
    }
    view.SortSpecs.erase(std::remove_if(view.SortSpecs.begin(), view.SortSpecs.end(),
                                        [&](const ViewSortSpec& s) { return keySet.find(s.ColumnKey) == keySet.end(); }),
                         view.SortSpecs.end());
}

/// Reorder `view.Columns` to `order` (a list of keys, canonicalized on the way in), preserving
/// each column's width. A key in `order` that does not name an existing column is dropped and
/// returned in the result so a caller (the `view.set_column_order` command) can report it as
/// ignored rather than silently swallowing a typo. A column NOT named in `order` keeps its
/// relative position and is appended after the ordered ones — reordering must never silently
/// remove a column, matching TicketGridColumnsBuilder's own tail-append semantics. Ends by
/// running NormalizeViewDefinition, so Fields/SortSpecs stay in lock-step. Used by both the
/// header-drag writeback (where `order` is already a complete permutation, so nothing is
/// ignored or tail-appended) and the column-order command (where it may be partial).
inline std::vector<std::string> ReorderViewColumns(const std::vector<std::string>& order, ViewDefinition& view) {
    std::unordered_map<std::string, ViewColumn> byKey;
    byKey.reserve(view.Columns.size());
    for (const auto& col : view.Columns) {
        byKey[col.Key] = col;
    }

    std::vector<ViewColumn> reordered;
    std::vector<std::string> ignored;
    std::unordered_set<std::string> used;
    reordered.reserve(view.Columns.size());
    for (const auto& raw : order) {
        const std::string key = CanonicalGridColumnKey(raw);
        const auto it = byKey.find(key);
        if (it == byKey.end()) {
            ignored.push_back(raw);
            continue;
        }
        if (!used.insert(key).second) {
            continue;
        }
        reordered.push_back(it->second);
    }
    for (const auto& col : view.Columns) {
        if (used.insert(col.Key).second) {
            reordered.push_back(col);
        }
    }

    view.Columns = std::move(reordered);
    NormalizeViewDefinition(view);
    return ignored;
}

/// True when `draft` has diverged from `saved` in any way the Views editor (or the grid's
/// remaining query-only strip) should surface as "unsaved" — name, JQL, parent-hierarchy
/// flags, sort specs (exact), the column KEY SEQUENCE (order + membership), and each column's
/// EffectiveColumnWidth within `widthTolPx`. A width stored under a key absent from the
/// current column list can never contribute (EffectiveColumnWidth is only ever asked about
/// keys that are in both sequences, since the key-sequence check already gates on that).
inline bool ViewDraftDiffersFromSaved(const ViewDefinition& draft, const ViewDefinition& saved, float widthTolPx) {
    if (draft.Name != saved.Name || draft.Jql != saved.Jql || draft.HideParents != saved.HideParents ||
        draft.StoryGroupSort != saved.StoryGroupSort) {
        return true;
    }
    if (draft.SortSpecs != saved.SortSpecs) {
        return true;
    }
    if (draft.Columns.size() != saved.Columns.size()) {
        return true;
    }
    for (std::size_t i = 0; i < draft.Columns.size(); ++i) {
        if (draft.Columns[i].Key != saved.Columns[i].Key) {
            return true;
        }
        if (ShouldCaptureColumnWidth(EffectiveColumnWidth(saved, draft.Columns[i].Key),
                                     EffectiveColumnWidth(draft, draft.Columns[i].Key), widthTolPx)) {
            return true;
        }
    }
    return false;
}
