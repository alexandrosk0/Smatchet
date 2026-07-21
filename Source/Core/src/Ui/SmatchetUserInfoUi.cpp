// User Info window — lifecycle + async fetch plumbing. Section draws live in
// SmatchetUserInfoUi_Sections.cpp (docs/plans/user-info-window.md, Slice 5).

#include "Ui/SmatchetUserInfoUi.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "P4Annotate.h"
#include "SmatchetUiSession.h"
#include "Ui/P4ClPreview.h"
#include "Ui/SmatchetLayoutBreakpoints.h"
#include "Vcs/GitHubCommits.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <chrono>
#include <ctime>
#include <utility>

std::string SmatchetUserInfoUi::IsoDateUtcDaysAgo(int daysBack) {
    const std::time_t t = std::time(nullptr) - static_cast<std::time_t>(daysBack) * 86400;
    std::tm tmv = {};
#if defined(_WIN32)
    if (gmtime_s(&tmv, &t) != 0) {
        return std::string();
    }
#else
    if (gmtime_r(&t, &tmv) == nullptr) {
        return std::string();
    }
#endif
    char buf[16] = {};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv) == 0) {
        return std::string();
    }
    return std::string(buf);
}

void SmatchetUserInfoUi::Open(UiDrawSession& d, const std::string& sourcePaneId, const std::string& displayName,
                              const std::string& email, const std::string& accountId) {
    d.userInfoSourcePaneId = sourcePaneId;
    d.userInfoDisplayName = displayName;
    d.userInfoEmail = email;
    d.userInfoAccountId = accountId;
    d.userInfoRequestPending = true;
    d.showUserInfo = true;
    d.requestUserInfoFocus = true;
}

void SmatchetUserInfoUi::DrawWindow(AppController& app, UiDrawSession& d, bool* open) {
    // Always poll, even hidden: in-flight workers must drain (and relaunch
    // flags fire) regardless of window visibility.
    pollFutures(app, d);
    if (open == nullptr || !*open) {
        if (wasOpen_) {
            closeCleanup(app);
            wasOpen_ = false;
        }
        return;
    }
    wasOpen_ = true;
    if (d.userInfoRequestPending) {
        adoptPendingRequest(app, d);
    }
    if (!ImGui::Begin("User Info", open)) {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        *open = false;
    }
    const bool narrow = ImGui::GetContentRegionAvail().x < smatchet::ui::kNarrowLayoutWidthPx;
    drawIdentitySection();
    drawVcsSection(app, d, narrow);
    drawActivitySection(app, d, narrow);
    drawGroupsSection(app, d);
    ImGui::End();
    if (!*open) {
        closeCleanup(app);
        wasOpen_ = false;
    }
}

void SmatchetUserInfoUi::adoptPendingRequest(AppController& app, UiDrawSession& d) {
    d.userInfoRequestPending = false;
    appForShutdownCancel_ = &app;
    if (!paneId_.empty()) {
        // Cancel the previous target's in-flight activity scan before retargeting.
        app.ClearPaneUserActivity(paneId_);
    }
    // WS-A: signal any abandoned workers from the previous target, then hand the
    // new workers a fresh (un-cancelled) flag. Workers still holding the OLD token
    // copy keep observing the cancelled flag and stop; the relaunched ones run
    // against the new flag.
    cancel_.Cancel();
    cancel_.Reset();
    paneId_ = d.userInfoSourcePaneId;
    displayName_ = d.userInfoDisplayName;
    email_ = d.userInfoEmail;
    accountId_ = d.userInfoAccountId;
    ++generation_;
    vcs_ = VcsPayload();
    activity_ = ActivityPayload();
    groups_ = GroupsPayload();
    vcsLoaded_ = false;
    activityLoaded_ = false;
    groupsLoaded_ = false;
    membersCache_.clear();
    membersErrors_.clear();
    annotateCfg_ = ConfigManager::LoadAnnotateAnalysis();
    supportsActivity_ = app.PaneSupportsActivity(paneId_);
    activityDayFilter_ = d.cfg.UserActivityDayWindow > 0 ? d.cfg.UserActivityDayWindow : 30;
    if (vcsLoading_) {
        vcsRelaunch_ = true;
    } else {
        launchVcsFetch(d);
    }
    if (supportsActivity_) {
        if (activityLoading_) {
            activityRelaunch_ = true;
        } else {
            launchActivityFetch(app);
        }
        if (groupsLoading_) {
            groupsRelaunch_ = true;
        } else {
            launchGroupsFetch(app);
        }
    }
}

