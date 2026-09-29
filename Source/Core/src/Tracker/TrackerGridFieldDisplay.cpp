// SMATCHET_DEVIATION(rule=duplication; reason=include overlap with sibling UI TU; owner=ui; revisit=dup-scoping)
#include "TrackerGridFieldDisplay.h"
#include "UiPerfMonitor.h"
#include "AppController.h"
#include "ConfigManager.h"

#include "DataFreshnessCue.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "SmatchetLocalization.h"
#include "StringUtil.h"
#include "TrackerGridFieldDisplayPure.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The render-model builders (attachment / watchers / votes / worklog / issue-restriction /
// progress) live in TrackerGridFieldDisplayPure.cpp — a byte-identical lift out of this TU's
// anonymous namespace so the untrusted tracker-value parsing is unit-testable without ImGui
// (tests/Core/TrackerGridFieldDisplayPure.test.cpp). This TU keeps the draw code + the
// per-value render-model caches.
// SMATCHET_DEVIATION(rule=duplication; reason=shared-helper using-block; owner=tracker-backend; revisit=2026-12-31)
using TrackerGridFieldDisplayPure::AttachmentRenderModel;
using TrackerGridFieldDisplayPure::BuildAttachmentRenderModel;
using TrackerGridFieldDisplayPure::BuildIssueRestrictionRenderModel;
using TrackerGridFieldDisplayPure::BuildProgressRenderModel;
using TrackerGridFieldDisplayPure::BuildVotesRenderModel;
using TrackerGridFieldDisplayPure::BuildWatchersRenderModel;
using TrackerGridFieldDisplayPure::BuildWorklogCellModel;
using TrackerGridFieldDisplayPure::IssueRestrictionRenderModel;
using TrackerGridFieldDisplayPure::ProgressRenderModel;
using TrackerGridFieldDisplayPure::VotesRenderModel;
using TrackerGridFieldDisplayPure::WatchersRenderModel;
using TrackerGridFieldDisplayPure::WorklogCellModel;

namespace {

constexpr std::size_t kMaxRenderCacheEntries = 256;

template <typename TValue, typename TBuilder>
TValue& GetOrBuildCachedValue(std::unordered_map<std::string, TValue>& cache, const std::string& key,
                              TBuilder&& build) {
    const auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    if (cache.size() >= kMaxRenderCacheEntries) {
        cache.clear();
    }
    return cache.emplace(key, build()).first->second;
}

bool ColumnIdEqualsLower(const std::string& id, const char* lowerAscii) { return ToLowerAsciiCopy(id) == lowerAscii; }

} // namespace

bool TrackerGridFieldDisplay::IsWatchersColumnId(const std::string& id) {
    const std::string lower = ToLowerAsciiCopy(id);
    return lower == "watchers" || lower == "watches";
}

bool TrackerGridFieldDisplay::IsVotesColumnId(const std::string& id) { return ColumnIdEqualsLower(id, "votes"); }

bool TrackerGridFieldDisplay::IsWorklogColumnId(const std::string& id) { return ColumnIdEqualsLower(id, "worklog"); }

bool TrackerGridFieldDisplay::IsProgressStyleColumnId(const std::string& fieldId) {
    return ColumnIdEqualsLower(fieldId, "aggregateprogress");
}

bool TrackerGridFieldDisplay::IsProgressDisplayField(const TrackerField* field) {
    if (field == nullptr) {
        return false;
    }
    if (IsProgressStyleColumnId(field->Id)) {
        return true;
    }
    return ColumnIdEqualsLower(field->Name, "progress") || ColumnIdEqualsLower(field->Name, "aggregateprogress") ||
           ColumnIdEqualsLower(field->Name, "aggregate progress");
}

