#pragma once

#include <cstddef>

// Settings-descriptor table for the Preferences window — the single source of
// truth for its taxonomy (categories → sections → settings) and for the global
// settings search. Pure data: no ImGui, no ConfigManager, no session state, so
// the whole surface is bucket-A testable.
//
// docs/plans/active/preferences-ia-resegmentation-and-search.md.
//
// Contract with the draw code (enforced by the bucket-E drift guard,
// prefs_schema_coverage):
//   - every descriptor id is passed to PreferencesFilter::ShowSetting by exactly
//     one always-drawn widget — ConditionalDraw rows may legitimately not draw.
//   - LabelEn is the VERBATIM English source string the widget renders — it is
//     both the search haystack seed and the drift guard's label check.
//   - feature-gated rows sit inside the same #if guards as their draw code, so
//     a feature-OFF build never carries a descriptor its UI cannot draw.

namespace SmatchetPrefsSchema {

/// Per-SECTION save semantics. Replaces the per-tab footer switch in
/// drawPreferencesWindow — a category can host sections with different save
/// paths (Connections mixes MarkPrefsDirty with Annotate's detached persist),
/// so the hint must render per section, not per category.
enum class SaveSemantics {
    SaveAndSync,      ///< Staged in buffers; committed by the footer "Save & Sync"
                      ///< button (Tracker credentials).
    Autosave,         ///< MarkPrefsDirty on change; debounced write-back.
    Immediate,        ///< Applied and persisted the frame the widget changes.
    Restart,          ///< Persisted immediately; takes effect on next launch.
    ExplicitSave,     ///< Section-local Save / Discard staging over a working copy
                      ///< (Assistant + Agent context — CopyAssistantAiFields).
                      ///< Sixth value beyond the plan's five: that staging flow
                      ///< maps to none of them (deviation noted in the plan doc).
    AnnotateDetached, ///< Persisted through the detached Annotate config path
                      ///< (PersistAnnotateCfg), outside MarkPrefsDirty entirely.
};

struct PrefsCategoryDesc {
    const char* Id;      ///< Stable dotted-id root, e.g. "general".
    const char* TitleEn; ///< English source string; display via TranslateSource.
};

struct PrefsSectionDesc {
    const char* Id;         ///< "general.updates" — always "<CategoryId>.<leaf>".
    const char* CategoryId; ///< Must name a row in Categories().
    const char* TitleEn;    ///< English source string; display via TranslateSource.
    SaveSemantics Save;
};

struct PrefsSettingDesc {
    const char* Id;        ///< "general.updates.check_enabled" — "<SectionId>.<leaf>".
    const char* SectionId; ///< Must name a row in Sections().
    const char* LabelEn;   ///< Verbatim rendered English label. For widgets drawn via
                           ///< explicit T(key, fallback) the fallback IS this string.
                           ///< Truncated at the first "##" when indexed for search.
    const char* Keywords;  ///< Extra lowercase search terms, space-separated. Seeded
                           ///< with the pre-resegmentation tab name so muscle memory
                           ///< still finds relocated settings.
    /// True when the row may legitimately not draw on a given frame (per-backend /
    /// per-provider switching, value-dependent rows). Relaxes only the
    /// "every descriptor was observed" direction of the drift guard — an observed
    /// id must ALWAYS be a descriptor. Each true row carries a comment naming the
    /// condition that gates it.
    bool ConditionalDraw = false;
};

/// Table accessors — pointers into static storage, valid for the process lifetime.
const PrefsCategoryDesc* Categories(std::size_t& count);
const PrefsSectionDesc* Sections(std::size_t& count);
const PrefsSettingDesc* Settings(std::size_t& count);

/// Linear-scan lookups; return nullptr when the id is unknown.
const PrefsCategoryDesc* FindCategory(const char* id);
const PrefsSectionDesc* FindSection(const char* id);
const PrefsSettingDesc* FindSetting(const char* id);

} // namespace SmatchetPrefsSchema