void SmatchetUserInfoUi::launchVcsFetch(UiDrawSession& d) {
    if (vcsLoading_) {
        return;
    }
    vcsLoading_ = true;
    const int gen = generation_;
    const int maxN = d.cfg.MaxUserChanges > 0 ? d.cfg.MaxUserChanges : 50;
    const int dayWindow = d.cfg.UserActivityDayWindow > 0 ? d.cfg.UserActivityDayWindow : 30;
    const std::string cutoff = IsoDateUtcDaysAgo(dayWindow);
    const std::string email = email_;
    // GitHub's ?author= accepts an email or a login — fall back to the account
    // id (the login on GitHub panes) when the tracker has no email.
    const std::string gitAuthor = email_.empty() ? accountId_ : email_;
    const AnnotateAnalysisConfig annotateCfg = annotateCfg_;
    const TrackerConfig cfg = d.cfg;
    const smatchet::ui::CancelToken cancel = cancel_;
    vcsFuture_ = std::async(std::launch::async, [gen, maxN, cutoff, email, gitAuthor, annotateCfg, cfg, cancel]() {
        VcsPayload p;
        p.Gen = gen;
        // WS-A cooperative cancel: bail before the p4 round-trip if already abandoned.
        if (cancel.IsCancelled()) {
            return p;
        }
        // Prefer the authoritative email->login map from `p4 users` (the p4 login often
        // differs from the email local-part, e.g. "alexk" for "alexkonstantonis@gmail.com");
        // fall back to the naive local-part strip when p4 has no matching user.
        std::string p4User = P4UserForEmail(annotateCfg, email);
        if (p4User.empty()) {
            p4User = Vcs::P4UserFromEmail(email);
        }
        if (p4User.empty()) {
            p.P4Error = "No Perforce user (email unknown).";
        } else {
            Result<std::vector<P4ChangeSummary>> changesResult = P4ChangesForUser(annotateCfg, p4User, maxN);
            if (changesResult.has_value()) {
                const std::vector<P4ChangeSummary>& changes = changesResult.value();
                for (size_t i = 0; i < changes.size(); ++i) {
                    Vcs::VcsSubmission row = Vcs::FromP4Change(changes[i]);
                    if (Vcs::KeepOnOrAfterCutoff(row.Timestamp, cutoff)) {
                        p.P4Rows.push_back(std::move(row));
                    }
                }
            } else {
                p.P4Error = changesResult.error().empty() ? "p4 changes failed." : changesResult.error();
            }
        }
        // WS-A: skip the second (GitHub) round-trip when cancelled between IO chunks.
        if (cancel.IsCancelled()) {
            return p;
        }
        std::vector<Vcs::VcsSubmission> commits;
        std::string gitErr;
        if (Vcs::GitHubCommitsForUser(cfg, gitAuthor, maxN, commits, gitErr)) {
            for (size_t i = 0; i < commits.size(); ++i) {
                if (Vcs::KeepOnOrAfterCutoff(commits[i].Timestamp, cutoff)) {
                    p.GitRows.push_back(std::move(commits[i]));
                }
            }
            // Per-repo failures are non-fatal — keep partial rows, surface the note.
            p.GitError = gitErr;
        } else {
            p.GitError = gitErr.empty() ? "Git commit feed unavailable." : gitErr;
        }
        p.Unified = Vcs::MergeFeedsNewestFirst(p.P4Rows, p.GitRows);
        return p;
    });
}