bool TrackerGridFieldDisplay::TryRenderProgressJsonField(const std::string& currentValue, float availWidth) {
    SMATCHET_UI_PERF_SCOPE("TryRenderProgressJsonField");

    // Zero-overhead shortcut for empty cells (no hashing, no map lookups)
    const bool isEmpty = !std::any_of(currentValue.begin(), currentValue.end(),
                                      [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });
    if (isEmpty) {
        ImGui::ProgressBar(0.0f, ImVec2(std::max(1.0f, availWidth), ImGui::GetFrameHeight()));
        return true;
    }

    static thread_local std::unordered_map<std::string, ProgressRenderModel> cache;
    const ProgressRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildProgressRenderModel(currentValue); });
    if (model.rendered) {
        ImGui::ProgressBar(model.fraction, ImVec2(std::max(1.0f, availWidth), ImGui::GetFrameHeight()));
    }
    return model.rendered;
}

bool TrackerGridFieldDisplay::IsIssueRestrictionColumnId(const std::string& fieldId) {
    return ColumnIdEqualsLower(fieldId, "issuerestriction");
}

bool TrackerGridFieldDisplay::IsIssueRestrictionField(const TrackerField* field) {
    if (field == nullptr) {
        return false;
    }
    if (IsIssueRestrictionColumnId(field->Id)) {
        return true;
    }
    return ColumnIdEqualsLower(field->Name, "issue restriction") ||
           ColumnIdEqualsLower(field->Name, "issue restrictions");
}

bool TrackerGridFieldDisplay::TryRenderIssueRestrictionField(const std::string& currentValue, float availWidth,
                                                             bool tooltipsEnabled) {
    SMATCHET_UI_PERF_SCOPE("TryRenderIssueRestrictionField");
    static thread_local std::unordered_map<std::string, IssueRestrictionRenderModel> cache;
    const IssueRestrictionRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildIssueRestrictionRenderModel(currentValue); });
    if (model.rendered) {
        const std::string* tip = tooltipsEnabled ? &currentValue : nullptr;
        RenderClippedFieldText(model.display, availWidth, tooltipsEnabled, true, tip);
    }
    return model.rendered;
}

void TrackerGridFieldDisplay::RenderAttachmentsField(AppController& app, const std::string& currentValue,
                                                     float availWidth, bool tooltipsEnabled) {
    SMATCHET_UI_PERF_SCOPE("RenderAttachmentsField");
    static thread_local std::unordered_map<std::string, AttachmentRenderModel> cache;
    const AttachmentRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildAttachmentRenderModel(currentValue); });
    if (!model.parsed) {
        if (model.explicitEmpty) {
            RenderClippedFieldText(std::string(), availWidth, tooltipsEnabled, true);
            return;
        }
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.65f, 1.0f, 1.0f));
    // Raw (::) on purpose: model.display is tracker data. The aliased TextUnformatted would run
    // it through TranslateSource, so a field value that exactly matches a catalog English
    // string ("Offline", "All", ...) would render translated instead of as-is.
    ::ImGui::TextUnformatted(model.display.c_str());

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", model.tooltip.c_str());
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::PopStyleColor();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        app.ShowAttachmentCollection(model.descriptors);
    }
}

