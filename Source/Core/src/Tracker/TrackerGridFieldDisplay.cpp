#include "TrackerGridFieldDisplay.h"
#include "UiPerfMonitor.h"
#include "AppController.h"
#include "ConfigManager.h"

#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "StringUtil.h"
#include "TrackerGridFieldDisplayPure.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

// SMATCHET_DEVIATION(rule=duplication; reason=UI-shell include boilerplate; owner=tracker-backend; revisit=2026-09-30)
#include <algorithm>
#include <cctype>
#include <exception>
#include <future>
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
using TrackerGridFieldDisplayPure::BuildWorklogRenderModel;
using TrackerGridFieldDisplayPure::IssueRestrictionRenderModel;
using TrackerGridFieldDisplayPure::ProgressRenderModel;
using TrackerGridFieldDisplayPure::VotesRenderModel;
using TrackerGridFieldDisplayPure::WatchersRenderModel;
using TrackerGridFieldDisplayPure::WorklogRenderModel;

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
        async.watchersPopupIssueKey = issueKey;
        async.watchersPanelOpen = true;
        async.watchersLoadInProgress = true;
        async.watchersLoadedList.clear();
        async.watchersLoadedError.clear();
        async.watchersFuture = std::async(std::launch::async, [&app, issueKey]() {
            WatchersLoadResult r;
            std::string err;
            std::vector<TrackerUser> watchers;
            if (app.FetchIssueWatchers(issueKey, watchers, err)) {
                r.Watchers = watchers;
                r.Ok = true;
            } else {
                r.Ok = false;
                r.Error = std::move(err);
            }
            return r;
        });
    }
    if (watchersBusy) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(watchersBusy ? "Loading watchers..." : "Load watcher list from Tracker");
    }

    const bool alreadyWatchedThisSession = (async.watchSelfSucceededIssueKeys.count(issueKey) > 0);
    if (model.parsed && !model.isWatching && !alreadyWatchedThisSession) {
        ImGui::SameLine();
        const std::string watchBtn = "Watch##wself_" + issueKey;
        const bool watchBusy = async.watchSelfInProgress && async.watchSelfFuture.valid() &&
                               async.watchSelfFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        if (watchBusy) {
            ImGui::BeginDisabled();
        }
        if (ImGui::SmallButton(watchBtn.c_str())) {
            async.watchSelfPendingIssueKey = issueKey;
            async.watchSelfInProgress = true;
            async.watchSelfError.clear();
            async.watchSelfFuture = std::async(std::launch::async, [&app, issueKey]() {
                const VoidResult r = app.AddIssueWatcher(issueKey);
                return r.has_value() ? std::string() : r.error();
            });
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
        async.votesPopupIssueKey = issueKey;
        async.votesPanelOpen = true;
        async.votesLoadInProgress = true;
        async.votesLoadedList.clear();
        async.votesLoadedError.clear();
        async.votesLoadedVoteCount = 0;
        async.votesLoadedHasVoted = false;
        async.votesLoadedVotersArrayInResponse = false;
        async.votesFuture = std::async(std::launch::async, [&app, issueKey]() {
            VotesLoadResult r;
            std::string err;
            std::vector<TrackerUser> voters;
            if (app.FetchIssueVotes(issueKey, voters, err, &r.VoteCount, &r.HasVoted, &r.VotersArrayInResponse)) {
                r.Voters = voters;
                r.Ok = true;
            } else {
                r.Ok = false;
                r.Error = std::move(err);
            }
            return r;
        });
    }
    if (votesBusy) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(votesBusy ? "Loading votes..." : "Load voter list from Tracker");
    }
}

void TrackerGridFieldDisplay::RenderWorklogField(const std::string& currentValue, float availWidth,
                                                 bool tooltipsEnabled) {
    SMATCHET_UI_PERF_SCOPE("RenderWorklogField");
    static thread_local std::unordered_map<std::string, WorklogRenderModel> cache;
    const WorklogRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildWorklogRenderModel(currentValue); });
    if (!model.parsed) {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
        return;
    }

    ImGui::TextUnformatted(model.line.c_str());
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", model.tooltip.c_str());
    }
}