void SmatchetUserInfoUi::launchActivityFetch(AppController& app) {
    if (activityLoading_ || !supportsActivity_) {
        return;
    }
    activityLoading_ = true;
    // Previous worker has drained (activityLoading_ was false) — safe to reset.
    activityProgress_.Current.store(0);
    activityProgress_.Total.store(0);
    const int gen = generation_;
    const std::string dayFrom = IsoDateUtcDaysAgo(activityDayFilter_ > 0 ? activityDayFilter_ : 30);
    const std::string dayTo = IsoDateUtcDaysAgo(0);
    const std::string paneId = paneId_;
    const std::string accountId = accountId_;
    AppController* appPtr = &app;
    TrackerActivityProgress* progress = &activityProgress_;
    const smatchet::ui::CancelToken cancel = cancel_;
    activityFuture_ =
        std::async(std::launch::async, [gen, dayFrom, dayTo, paneId, accountId, appPtr, progress, cancel]() {
            ActivityPayload p;
            p.Gen = gen;
            // WS-A (Pillar-3): if cancel was signalled before the worker started, return
            // WITHOUT dereferencing appPtr — the multi-second p4-annotate scan is the
            // #1150 shutdown-stall case. A cancel that lands mid-scan is delivered through
            // the backend's ClearUserActivity (signalled alongside this token by the
            // destructor / closeCleanup), which unblocks FetchPaneUserActivity from inside.
            if (cancel.IsCancelled()) {
                return p;
            }
            std::string err;
            if (!appPtr->FetchPaneUserActivity(paneId, accountId, dayFrom, dayTo, std::string(), *progress, p.Entries,
                                               err)) {
                p.Error = err.empty() ? "Activity fetch failed." : err;
            }
            return p;
        });
}

void SmatchetUserInfoUi::launchGroupsFetch(AppController& app) {
    if (groupsLoading_ || !supportsActivity_) {
        return;
    }
    groupsLoading_ = true;
    const int gen = generation_;
    const std::string accountId = accountId_;
    AppController* appPtr = &app;
    const smatchet::ui::CancelToken cancel = cancel_;
    groupsFuture_ = std::async(std::launch::async, [gen, accountId, appPtr, cancel]() {
        GroupsPayload p;
        p.Gen = gen;
        // WS-A (Pillar-3): bail before dereferencing appPtr if cancelled.
        if (cancel.IsCancelled()) {
            return p;
        }
        Result<std::vector<std::string>> r = appPtr->FetchUserGroupNames(accountId);
        if (r.has_value()) {
            p.Names = std::move(r.value());
        } else {
            p.Error = r.error().empty() ? "Group lookup failed." : r.error();
        }
        return p;
    });
}

void SmatchetUserInfoUi::launchMembersFetch(AppController& app, const std::string& groupName) {
    if (membersLoading_ || membersCache_.count(groupName) != 0 || membersErrors_.count(groupName) != 0) {
        return;
    }
    membersLoading_ = true;
    membersFetchGroup_ = groupName;
    const int gen = generation_;
    const std::string paneId = paneId_;
    AppController* appPtr = &app;
    const smatchet::ui::CancelToken cancel = cancel_;
    membersFuture_ = std::async(std::launch::async, [gen, paneId, groupName, appPtr, cancel]() {
        MembersPayload p;
        p.Gen = gen;
        p.GroupName = groupName;
        // WS-A (Pillar-3): bail before dereferencing appPtr if cancelled.
        if (cancel.IsCancelled()) {
            return p;
        }
        Result<std::vector<TrackerUser>> r = appPtr->FetchPaneGroupMembers(paneId, groupName);
        if (r.has_value()) {
            p.Members = std::move(r.value());
        } else {
            p.Error = r.error().empty() ? "Member lookup failed." : r.error();
        }
        return p;
    });
}

void SmatchetUserInfoUi::pollFutures(AppController& app, UiDrawSession& d) {
    P4ClPreview::ReapDetached();
    pollVcsFuture(d);
    pollActivityFuture(app);
    pollGroupsFuture(app);
    pollMembersFuture();
}