namespace {

enum class QueuedWatchState : unsigned char { Queued, Failed, Gone };

// Where the watch saved as queue row `queueId` stands, from the in-memory queue snapshot (no SQLite). Gone
// means it left the queue without failing: it was applied, or the user discarded it.
QueuedWatchState ClassifyQueuedWatch(const AppController& app, std::int64_t queueId) {
    const std::shared_ptr<const PendingActionsSnapshot> snap = app.GetPendingActionsSnapshot();
    if (std::any_of(snap->Pending.begin(), snap->Pending.end(),
                    [queueId](const PendingActionRecord& row) { return row.Id == queueId; })) {
        return QueuedWatchState::Queued;
    }
    if (std::any_of(snap->Dead.begin(), snap->Dead.end(),
                    [queueId](const DeadPendingAction& dead) { return dead.Row.Id == queueId; })) {
        return QueuedWatchState::Failed;
    }
    return QueuedWatchState::Gone;
}

// Key of the per-issue maps in TrackerGridFieldAsyncState (queued watches, saved watchers / votes lists):
// an entry belongs to one backend, so another backend's issue with the same key never sees it.
std::string BackendIssueKey(const std::string& backendKey, const std::string& issueKey) {
    return backendKey + '\x1f' + issueKey;
}

// A watch queued in an earlier session (or restored from the failed list) is adopted into the tracking
// below, so a restart never lets the same watch be queued twice and its exit re-reads the issue too.
bool AdoptPersistedWatch(AppController& app, const std::string& issueKey, TrackerGridFieldAsyncState& async) {
    const std::int64_t persistedId = app.FindQueuedPendingActionId(PendingActionKind::WatchAdd, issueKey);
    if (persistedId == 0) {
        return false;
    }
    async.watchSelfQueuedIds[BackendIssueKey(app.FocusedCacheBackendKey(), issueKey)] = persistedId;
    return true;
}

// True while this issue's watch on the focused backend is still waiting in the offline queue. Once it
// leaves the queue its entry is dropped so the Watch button can show again: a failed watch can be retried,
// and after any other exit the issue is re-read so its watchers field says whether the watch was applied.
bool WatchQueuedForIssue(AppController& app, const std::string& issueKey, TrackerGridFieldAsyncState& async) {
    if (async.watchSelfQueuedIds.empty()) {
        return AdoptPersistedWatch(app, issueKey, async); // the common case: no per-frame backend-key copy
    }
    const auto queued = async.watchSelfQueuedIds.find(BackendIssueKey(app.FocusedCacheBackendKey(), issueKey));
    if (queued == async.watchSelfQueuedIds.end()) {
        return AdoptPersistedWatch(app, issueKey, async);
    }
    const QueuedWatchState state = ClassifyQueuedWatch(app, queued->second);
    if (state == QueuedWatchState::Queued) {
        return true;
    }
    async.watchSelfQueuedIds.erase(queued);
    if (state == QueuedWatchState::Gone) {
        app.PrefetchIssueTicketsForKeys({issueKey}, true);
    }
    return false;
}

// A watchers / votes load is starting: the list saved this session (when there is one) stays on screen
// until the tracker answers. Offline no request is sent and the saved list is marked as offline.
// Returns true when the caller should launch the request. A finished result the window has not read yet
// belongs to the previously clicked issue, so it is dropped (the Load button is disabled while a load
// runs, so that future is already ready and dropping it never waits).
template <typename LoadResult>
bool BeginListLoad(AppController& app, std::future<LoadResult>& future, bool& inProgress, CollabListStatus& status,
                   bool haveSaved) {
    future = std::future<LoadResult>();
    inProgress = false;
    status = CollabListStatus();
    status.HaveData = haveSaved;
    if (!app.IsTrackerOffline()) {
        return true;
    }
    status.LastLoadFailed = true;
    status.LastErrorKind = TrackerErrorKind::Transport;
    status.Error = SmatchetLocalization::T("collab_list.offline", "The tracker is offline.");
    return false;
}

void FinishListLoad(CollabListStatus& status, bool ok, TrackerErrorKind kind, const std::string& error) {
    status.LastLoadFailed = !ok;
    status.LastErrorKind = ok ? TrackerErrorKind::None : kind;
    status.Error = ok ? std::string() : error;
    if (ok) {
        status.HaveData = true;
        status.DataLive = true;
    }
}

// Runs `fetch` on a worker. The in-flight flag is set only once the worker exists, so a failed launch
// shows an error instead of a load that never finishes.
template <typename LoadResult, typename FetchFn>
void LaunchListLoad(std::future<LoadResult>& future, bool& inProgress, CollabListStatus& status, const char* what,
                    FetchFn fetch) {
    try {
        future = std::async(std::launch::async, std::move(fetch));
        inProgress = true;
    } catch (const std::exception& ex) {
        FinishListLoad(status, false, TrackerErrorKind::Unknown,
                       std::string("Failed to load ") + what + ": " + ex.what());
        LOG_ERROR("TrackerGridFieldDisplay: could not start the %s load: %s", what, ex.what());
    }
}

// Above a watchers / votes list: the freshness cue (nothing when the list is live), or, when a load the
// tracker refused left nothing to show, that error. True when the list itself should be drawn.
bool DrawListStatus(const CollabListStatus& status, bool loading) {
    if (!status.HaveData && !loading && status.LastLoadFailed && status.LastErrorKind != TrackerErrorKind::Transport) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", status.Error.c_str());
        ImGui::PopStyleColor();
        return false;
    }
    smatchet::offline::FreshnessInputs in;
    in.HasCache = status.HaveData;
    in.Live = status.DataLive;
    in.InFlight = loading;
    in.LastAttemptFailed = status.LastLoadFailed;
    in.Connectivity = status.LastErrorKind == TrackerErrorKind::Transport
                          ? TrackerConnectivityState::TransportDown
                          : TrackerConnectivityState::AuthenticatedReachable;
    const smatchet::offline::DataFreshness freshness = smatchet::offline::ClassifyFreshness(in);
    DataFreshnessCue::Draw(freshness, status.Error.empty() ? nullptr : status.Error.c_str());
    return smatchet::offline::ShouldRenderContent(freshness);
}

