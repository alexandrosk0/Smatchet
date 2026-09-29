#pragma once

#include "PendingActionTypes.h"
#include "TrackerError.h"
#include "TrackerFieldSchema.h"

#include <cstdint>
#include <future>
#include <map>
#include <set>
#include <string>
#include <vector>

class AppController;

struct WatchersLoadResult {
    bool Ok = false;
    std::vector<TrackerUser> Watchers;
    std::string Error;
    TrackerErrorKind ErrorKind = TrackerErrorKind::None; ///< Transport when the tracker was unreachable
};

struct VotesLoadResult {
    bool Ok = false;
    std::vector<TrackerUser> Voters;
    std::string Error;
    TrackerErrorKind ErrorKind = TrackerErrorKind::None; ///< Transport when the tracker was unreachable
    int VoteCount = 0;
    bool HasVoted = false;
    /** True when JSON contained a `voters` array (may be empty); false when key absent (often permissions). */
    bool VotersArrayInResponse = false;
};

/// What a watchers / votes window shows (Quality Pillar 6): the list loaded earlier this session stays on
/// screen, with a freshness cue, while a refresh runs, while the tracker is offline and after a failed load.
struct CollabListStatus {
    bool HaveData = false;       ///< a list is shown: the one this load returned or the one saved this session
    bool DataLive = false;       ///< it came from the load that just finished
    bool LastLoadFailed = false; ///< the last load failed, or was skipped because the tracker is offline
    TrackerErrorKind LastErrorKind = TrackerErrorKind::None;
    std::string Error; ///< why the last load failed (the cue's tooltip)
};

/** Async UI state for watchers/votes side panels (owned by the main UI session). */
struct TrackerGridFieldAsyncState {
    bool watchersPanelOpen = false;
    std::string watchersPopupIssueKey;
    bool watchersLoadInProgress = false;
    std::future<WatchersLoadResult> watchersFuture;
    std::vector<TrackerUser> watchersLoadedList;
    CollabListStatus watchersStatus;
    /// (backend key, issue key) -> the last watchers list loaded this session.
    std::map<std::string, std::vector<TrackerUser>> watchersSessionCache;
    std::string watchersSessionKey; ///< the entry the open window's load belongs to

    bool watchSelfInProgress = false;
    std::future<PendingActionSubmitResult> watchSelfFuture;
    std::string watchSelfError;
    std::string watchSelfPendingIssueKey;
    std::set<std::string> watchSelfSucceededIssueKeys; ///< sent this session: the Watch button hides
    std::string watchSelfPendingBackendKey;            ///< focused backend when Watch was clicked
    /// (backend key, issue key) -> offline-queue id of a watch saved offline (Pillar 6). The Watch button hides
    /// only while that row is still queued, so a watch that later fails or is discarded can be retried; the
    /// backend part keeps one backend's queued watch from hiding another backend's issue with the same key.
    std::map<std::string, std::int64_t> watchSelfQueuedIds;

    bool votesPanelOpen = false;
    std::string votesPopupIssueKey;
    bool votesLoadInProgress = false;
    std::future<VotesLoadResult> votesFuture;
    VotesLoadResult votesLoaded; ///< the votes shown (their Ok / Error are unused; the status says)
    CollabListStatus votesStatus;
    /// (backend key, issue key) -> the last votes loaded this session.
    std::map<std::string, VotesLoadResult> votesSessionCache;
    std::string votesSessionKey; ///< the entry the open window's load belongs to
};

/**
 * Renders Jira-specific grid cell types: attachments, watchers, votes, worklog summary.
 * List panels for watchers/votes use TrackerGridFieldAsyncState on the UI session.
 */
class TrackerGridFieldDisplay {
  public:
    static bool IsWatchersColumnId(const std::string& id);
    static bool IsVotesColumnId(const std::string& id);
    static bool IsWorklogColumnId(const std::string& id);

    static void RenderAttachmentsField(AppController& app, const std::string& currentValue, float availWidth,
                                       bool tooltipsEnabled);
    static void RenderWatchersField(AppController& app, const std::string& issueKey, const std::string& currentValue,
                                    float availWidth, bool tooltipsEnabled, TrackerGridFieldAsyncState& async);
    static void RenderVotesField(AppController& app, const std::string& issueKey, const std::string& currentValue,
                                 float availWidth, bool tooltipsEnabled, TrackerGridFieldAsyncState& async);
    /**
     * Renders the `worklog` ("Log Work") cell as a flat, full-width action button carrying the
     * work-log summary (or "Log work" when nothing is logged yet).
     * @return true on the frame the user clicked it — the caller opens the time-tracking dialog.
     *         Always false for an unparseable non-empty payload, which stays read-only text.
     */
    static bool RenderWorklogField(const std::string& currentValue, float availWidth, bool tooltipsEnabled);

    /**
     * Flat, chrome-less, full-width cell button (transparent until hovered, left-aligned label) —
     * the affordance shared by the grid's in-cell actions (`timespent`, `worklog`).
     * @param availWidth cell width; <= 0 stretches to the remaining content region.
     */
    static bool DrawCellActionButton(const std::string& label, float availWidth);

    /** Column key `aggregateprogress` (Jira schema id; case-insensitive). */
    static bool IsProgressStyleColumnId(const std::string& fieldId);

    /**
     * Jira Progress / Aggregate progress: display name or id.
     * Matches name "Progress", "aggregate progress", "aggregateprogress", or id "aggregateprogress".
     */
    static bool IsProgressDisplayField(const TrackerField* field);

    /**
     * Renders `{"progress":n,"total":m}` (and double-encoded JSON string) as ImGui::ProgressBar.
     * @return false if the value is not a matching JSON object (caller should use default text).
     */
    static bool TryRenderProgressJsonField(const std::string& currentValue, float availWidth);

    /** Column key `issuerestriction` (Jira schema id; case-insensitive). */
    static bool IsIssueRestrictionColumnId(const std::string& fieldId);

    /** Issue restriction field by id or display name (issue restriction / issue restrictions). */
    static bool IsIssueRestrictionField(const TrackerField* field);

    /**
     * Renders `{"issuerestrictions":{...},"shouldDisplay":bool}` as compact read-only text.
     * @return false if empty, invalid JSON, or wrong shape (caller uses default text).
     */
    static bool TryRenderIssueRestrictionField(const std::string& currentValue, float availWidth, bool tooltipsEnabled);

    static void DrawWatchersListWindow(TrackerGridFieldAsyncState& async);
    static void DrawVotesListWindow(TrackerGridFieldAsyncState& async);
};