void TrackerGridFieldDisplay::DrawWatchersListWindow(TrackerGridFieldAsyncState& d) {
    if (d.watchSelfFuture.valid()) {
        if (d.watchSelfFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                std::string err = d.watchSelfFuture.get();
                d.watchSelfInProgress = false;
                if (err.empty()) {
                    d.watchSelfSucceededIssueKeys.insert(d.watchSelfPendingIssueKey);
                    d.watchSelfError.clear();
                } else {
                    d.watchSelfError = std::move(err);
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

    if (d.watchersFuture.valid()) {
        if (d.watchersFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                WatchersLoadResult r = d.watchersFuture.get();
                d.watchersLoadInProgress = false;
                if (r.Ok) {
                    d.watchersLoadedList = std::move(r.Watchers);
                    d.watchersLoadedError.clear();
                } else {
                    d.watchersLoadedList.clear();
                    d.watchersLoadedError = std::move(r.Error);
                }
            } catch (const std::exception& ex) {
                d.watchersLoadInProgress = false;
                d.watchersLoadedList.clear();
                d.watchersLoadedError = std::string("Failed to load watchers: ") + ex.what();
                LOG_ERROR("TrackerGridFieldDisplay: watchers future exception issue=%s err=%s",
                          d.watchersPopupIssueKey.c_str(), ex.what());
            } catch (...) {
                d.watchersLoadInProgress = false;
                d.watchersLoadedList.clear();
                d.watchersLoadedError = "Failed to load watchers.";
                LOG_ERROR("TrackerGridFieldDisplay: watchers future unknown exception issue=%s",
                          d.watchersPopupIssueKey.c_str());
            }
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
        if (d.watchersLoadInProgress) {
            ImGui::TextDisabled("Loading...");
        } else if (!d.watchersLoadedError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", d.watchersLoadedError.c_str());
            ImGui::PopStyleColor();
        } else {
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
    if (d.votesFuture.valid()) {
        if (d.votesFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                VotesLoadResult r = d.votesFuture.get();
                d.votesLoadInProgress = false;
                if (r.Ok) {
                    d.votesLoadedList = std::move(r.Voters);
                    d.votesLoadedError.clear();
                    d.votesLoadedVoteCount = r.VoteCount;
                    d.votesLoadedHasVoted = r.HasVoted;
                    d.votesLoadedVotersArrayInResponse = r.VotersArrayInResponse;
                } else {
                    d.votesLoadedList.clear();
                    d.votesLoadedError = std::move(r.Error);
                    d.votesLoadedVoteCount = 0;
                    d.votesLoadedHasVoted = false;
                    d.votesLoadedVotersArrayInResponse = false;
                }
            } catch (const std::exception& ex) {
                d.votesLoadInProgress = false;
                d.votesLoadedList.clear();
                d.votesLoadedError = std::string("Failed to load votes: ") + ex.what();
                d.votesLoadedVoteCount = 0;
                d.votesLoadedHasVoted = false;
                d.votesLoadedVotersArrayInResponse = false;
                LOG_ERROR("TrackerGridFieldDisplay: votes future exception issue=%s err=%s",
                          d.votesPopupIssueKey.c_str(), ex.what());
            } catch (...) {
                d.votesLoadInProgress = false;
                d.votesLoadedList.clear();
                d.votesLoadedError = "Failed to load votes.";
                d.votesLoadedVoteCount = 0;
                d.votesLoadedHasVoted = false;
                d.votesLoadedVotersArrayInResponse = false;
                LOG_ERROR("TrackerGridFieldDisplay: votes future unknown exception issue=%s",
                          d.votesPopupIssueKey.c_str());
            }
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
        if (d.votesLoadInProgress) {
            ImGui::TextDisabled("Loading...");
        } else if (!d.votesLoadedError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", d.votesLoadedError.c_str());
            ImGui::PopStyleColor();
        } else {
            std::string summary = std::to_string(d.votesLoadedVoteCount) + " vote";
            if (d.votesLoadedVoteCount != 1) {
                summary += "s";
            }
            if (d.votesLoadedHasVoted) {
                summary += " (you voted)";
            }
            ImGui::TextUnformatted(summary.c_str());
            ImGui::Spacing();
            if (d.votesLoadedList.empty()) {
                if (d.votesLoadedVoteCount == 0) {
                    ImGui::TextDisabled("No votes.");
                } else if (!d.votesLoadedVotersArrayInResponse) {
                    ImGui::TextDisabled("Voter names are hidden by Tracker permissions (View voters and watchers).");
                } else {
                    ImGui::TextDisabled("No voters to list.");
                }
            } else {
                ImGui::BeginChild("VotesList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
                for (const auto& w : d.votesLoadedList) {
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