// Takes a finished watchers / votes load off its future (true when one finished this frame). A worker
// exception becomes the load's error, never a crash, and the in-flight flag clears on every path.
template <typename LoadResult>
bool TakeFinishedLoad(std::future<LoadResult>& future, bool& inProgress, const char* what, const std::string& issueKey,
                      LoadResult& out) {
    if (!future.valid() || future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return false;
    }
    inProgress = false;
    try {
        out = future.get();
    } catch (const std::exception& ex) {
        out = LoadResult();
        out.Error = std::string("Failed to load ") + what + ": " + ex.what();
        out.ErrorKind = TrackerErrorKind::Unknown;
        LOG_ERROR("TrackerGridFieldDisplay: %s future exception issue=%s err=%s", what, issueKey.c_str(), ex.what());
    } catch (...) { // catch-all-ok: a worker exception becomes the load's error, never a crash
        out = LoadResult();
        out.Error = std::string("Failed to load ") + what + ".";
        out.ErrorKind = TrackerErrorKind::Unknown;
        LOG_ERROR("TrackerGridFieldDisplay: %s future unknown exception issue=%s", what, issueKey.c_str());
    }
    return true;
}

} // namespace

void TrackerGridFieldDisplay::RenderWatchersField(AppController& app, const std::string& issueKey,
                                                  const std::string& currentValue, float availWidth,
                                                  bool tooltipsEnabled, TrackerGridFieldAsyncState& async) {
    SMATCHET_UI_PERF_SCOPE("RenderWatchersField");
    static thread_local std::unordered_map<std::string, WatchersRenderModel> cache;
    const WatchersRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildWatchersRenderModel(currentValue); });
    if (model.parsed) {
        ImGui::TextUnformatted(model.line.c_str());
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", model.tooltip.c_str());
        }
    } else {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
    }

    ImGui::SameLine();
    const std::string loadBtn = "Load##watch_" + issueKey;
    const bool watchersBusy = async.watchersLoadInProgress && async.watchersFuture.valid() &&
                              async.watchersFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (watchersBusy) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton(loadBtn.c_str())) {
        // Pillar 6: the list loaded earlier this session shows at once and stays while the tracker answers
        // (or while it is offline, when no request is sent).
        async.watchersPopupIssueKey = issueKey;
        async.watchersPanelOpen = true;
        async.watchersSessionKey = BackendIssueKey(app.FocusedCacheBackendKey(), issueKey);
        const auto saved = async.watchersSessionCache.find(async.watchersSessionKey);
        const bool haveSaved = saved != async.watchersSessionCache.end();
        async.watchersLoadedList = haveSaved ? saved->second : std::vector<TrackerUser>();
        if (BeginListLoad(app, async.watchersFuture, async.watchersLoadInProgress, async.watchersStatus, haveSaved)) {
            LaunchListLoad(async.watchersFuture, async.watchersLoadInProgress, async.watchersStatus, "watchers",
                           [&app, issueKey]() {
                               WatchersLoadResult r;
                               Result<std::vector<TrackerUser>, TrackerError> res =
                                   app.FetchIssueWatchersTyped(issueKey);
                               r.Ok = res.has_value();
                               if (r.Ok) {
                                   r.Watchers = std::move(res.value());
                               } else {
                                   r.Error = res.error().Detail;
                                   r.ErrorKind = res.error().Kind;
                               }
                               return r;
                           });
        }
    }
    if (watchersBusy) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(watchersBusy ? "Loading watchers..." : "Load watcher list from Tracker");
    }

    const bool alreadyWatchedThisSession = (async.watchSelfSucceededIssueKeys.count(issueKey) > 0);
    const bool watchQueued = WatchQueuedForIssue(app, issueKey, async);
    if (watchQueued && model.parsed && !model.isWatching) {
        ImGui::SameLine();
        ImGui::TextDisabled("(watch queued)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Queued \xE2\x80\x94 will apply when the tracker is reachable");
        }
    }
    if (model.parsed && !model.isWatching && !alreadyWatchedThisSession && !watchQueued) {
        ImGui::SameLine();
        const std::string watchBtn = "Watch##wself_" + issueKey;
        const bool watchBusy = async.watchSelfInProgress && async.watchSelfFuture.valid() &&
                               async.watchSelfFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        if (watchBusy) {
            ImGui::BeginDisabled();
        }
        if (ImGui::SmallButton(watchBtn.c_str())) {
            // Latched on click: the watch goes to this pane's tracker even if focus moves before the worker runs.
            const PendingActionTarget target = app.LatchPendingActionTarget();
            async.watchSelfPendingIssueKey = issueKey;
            async.watchSelfPendingBackendKey = target.BackendKey;
            async.watchSelfInProgress = true;
            async.watchSelfError.clear();
            // Offline the watch is saved and applied on reconnect (pending-action queue, Pillar 6).
            async.watchSelfFuture = std::async(
                std::launch::async, [&app, target, issueKey]() { return app.SubmitOrQueueWatch(target, issueKey); });
        }
        if (watchBusy) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (watchBusy) {
                ImGui::SetTooltip("Adding you as a watcher...");
            } else if (!async.watchSelfError.empty() && !async.watchSelfFuture.valid()) {
                ImGui::SetTooltip("Watch failed: %s", async.watchSelfError.c_str());
            } else {
                ImGui::SetTooltip("Watch this issue (adds you as a watcher)");
            }
        }
    }
}