namespace {

/// Shared ready-check for the poll helpers: true when an in-flight future has a
/// result to adopt this frame.
template <typename T> bool FutureReady(bool loading, const std::future<T>& fut) {
    return loading && fut.valid() && fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

} // namespace

void SmatchetUserInfoUi::pollVcsFuture(UiDrawSession& d) {
    if (!FutureReady(vcsLoading_, vcsFuture_)) {
        return;
    }
    try {
        VcsPayload p = vcsFuture_.get();
        if (p.Gen == generation_) {
            vcs_ = std::move(p);
            vcsLoaded_ = true;
        }
    } catch (const std::exception& ex) {
        vcs_.P4Error = ex.what();
        vcsLoaded_ = true;
    } catch (...) { // catch-all-ok: worker exceptions surface as a section error, never crash the UI thread
        vcs_.P4Error = "VCS fetch failed (unknown error).";
        vcsLoaded_ = true;
    }
    vcsLoading_ = false;
    if (vcsRelaunch_) {
        vcsRelaunch_ = false;
        launchVcsFetch(d);
    }
}

void SmatchetUserInfoUi::pollActivityFuture(AppController& app) {
    if (!FutureReady(activityLoading_, activityFuture_)) {
        return;
    }
    try {
        ActivityPayload p = activityFuture_.get();
        if (p.Gen == generation_) {
            activity_ = std::move(p);
            activityLoaded_ = true;
        }
    } catch (const std::exception& ex) {
        activity_.Error = ex.what();
        activityLoaded_ = true;
    } catch (...) { // catch-all-ok: worker exceptions surface as a section error, never crash the UI thread
        activity_.Error = "Activity fetch failed (unknown error).";
        activityLoaded_ = true;
    }
    activityLoading_ = false;
    if (activityRelaunch_) {
        activityRelaunch_ = false;
        launchActivityFetch(app);
    }
}

void SmatchetUserInfoUi::pollGroupsFuture(AppController& app) {
    if (!FutureReady(groupsLoading_, groupsFuture_)) {
        return;
    }
    try {
        GroupsPayload p = groupsFuture_.get();
        if (p.Gen == generation_) {
            groups_ = std::move(p);
            groupsLoaded_ = true;
        }
    } catch (const std::exception& ex) {
        groups_.Error = ex.what();
        groupsLoaded_ = true;
    } catch (...) { // catch-all-ok: worker exceptions surface as a section error, never crash the UI thread
        groups_.Error = "Group lookup failed (unknown error).";
        groupsLoaded_ = true;
    }
    groupsLoading_ = false;
    if (groupsRelaunch_) {
        groupsRelaunch_ = false;
        launchGroupsFetch(app);
    }
}

void SmatchetUserInfoUi::pollMembersFuture() {
    if (!FutureReady(membersLoading_, membersFuture_)) {
        return;
    }
    try {
        MembersPayload p = membersFuture_.get();
        if (p.Gen == generation_) {
            if (p.Error.empty()) {
                membersCache_[p.GroupName] = std::move(p.Members);
            } else {
                LOG_WARN("UserInfoUi: group members fetch failed group=%s: %s", p.GroupName.c_str(), p.Error.c_str());
                membersErrors_[p.GroupName] = p.Error;
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("UserInfoUi: group members fetch threw: %s", ex.what());
    } catch (...) { // catch-all-ok: worker exceptions degrade to a missing roster, never crash the UI thread
        LOG_WARN("UserInfoUi: group members fetch threw (unknown error)");
    }
    membersLoading_ = false;
    membersFetchGroup_.clear();
}

void SmatchetUserInfoUi::closeCleanup(AppController& app) {
    // WS-A: signal cooperative cancel so any in-flight worker stops at its next
    // check; ClearPaneUserActivity additionally unblocks the in-progress p4 scan
    // from inside FetchPaneUserActivity. The futures still drain (their destructors
    // join) but near-instantly. Reset() hands the next open a fresh flag.
    cancel_.Cancel();
    if (!paneId_.empty()) {
        app.ClearPaneUserActivity(paneId_);
    }
    P4ClPreview::DetachInFlight();
    cancel_.Reset();
}

SmatchetUserInfoUi::~SmatchetUserInfoUi() {
    // App-shutdown path: SmatchetUI (owner of this object) is destroyed before
    // AppController, so appForShutdownCancel_ is still valid here. Signal cooperative
    // cancel + the backend's in-scan IO cancel BEFORE the std::async future members
    // destruct (their destructors block until the worker drains) — so the multi-second
    // p4 activity scan returns promptly and teardown does not stall (#1150). The join
    // itself is kept (the future destructors), preserving the no-UAF guarantee: a
    // cancelled worker returns before touching appPtr again, and appPtr outlives this.
    cancel_.Cancel();
    if (appForShutdownCancel_ != nullptr && !paneId_.empty()) {
        appForShutdownCancel_->ClearPaneUserActivity(paneId_);
    }
    // vcsFuture_/activityFuture_/groupsFuture_/membersFuture_ destruct here, joining fast.
}
