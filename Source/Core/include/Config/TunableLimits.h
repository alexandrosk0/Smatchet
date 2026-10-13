#pragma once

// Limits — user-tunable caps, timeouts, and cache sizes that used to be scattered `constexpr`
// literals across Source/Core. One table is the single source of truth for: the struct field and
// its default, the JSON key persisted under the config's top-level "limits" object, and the
// load-time clamp range. Header-only and nlohmann-free so pure helpers and plugins can read it
// without linking ConfigManager.
//
// Access: limits::Get() returns a by-value snapshot of every field (thread-safe; ~70 ints, cheap).
// ConfigManager::Load() publishes the loaded values via `Set()` on a cache miss, so a hand-edited config.json takes
// effect on the next launch. Safety bounds that are NOT preferences (recursion depth guards, parser buffer ceilings,
// wire-protocol sizes) intentionally stay `constexpr`.
//
// Unit conventions: `*Ms` milliseconds, `*MiB` mebibytes (use `limits::MiB()` for bytes), the
// rest are plain counts. Every value is an `int`; call sites cast to the type they need.
//
// Adding a row: append one X(...) line below; the struct, JSON round-trip, clamp, and the
// defaults/clamp unit test pick it up automatically. Do not renumber or rename existing keys —
// they are persisted.

#include <cstddef>
#include <mutex>