void TrackerGridFieldDisplay::RenderVotesField(AppController& app, const std::string& issueKey,
                                               const std::string& currentValue, float availWidth, bool tooltipsEnabled,
                                               TrackerGridFieldAsyncState& async) {
    SMATCHET_UI_PERF_SCOPE("RenderVotesField");
    static thread_local std::unordered_map<std::string, VotesRenderModel> cache;
    const VotesRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildVotesRenderModel(currentValue); });
    if (model.parsed) {
        ImGui::TextUnformatted(model.line.c_str());
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", model.tooltip.c_str());
        }
    } else {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
    }

    ImGui::SameLine();
    const std::string loadBtn = "Load##votes_" + issueKey;
    const bool votesBusy = async.votesLoadInProgress && async.votesFuture.valid() &&
                           async.votesFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (votesBusy) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton(loadBtn.c_str())) {
        // Pillar 6: as for watchers, the votes loaded earlier this session show while the tracker answers.
        async.votesPopupIssueKey = issueKey;
        async.votesPanelOpen = true;
        async.votesSessionKey = BackendIssueKey(app.FocusedCacheBackendKey(), issueKey);
        const auto saved = async.votesSessionCache.find(async.votesSessionKey);
        const bool haveSaved = saved != async.votesSessionCache.end();
        async.votesLoaded = haveSaved ? saved->second : VotesLoadResult();
        if (BeginListLoad(app, async.votesFuture, async.votesLoadInProgress, async.votesStatus, haveSaved)) {
            LaunchListLoad(async.votesFuture, async.votesLoadInProgress, async.votesStatus, "votes",
                           [&app, issueKey]() {
                               VotesLoadResult r;
                               Result<TrackerIssueVotes, TrackerError> res = app.FetchIssueVotesTyped(issueKey);
                               r.Ok = res.has_value();
                               if (r.Ok) {
                                   TrackerIssueVotes& v = res.value();
                                   r.Voters = std::move(v.Voters);
                                   r.VoteCount = v.VoteCount;
                                   r.HasVoted = v.HasVoted;
                                   r.VotersArrayInResponse = v.VotersArrayInResponse;
                               } else {
                                   r.Error = res.error().Detail;
                                   r.ErrorKind = res.error().Kind;
                               }
                               return r;
                           });
        }
    }
    if (votesBusy) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(votesBusy ? "Loading votes..." : "Load voter list from Tracker");
    }
}

