#ifndef SMATCHET_USER_INFO_UI_H
#define SMATCHET_USER_INFO_UI_H

#include "ConfigManager.h"
#include "ITrackerActivity.h"
#include "TrackerFieldSchema.h"
#include "Vcs/VcsSubmission.h"

#include <future>
#include <map>
#include <string>
#include <vector>

class AppController;
struct UiDrawSession;

/**
 * Dockable "User Info" window: identity + recent VCS submissions (p4 + git) +
 * tracker activity + group membership for one tracker user, opened from the
 * grid's right-click menu on user-type cells
 * (docs/plans/user-info-window.md, Slice 5).
 *
 * Threading: every fetch runs on a std::async worker; results are adopted on
 * the UI thread in pollFutures with a generation check so a retarget
 * mid-flight never mixes payloads. An in-flight future is never reassigned
 * (its destructor would block the UI thread) — retargets set a relaunch flag
 * that pollFutures consumes after the old worker drains.
 */
class SmatchetUserInfoUi {
  public:
    /// Stage an open/retarget request on the session; DrawWindow consumes it
    /// next frame via adoptPendingRequest. Static so grid popup call sites
    /// don't need the SmatchetUI instance.
    static void Open(UiDrawSession& d, const std::string& sourcePaneId, const std::string& displayName,
                     const std::string& email, const std::string& accountId);

    /// Per-frame draw. The host (SmatchetUI::drawSecondaryWindows) does
    /// prepareTopLevelWindow + the focus latch; this does Begin/End, the
    /// lifecycle (Escape-to-close, close cleanup), and the sections.
    void DrawWindow(AppController& app, UiDrawSession& d, bool* open);

  private:
    struct VcsPayload {
        std::vector<Vcs::VcsSubmission> P4Rows;
        std::vector<Vcs::VcsSubmission> GitRows;
        std::vector<Vcs::VcsSubmission> Unified;
        std::string P4Error;
        std::string GitError;
        int Gen = 0;
    };
    struct ActivityPayload {
        std::vector<TrackerActivityEntry> Entries;
        std::string Error;
        int Gen = 0;
    };
    struct GroupsPayload {
        std::vector<std::string> Names;
        std::string Error;
        int Gen = 0;
    };
    struct MembersPayload {
        std::string GroupName;
        std::vector<TrackerUser> Members;
        std::string Error;
        int Gen = 0;
    };

    // Lifecycle + fetch plumbing (SmatchetUserInfoUi.cpp).
    void adoptPendingRequest(AppController& app, UiDrawSession& d);
    void launchVcsFetch(UiDrawSession& d);
    void launchActivityFetch(AppController& app);
    void launchGroupsFetch(AppController& app);
    void launchMembersFetch(AppController& app, const std::string& groupName);
    void pollFutures(AppController& app, UiDrawSession& d);
    void pollVcsFuture(UiDrawSession& d);
    void pollActivityFuture(AppController& app);
    void pollGroupsFuture(AppController& app);
    void pollMembersFuture();
    void closeCleanup(AppController& app);
    /// UTC calendar date `daysBack` days before now as `YYYY-MM-DD`; "" on a
    /// (never-expected) gmtime/strftime failure.
    static std::string IsoDateUtcDaysAgo(int daysBack);

    // Section draws (SmatchetUserInfoUi_Sections.cpp).
    void drawIdentitySection();
    void drawVcsSection(AppController& app, UiDrawSession& d, bool narrow);
    void drawVcsRows(AppController& app, UiDrawSession& d, const std::vector<Vcs::VcsSubmission>& rows,
                     const char* idPrefix, bool narrow);
    void drawVcsRow(AppController& app, UiDrawSession& d, const Vcs::VcsSubmission& row, bool narrow);
    void activateVcsRow(AppController& app, const Vcs::VcsSubmission& row);
    void drawActivitySection(AppController& app, UiDrawSession& d, bool narrow);
    void drawActivityRow(AppController& app, UiDrawSession& d, const TrackerActivityEntry& entry, bool narrow);
    void drawGroupsSection(AppController& app, UiDrawSession& d);
    void drawGroupMembers(AppController& app, const std::string& groupName);
    void drawIssueKeyContextMenu(AppController& app, UiDrawSession& d, const char* popupId, const std::string& issueKey,
                                 const std::string& issueUrl);

    // Target identity (adopted from the session request).
    std::string paneId_;
    std::string displayName_;
    std::string email_;
    std::string accountId_;
    bool supportsActivity_ = false;
    /// Bumped on every retarget; stale worker payloads (older Gen) are dropped.
    int generation_ = 0;

    // In-flight futures + their UI-side flags.
    std::future<VcsPayload> vcsFuture_;
    std::future<ActivityPayload> activityFuture_;
    std::future<GroupsPayload> groupsFuture_;
    std::future<MembersPayload> membersFuture_;
    bool vcsLoading_ = false;
    bool activityLoading_ = false;
    bool groupsLoading_ = false;
    bool membersLoading_ = false;
    /// Group the in-flight members fetch targets (for the per-node loading label).
    std::string membersFetchGroup_;
    // Retarget arrived while the matching future was in flight — relaunch after drain.
    bool vcsRelaunch_ = false;
    bool activityRelaunch_ = false;
    bool groupsRelaunch_ = false;

    // Adopted (current-generation) payloads.
    VcsPayload vcs_;
    ActivityPayload activity_;
    GroupsPayload groups_;
    bool vcsLoaded_ = false;
    bool activityLoaded_ = false;
    bool groupsLoaded_ = false;

    /// One-shot per-group member rosters (cleared on retarget). A group lands in
    /// exactly one of the two maps once its fetch completes.
    std::map<std::string, std::vector<TrackerUser>> membersCache_;
    std::map<std::string, std::string> membersErrors_;

    /// Worker-incremented progress for the activity scan (non-copyable; the
    /// worker holds a pointer — safe because this object is app-lifetime and
    /// relaunch waits for the previous worker to drain).
    TrackerActivityProgress activityProgress_;
    /// Snapshot for p4 fetches + the CL tooltip/p4vc launch (re-read on retarget).
    AnnotateAnalysisConfig annotateCfg_;
    /// UI-side day filter for the activity list (clamped to cfg.UserActivityDayWindow).
    int activityDayFilter_ = 30;
    /// Whether the window was visible last frame (drives close-edge cleanup).
    bool wasOpen_ = false;
};

#endif