namespace smatchet {
namespace limits {

// X(Field, "json_key", default, min, max)
#define SMATCHET_LIMITS_TABLE(X)                                                                                       \
    /* ---- Tier A: paginated search fetch guards (SearchFetchGuards.h) ---- */                                        \
    X(JiraSearchMaxPages, "jira_search_max_pages", 50, 1, 1000)                                                        \
    X(JiraSearchMaxFetchMiB, "jira_search_max_fetch_mib", 100, 1, 1024)                                                \
    X(JiraSearchMaxResults, "jira_search_max_results", 500, 1, 100000)                                                 \
    X(PlaneSearchMaxPages, "plane_search_max_pages", 50, 1, 1000)                                                      \
    X(PlaneSearchMaxFetchMiB, "plane_search_max_fetch_mib", 100, 1, 1024)                                              \
    X(PlaneSearchMaxResults, "plane_search_max_results", 500, 1, 100000)                                               \
    X(GitHubSearchMaxPages, "github_search_max_pages", 10, 1, 10)                                                      \
    X(GitHubSearchMaxFetchMiB, "github_search_max_fetch_mib", 50, 1, 1024)                                             \
    X(GitHubSearchMaxResults, "github_search_max_results", 500, 1, 1000)                                               \
    /* ---- Tier A: other pagination ceilings ---- */                                                                  \
    X(GitHubCommentsMaxPages, "github_comments_max_pages", 50, 1, 1000)                                                \
    X(GitHubMilestonesMaxPages, "github_milestones_max_pages", 10, 1, 1000)                                            \
    X(GitHubReposMaxPages, "github_repos_max_pages", 10, 1, 1000)                                                      \
    X(GitHubProjectsV2MaxPages, "github_projects_v2_max_pages", 5, 1, 1000)                                            \
                                                                                                                       \
    X(GitHubActivityMaxPages, "github_activity_max_pages", 10, 1, 1000)                                                \
    X(GitHubActivityMaxTeamChecks, "github_activity_max_team_checks", 50, 1, 1000)                                     \
    X(JiraActivityMaxPages, "jira_activity_max_pages", 20, 1, 1000)                                                    \
    X(PlaneActivityMaxPages, "plane_activity_max_pages", 5, 1, 1000)                                                   \
    X(PlaneDiscoveryMaxPages, "plane_discovery_max_pages", 10, 1, 1000)                                                \
    X(LinearMaxPages, "linear_max_pages", 20, 1, 1000)                                                                 \
    X(FieldCatalogMaxPages, "field_catalog_max_pages", 50, 1, 1000)                                                    \
    X(FieldCatalogMaxBoardPages, "field_catalog_max_board_pages", 20, 1, 1000)                                         \
    X(JiraUsersMaxPages, "jira_users_max_pages", 50, 1, 1000)                                                          \
    X(JiraMembersMaxPages, "jira_members_max_pages", 20, 1, 1000)                                                      \
    /* ---- Tier A: tracker HTTP timeouts + retry ---- */                                                              \
    X(TrackerConnectTimeoutMs, "tracker_connect_timeout_ms", 5000, 100, 120000)                                        \
    X(TrackerOverallTimeoutMs, "tracker_overall_timeout_ms", 30000, 1000, 600000)                                      \
    X(TrackerProbeConnectTimeoutMs, "tracker_probe_connect_timeout_ms", 2000, 100, 60000)                              \
    X(TrackerProbeOverallTimeoutMs, "tracker_probe_overall_timeout_ms", 5000, 500, 120000)                             \
    X(TrackerHttpMaxAttempts, "tracker_http_max_attempts", 3, 1, 10)                                                   \
    X(TrackerHttpBaseRetryDelayMs, "tracker_http_base_retry_delay_ms", 250, 10, 10000)                                 \
    X(TrackerHttpMaxRetryDelayMs, "tracker_http_max_retry_delay_ms", 4000, 100, 120000)                                \
    X(TrackerHttpMaxRetryAfterHonorMs, "tracker_http_max_retry_after_honor_ms", 30000, 1000, 600000)                   \
    /* ---- Tier A: Perforce + attachment transfer ---- */                                                             \
    X(P4ProcessTimeoutMs, "p4_process_timeout_ms", 120000, 1000, 3600000)                                              \
    X(P4CaptureMaxMiB, "p4_capture_max_mib", 4, 1, 256)                                                                \
    X(AttachmentDownloadMaxMiB, "attachment_download_max_mib", 50, 1, 2048)                                            \
    X(AttachmentDownloadTimeoutMs, "attachment_download_timeout_ms", 120000, 5000, 3600000)                            \
    /* ---- Tier B: cache + history sizes ---- */                                                                      \
    X(ImageTextureCacheMaxEntries, "image_texture_cache_max_entries", 384, 16, 10000)                                  \
    X(ImageTextureCacheMaxMiB, "image_texture_cache_max_mib", 96, 8, 4096)                                             \
    X(LookupMaxStoredUsers, "lookup_max_stored_users", 50000, 100, 1000000)                                            \
    X(LookupMaxStoredProjects, "lookup_max_stored_projects", 10000, 100, 1000000)                                      \
    X(FieldIconPathCacheMax, "field_icon_path_cache_max", 256, 16, 100000)                                             \
    X(FieldIconFetchNegativeMax, "field_icon_fetch_negative_max", 256, 16, 100000)                                     \
    X(PriorityResolveMemoMax, "priority_resolve_memo_max", 256, 16, 100000)                                            \
    X(GridRenderCacheMax, "grid_render_cache_max", 256, 16, 100000)                                                    \
    X(CodeColorCacheMax, "code_color_cache_max", 256, 16, 100000)                                                      \
    X(AiMaxCachedPlans, "ai_max_cached_plans", 256, 8, 10000)                                                          \
    X(AiPlanCacheMaxMiB, "ai_plan_cache_max_mib", 4, 1, 256)                                                           \
    X(NavigationHistoryMax, "navigation_history_max", 200, 10, 10000)                                                  \
    X(ToastHistoryMax, "toast_history_max", 200, 10, 10000)                                                            \
    X(ToastMaxLive, "toast_max_live", 6, 1, 50)                                                                        \
    X(LogBufferMaxEntries, "log_buffer_max_entries", 1000, 100, 100000)                                                \
    X(CommandRecentsMax, "command_recents_max", 16, 1, 200)                                                            \
    X(McpActivityLogMax, "mcp_activity_log_max", 100, 10, 10000)                                                       \
    /* ---- Tier C: UI + feature row limits ---- */                                                                    \
    X(AutocompleteMaxResolved, "autocomplete_max_resolved", 200, 10, 5000)                                             \
    X(AutocompleteMaxAsyncRows, "autocomplete_max_async_rows", 40, 5, 1000)                                            \
    X(AutocompleteMaxMerged, "autocomplete_max_merged", 120, 10, 5000)                                                 \
    X(JqlSuggestMaxUsers, "jql_suggest_max_users", 50, 5, 1000)                                                        \
    X(QuerySuggestMax, "query_suggest_max", 80, 5, 1000)                                                               \
    X(AiContextRowsMax, "ai_context_rows_max", 50, 1, 1000)                                                            \
    X(AiContextAuditMax, "ai_context_audit_max", 20, 1, 1000)                                                          \
    X(LogLinesDrawnMax, "log_lines_drawn_max", 8000, 100, 1000000)                                                     \
    X(AiPinnedRowsVisibleMax, "ai_pinned_rows_visible_max", 4, 1, 50)                                                  \
    X(GridTooltipMaxEntries, "grid_tooltip_max_entries", 12, 1, 200)

struct LimitsConfig {
#define SMATCHET_LIMITS_FIELD(name, key, def, lo, hi) int name = def;
    SMATCHET_LIMITS_TABLE(SMATCHET_LIMITS_FIELD)
#undef SMATCHET_LIMITS_FIELD
};

/// One row of the table, for the JSON load/save loop and the unit test.
struct LimitDescriptor {
    const char* key;
    int LimitsConfig::* member;
    int defaultValue;
    int minValue;
    int maxValue;
};

/// The full descriptor table; `outCount` receives its length.
inline const LimitDescriptor* Descriptors(std::size_t& outCount) {
    static const LimitDescriptor kRows[] = {
#define SMATCHET_LIMITS_ROW(name, key, def, lo, hi) {key, &LimitsConfig::name, def, lo, hi},
        SMATCHET_LIMITS_TABLE(SMATCHET_LIMITS_ROW)
#undef SMATCHET_LIMITS_ROW
    };
    outCount = sizeof(kRows) / sizeof(kRows[0]);
    return kRows;
}

/// Clamp every field into its [min, max] range (load-time repair of hand-edited configs).
inline void ClampAll(LimitsConfig& cfg) {
    std::size_t n = 0;
    const LimitDescriptor* rows = Descriptors(n);
    for (std::size_t i = 0; i < n; ++i) {
        int& v = cfg.*(rows[i].member);
        if (v < rows[i].minValue) {
            v = rows[i].minValue;
        } else if (v > rows[i].maxValue) {
            v = rows[i].maxValue;
        }
    }
}

namespace detail {
struct State {
    std::mutex mutex;
    LimitsConfig current;
};
inline State& GetState() {
    // Deliberately leaked: Logger::Log reads Get() and may run during static destruction, after a
    // function-local static here would already be gone.
    // SMATCHET_DEVIATION(rule=no-raw-new; reason=leaked singleton must outlive Logger at static destruction; owner=config; revisit=2027-10-13)
    static State* const s = new State;
    return *s;
}
} // namespace detail

/// Snapshot of the currently published limits (defaults until the first Set()).
inline LimitsConfig Get() {
    detail::State& s = detail::GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.current;
}

/// Publish new limits (clamped). Called by ConfigManager::Load().
inline void Set(const LimitsConfig& next) {
    LimitsConfig clamped = next;
    ClampAll(clamped);
    detail::State& s = detail::GetState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.current = clamped;
}

/// Mebibytes -> bytes without int overflow.
inline std::size_t MiB(int mib) { return static_cast<std::size_t>(mib) * 1024u * 1024u; }

} // namespace limits
} // namespace smatchet