bool TrackerGridFieldDisplay::DrawCellActionButton(const std::string& label, float availWidth) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0)); // invisible background when normal
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    // ID uniqueness comes from the caller's CellIdScope (ticket id + field id on the ID stack).
    const bool clicked = ImGui::Button(label.c_str(), ImVec2(availWidth > 0.0f ? availWidth : -FLT_MIN, 0.0f));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return clicked;
}

bool TrackerGridFieldDisplay::RenderWorklogField(const std::string& currentValue, float availWidth,
                                                 bool tooltipsEnabled) {
    SMATCHET_UI_PERF_SCOPE("RenderWorklogField");
    static thread_local std::unordered_map<std::string, WorklogCellModel> cache;
    const WorklogCellModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildWorklogCellModel(currentValue); });
    if (!model.clickable) {
        RenderClippedFieldText(model.label, availWidth, tooltipsEnabled, true);
        return false;
    }

    const bool clicked = DrawCellActionButton(model.label, availWidth);
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        // Localized at the sink, not in the model (#2173): SetTooltip("%s", buf) translates
        // only the format string, so an English sentence baked into the model stayed English
        // in every locale while the sibling timespent cell showed the same text translated.
        // The fixed sentences go through the table; the per-entry summary is data.
        ImGui::BeginTooltip();
        if (model.noWorkLogged) {
            ImGui::TextUnformatted(SmatchetLocalization::T("worklog.none", "No work logged yet. Click to log work."));
        } else {
            ImGui::TextUnformatted(model.tooltip.c_str());
            ImGui::NewLine();
            ImGui::TextUnformatted(
                SmatchetLocalization::T("worklog.click_hint", "Click to log work / edit estimates."));
        }
        ImGui::EndTooltip();
    }
    return clicked;
}

