#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Live search filter for the Preferences window. Matches the query against
// every descriptor in PreferencesSchema (translated label + keywords +
// translated section/category titles) and answers per-setting "should this
// widget draw?" queries. No ImGui — bucket-A testable.
//
// docs/plans/active/preferences-ia-resegmentation-and-search.md.
//
// Matching: tier 1 is case-insensitive substring; tier 2 (fuzzy subsequence via
// smatchet::cmd::FuzzyScore) runs only when tier 1 matched nothing, so exact
// text always wins and fuzzy noise never dilutes a good substring hit.
//
// The observed-id set exists for the bucket-E drift guard: the guard walks all
// categories with an empty query and diffs ObservedSettingIds() against the
// descriptor table.

class PreferencesFilter {
  public:
    // Shared ASCII helpers, absorbed from SmatchetPreferencesUi_Keybindings.cpp
    // (same semantics; the keybindings TU switches to these when slice 3 wires
    // the global query into its row filter — Pillar 5).
    static std::string ToLowerAscii(const std::string& s);
    /// True when `needleLower` (already lowercased) occurs in `haystack`.
    static bool ContainsLower(const std::string& haystack, const std::string& needleLower);

    /// Refresh for this frame. Rebuilds the haystack index when `uiLanguage`
    /// changed since the last call (labels are indexed *translated*) and
    /// recomputes matches when `query` changed. Cheap no-op otherwise.
    /// `query` may be nullptr (treated as empty).
    void Update(const char* query, const std::string& uiLanguage);

    /// True while a non-empty query is filtering.
    bool Active() const { return !queryLower_.empty(); }

    /// True when the most recent Update() saw a different query string than the
    /// call before it. The nav rail uses this as the edge to re-home the
    /// selection on the first matching category: reacting to the edge rather
    /// than to Active() lets the user click a different category mid-query
    /// without being yanked back on the next frame.
    bool QueryChangedThisUpdate() const { return queryChanged_; }

    /// True when the setting should draw. Always true on an empty query (no
    /// lookup on the non-searching hot path). Unknown ids fail OPEN — a widget
    /// whose id is missing from the schema must stay visible, and the drift
    /// guard (not the filter) reports the mismatch. Records `id` in the observed
    /// set (either way) only while recording is enabled — see
    /// SetRecordObservedSettingIds.
    bool ShowSetting(const char* id);

    /// Fold a match the schema cannot see into this frame's result set. The
    /// keybindings table is dynamic rows with no descriptors, so its "does the
    /// query hit a command?" answer only exists at draw time; without folding it
    /// back in, the rail would disable the Shortcuts category and the auto-switch
    /// would skip past it while the section below happily listed hits. Idempotent;
    /// cleared by the next query change. `settingId` must be a real descriptor id
    /// (it stands in for the dynamic content), otherwise this is a no-op.
    void AddDynamicMatch(const char* settingId);

    /// Matched / total descriptor counts for the "(showing N of M)" readout.
    /// MatchCount() is 0 while inactive.
    std::size_t MatchCount() const { return matched_.size(); }
    std::size_t TotalCount() const;

    /// Section/category roll-ups for auto-expand and rail badges/dimming.
    /// While inactive: sections/categories all "have matches" (nothing hidden).
    bool SectionHasMatch(const char* sectionId) const;
    bool CategoryHasMatch(const char* categoryId) const;
    std::size_t CategoryMatchCount(const char* categoryId) const;

    /// Drift-guard support: every id ever passed to ShowSetting since the last
    /// reset (accumulates across frames; the guard resets explicitly).
    /// Recording is OFF by default and only the drift guard turns it on: the
    /// bookkeeping is a std::string node per drawn setting per frame (~120 of
    /// them), which is real allocator traffic on the steady-state path where the
    /// user is not searching at all (Pillar 1).
    void SetRecordObservedSettingIds(bool enabled) { recordObserved_ = enabled; }
    const std::set<std::string>& ObservedSettingIds() const { return observed_; }
    void ResetObservedSettingIds() { observed_.clear(); }

  private:
    struct Entry {
        const char* Id;
        const char* SectionId;
        const char* CategoryId;
        std::string Haystack; // translated label (##-truncated) + keywords + titles
    };

    void RebuildIndex();
    void RecomputeMatches();

    std::string queryRaw_;
    std::string queryLower_;
    std::string indexedLanguage_;
    bool indexBuilt_ = false;
    bool queryChanged_ = false;
    bool recordObserved_ = false;
    std::vector<Entry> index_;
    std::unordered_set<std::string> matched_;
    std::unordered_set<std::string> sectionMatches_;
    std::unordered_map<std::string, std::size_t> categoryMatches_;
    std::set<std::string> observed_;
};
