#pragma once

#include "TrackerFieldSchema.h"

#include <cstdint>
#include <future>
#include <set>
#include <string>
#include <vector>

class AppController;

struct WatchersLoadResult {
    bool Ok = false;
    std::vector<TrackerUser> Watchers;
    std::string Error;
};

struct VotesLoadResult {
    bool Ok = false;
    std::vector<TrackerUser> Voters;
    std::string Error;
    int VoteCount = 0;
    bool HasVoted = false;
    /** True when JSON contained a `voters` array (may be empty); false when key absent (often permissions). */
    bool VotersArrayInResponse = false;
};

/** Async UI state for watchers/votes side panels (owned by the main UI session). */
struct TrackerGridFieldAsyncState {
    bool watchersPanelOpen = false;
    std::string watchersPopupIssueKey;
    bool watchersLoadInProgress = false;
    std::future<WatchersLoadResult> watchersFuture;
    std::vector<TrackerUser> watchersLoadedList;
    std::string watchersLoadedError;

    bool watchSelfInProgress = false;
    std::future<std::string> watchSelfFuture;
    std::string watchSelfError;
    std::string watchSelfPendingIssueKey;
    std::set<std::string> watchSelfSucceededIssueKeys;

    bool votesPanelOpen = false;
    std::string votesPopupIssueKey;
    bool votesLoadInProgress = false;
    std::future<VotesLoadResult> votesFuture;
    std::vector<TrackerUser> votesLoadedList;
    std::string votesLoadedError;
    int votesLoadedVoteCount = 0;
    bool votesLoadedHasVoted = false;
    bool votesLoadedVotersArrayInResponse = false;
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
    static void RenderWorklogField(const std::string& currentValue, float availWidth, bool tooltipsEnabled);

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