void TrackerGridFieldDisplay::DrawWatchersListWindow(TrackerGridFieldAsyncState& d) {
    if (d.watchSelfFuture.valid()) {
        if (d.watchSelfFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                const PendingActionSubmitResult result = d.watchSelfFuture.get();
                d.watchSelfInProgress = false;
                if (result.K == PendingActionSubmitResult::Kind::Sent) {
                    d.watchSelfSucceededIssueKeys.insert(d.watchSelfPendingIssueKey);
                    d.watchSelfError.clear();
                } else if (result.K == PendingActionSubmitResult::Kind::Queued) {
                    // Saved, not applied: the button stays hidden only while the row is queued.
                    d.watchSelfQueuedIds[BackendIssueKey(d.watchSelfPendingBackendKey, d.watchSelfPendingIssueKey)] =
                        result.QueueId;
                    d.watchSelfError.clear();
                } else {
                    d.watchSelfError = result.Error.empty() ? std::string("Watch failed.") : result.Error;
                    LOG_ERROR("TrackerGridFieldDisplay: watch self failed issue=%s err=%s",
                              d.watchSelfPendingIssueKey.c_str(), d.watchSelfError.c_str());
                }
            } catch (const std::exception& ex) {
                d.watchSelfInProgress = false;
                d.watchSelfError = std::string("Watch failed: ") + ex.what();
                LOG_ERROR("TrackerGridFieldDisplay: watchSelf future exception: %s", ex.what());
            } catch (...) {
                d.watchSelfInProgress = false;
                d.watchSelfError = "Watch failed.";
                LOG_ERROR("TrackerGridFieldDisplay: watchSelf future unknown exception");
            }
        }
    }

    WatchersLoadResult watchers;
    if (TakeFinishedLoad(d.watchersFuture, d.watchersLoadInProgress, "watchers", d.watchersPopupIssueKey, watchers)) {
        FinishListLoad(d.watchersStatus, watchers.Ok, watchers.ErrorKind, watchers.Error);
        if (watchers.Ok) {
            d.watchersLoadedList = watchers.Watchers;
            d.watchersSessionCache[d.watchersSessionKey] = std::move(watchers.Watchers);
        }
    }

    if (!d.watchersPanelOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Watchers", &d.watchersPanelOpen, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("Issue:");
        ImGui::SameLine();
        ImGui::TextUnformatted(d.watchersPopupIssueKey.c_str());
        ImGui::Separator();
        if (DrawListStatus(d.watchersStatus, d.watchersLoadInProgress)) {
            if (d.watchersLoadedList.empty()) {
                ImGui::TextDisabled("No watchers.");
            } else {
                ImGui::BeginChild("WatchersList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
                for (const auto& w : d.watchersLoadedList) {
                    const std::string label = w.DisplayName.empty() ? w.AccountId : w.DisplayName;
                    ImGui::BulletText("%s", label.c_str());
                }
                ImGui::EndChild();
            }
        }
        if (ImGui::Button("Close")) {
            d.watchersPanelOpen = false;
        }
    }
    ImGui::End();
}

void TrackerGridFieldDisplay::DrawVotesListWindow(TrackerGridFieldAsyncState& d) {
    VotesLoadResult votes;
    if (TakeFinishedLoad(d.votesFuture, d.votesLoadInProgress, "votes", d.votesPopupIssueKey, votes)) {
        FinishListLoad(d.votesStatus, votes.Ok, votes.ErrorKind, votes.Error);
        if (votes.Ok) {
            d.votesLoaded = votes;
            d.votesSessionCache[d.votesSessionKey] = std::move(votes);
        }
    }

    if (!d.votesPanelOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Votes", &d.votesPanelOpen, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("Issue:");
        ImGui::SameLine();
        ImGui::TextUnformatted(d.votesPopupIssueKey.c_str());
        ImGui::Separator();
        if (DrawListStatus(d.votesStatus, d.votesLoadInProgress)) {
            const VotesLoadResult& shown = d.votesLoaded;
            std::string summary = std::to_string(shown.VoteCount) + " vote";
            if (shown.VoteCount != 1) {
                summary += "s";
            }
            if (shown.HasVoted) {
                summary += " (you voted)";
            }
            ImGui::TextUnformatted(summary.c_str());
            ImGui::Spacing();
            if (shown.Voters.empty()) {
                if (shown.VoteCount == 0) {
                    ImGui::TextDisabled("No votes.");
                } else if (!shown.VotersArrayInResponse) {
                    ImGui::TextDisabled("Voter names are hidden by Tracker permissions (View voters and watchers).");
                } else {
                    ImGui::TextDisabled("No voters to list.");
                }
            } else {
                ImGui::BeginChild("VotesList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
                for (const auto& w : shown.Voters) {
                    const std::string label = w.DisplayName.empty() ? w.AccountId : w.DisplayName;
                    ImGui::BulletText("%s", label.c_str());
                }
                ImGui::EndChild();
            }
        }
        if (ImGui::Button("Close")) {
            d.votesPanelOpen = false;
        }
    }
    ImGui::End();
}
