// User Info window — section draws (identity / VCS feed / tracker activity /
// groups). Lifecycle + fetch plumbing live in SmatchetUserInfoUi.cpp
// (docs/plans/user-info-window.md, Slice 5).

#include "Ui/SmatchetUserInfoUi.h"

#include "AppController.h"
#include "SmatchetUiSession.h"
#include "StringUtil.h"
#include "Tracker/ProjectResolver.h"
#include "Ui/P4ClPreview.h"
#include "Ui/P4vLaunch.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <string>

namespace {

const ImVec4 kSectionErrorColor(1.0f, 0.6f, 0.35f, 1.0f);
const ImVec4 kProductionGroupColor(0.55f, 0.9f, 0.55f, 1.0f);

} // namespace

void SmatchetUserInfoUi::drawIdentitySection() {
    ImGui::Text("%s", displayName_.empty() ? accountId_.c_str() : displayName_.c_str());
    if (!email_.empty()) {
        ImGui::TextDisabled("%s", email_.c_str());
    }
    if (!accountId_.empty() && accountId_ != displayName_) {
        ImGui::TextDisabled("ID: %s", accountId_.c_str());
    }
    if (!supportsActivity_) {
        ImGui::TextDisabled("Tracker activity/groups: not supported by this backend.");
    }
    ImGui::Separator();
}

void SmatchetUserInfoUi::drawVcsSection(AppController& app, UiDrawSession& d, bool narrow) {
    if (!ImGui::CollapsingHeader("Recent submissions", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    const bool unified = d.cfg.VcsFeedLayout != "separate";
    if (ImGui::RadioButton("Unified", unified) && !unified) {
        d.cfg.VcsFeedLayout = "unified";
        MarkPrefsDirty(d);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Separate", !unified) && unified) {
        d.cfg.VcsFeedLayout = "separate";
        MarkPrefsDirty(d);
    }
    if (vcsLoading_) {
        ImGui::TextDisabled("Loading submissions...");
        return;
    }
    if (!vcs_.P4Error.empty()) {
        ImGui::TextColored(kSectionErrorColor, "Perforce: %s", vcs_.P4Error.c_str());
    }
    if (!vcs_.GitError.empty()) {
        ImGui::TextColored(kSectionErrorColor, "Git: %s", vcs_.GitError.c_str());
    }
    if (unified) {
        drawVcsRows(app, d, vcs_.Unified, "vcs", narrow);
        return;
    }
    // Separate layout: each VCS gets its own collapsible sub-header with a
    // separator between them (requested UX — fold p4 or git independently).
    if (ImGui::CollapsingHeader("Perforce", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawVcsRows(app, d, vcs_.P4Rows, "p4", narrow);
    }
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Git", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawVcsRows(app, d, vcs_.GitRows, "git", narrow);
    }
}

void SmatchetUserInfoUi::drawVcsRows(AppController& app, UiDrawSession& d, const std::vector<Vcs::VcsSubmission>& rows,
                                     const char* idPrefix, bool narrow) {
    if (rows.empty()) {
        if (vcsLoaded_) {
            ImGui::TextDisabled("(no submissions in window)");
        }
        return;
    }
    ImGui::PushID(idPrefix);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        ImGui::PushID(i);
        drawVcsRow(app, d, rows[static_cast<size_t>(i)], narrow);
        ImGui::PopID();
    }
    ImGui::PopID();
}

void SmatchetUserInfoUi::drawVcsRow(AppController& app, UiDrawSession& d, const Vcs::VcsSubmission& row, bool narrow) {
    const bool isP4 = row.Source == Vcs::VcsSource::P4;
    const char* tag = isP4 ? "[p4]" : "[git]";
    const std::string shortId = isP4 ? row.Id : row.Id.substr(0, 9);
    const std::string date = row.Timestamp.size() >= 10 ? row.Timestamp.substr(0, 10) : row.Timestamp;
    std::string label;
    float rowHeight = 0.0f;
    if (narrow) {
        // Two-line stacked row with a ~40px touch target (mobile-first, Slice 5).
        label = std::string(tag) + " " + shortId + "  " + date + "  " + row.Author + "\n" + row.FirstLine;
        rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
    } else {
        label = std::string(tag) + " " + shortId + "  " + date + "  " + row.Author + "  " + row.FirstLine;
    }
    if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0.0f, rowHeight))) {
        activateVcsRow(app, row);
    }
    if (isP4 && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        P4ClPreview::DrawClTooltipAsync(row.Id, annotateCfg_, annotateCfg_.UiColors);
    }
    const smatchet::IssueKeyMatch match = smatchet::FindFirstIssueKeyInText(row.FirstLine);
    if (!match.Key.empty()) {
        drawIssueKeyContextMenu(app, d, "vcs_row_menu", match.Key, std::string());
    }
}

void SmatchetUserInfoUi::activateVcsRow(AppController& app, const Vcs::VcsSubmission& row) {
    if (row.Source == Vcs::VcsSource::P4) {
        P4vLaunch::LaunchP4VcLike(annotateCfg_, std::string(), std::string(), /*isTimelapse=*/false, std::string(), 0,
                                  row.Id);
        return;
    }
    if (!row.Url.empty()) {
        app.OpenUrl(row.Url);
    }
}

void SmatchetUserInfoUi::drawActivitySection(AppController& app, UiDrawSession& d, bool narrow) {
    if (!supportsActivity_) {
        return;
    }
    if (!ImGui::CollapsingHeader("Tracker activity", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::BeginDisabled(activityLoading_);
    if (ImGui::Button(activityLoaded_ ? "Reload" : "Load")) {
        launchActivityFetch(app);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    const int maxDays = d.cfg.UserActivityDayWindow > 0 ? d.cfg.UserActivityDayWindow : 30;
    if (ImGui::InputInt("days", &activityDayFilter_)) {
        if (activityDayFilter_ < 1) {
            activityDayFilter_ = 1;
        }
        if (activityDayFilter_ > maxDays) {
            activityDayFilter_ = maxDays;
        }
    }
    if (activityLoading_) {
        const int total = activityProgress_.Total.load();
        const int current = activityProgress_.Current.load();
        const float fraction = total > 0 ? static_cast<float>(current) / static_cast<float>(total) : 0.0f;
        const std::string overlay = std::to_string(current) + " / " + std::to_string(total);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay.c_str());
        return;
    }
    if (!activity_.Error.empty()) {
        ImGui::TextColored(kSectionErrorColor, "%s", activity_.Error.c_str());
        return;
    }
    // Tightening the day filter re-filters the already-fetched window per frame
    // (string compare on the ISO prefix); widening past the fetch needs Reload.
    const std::string cutoff = IsoDateUtcDaysAgo(activityDayFilter_);
    int shown = 0;
    for (int i = 0; i < static_cast<int>(activity_.Entries.size()); ++i) {
        const TrackerActivityEntry& entry = activity_.Entries[static_cast<size_t>(i)];
        if (entry.Timestamp.size() >= 10 && entry.Timestamp.substr(0, 10) < cutoff) {
            continue;
        }
        ImGui::PushID(i);
        drawActivityRow(app, d, entry, narrow);
        ImGui::PopID();
        ++shown;
    }
    if (shown == 0 && activityLoaded_) {
        ImGui::TextDisabled("(no activity in window)");
    }
}

void SmatchetUserInfoUi::drawActivityRow(AppController& app, UiDrawSession& d, const TrackerActivityEntry& entry,
                                         bool narrow) {
    const std::string stamp = entry.Timestamp.size() >= 16 ? entry.Timestamp.substr(0, 16) : entry.Timestamp;
    const std::string detail = entry.Details.empty() ? entry.Summary : entry.Details;
    std::string label;
    float rowHeight = 0.0f;
    if (narrow) {
        label = stamp + "  " + entry.IssueKey + "\n" + entry.ActionLabel + ": " + detail;
        rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
    } else {
        label = stamp + "  " + entry.IssueKey + "  " + entry.ActionLabel + "  " + detail;
    }
    ImGui::Selectable(label.c_str(), false, 0, ImVec2(0.0f, rowHeight));
    if (!entry.IssueKey.empty()) {
        drawIssueKeyContextMenu(app, d, "act_row_menu", entry.IssueKey, entry.IssueUrl);
    }
}

void SmatchetUserInfoUi::drawGroupsSection(AppController& app, UiDrawSession& d) {
    if (!supportsActivity_) {
        return;
    }
    if (!ImGui::CollapsingHeader("Groups", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (groupsLoading_) {
        ImGui::TextDisabled("Loading groups...");
        return;
    }
    if (!groups_.Error.empty()) {
        ImGui::TextColored(kSectionErrorColor, "%s", groups_.Error.c_str());
        return;
    }
    if (groups_.Names.empty()) {
        if (groupsLoaded_) {
            ImGui::TextDisabled("(no groups)");
        }
        return;
    }
    const std::string& keyword = d.cfg.ProductionGroupKeyword;
    for (int i = 0; i < static_cast<int>(groups_.Names.size()); ++i) {
        const std::string& name = groups_.Names[static_cast<size_t>(i)];
        const bool isProduction = !keyword.empty() && ContainsCaseInsensitive(name, keyword);
        ImGui::PushID(i);
        if (isProduction) {
            ImGui::PushStyleColor(ImGuiCol_Text, kProductionGroupColor);
        }
        const std::string label = isProduction ? name + "  (production)" : name;
        const bool nodeOpen = ImGui::TreeNode(label.c_str());
        if (isProduction) {
            ImGui::PopStyleColor();
        }
        if (nodeOpen) {
            drawGroupMembers(app, name);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void SmatchetUserInfoUi::drawGroupMembers(AppController& app, const std::string& groupName) {
    const std::map<std::string, std::vector<TrackerUser>>::const_iterator hit = membersCache_.find(groupName);
    if (hit != membersCache_.end()) {
        if (hit->second.empty()) {
            ImGui::TextDisabled("(no members)");
            return;
        }
        for (size_t i = 0; i < hit->second.size(); ++i) {
            const TrackerUser& member = hit->second[i];
            ImGui::BulletText("%s", member.DisplayName.empty() ? member.AccountId.c_str() : member.DisplayName.c_str());
        }
        return;
    }
    const std::map<std::string, std::string>::const_iterator errHit = membersErrors_.find(groupName);
    if (errHit != membersErrors_.end()) {
        ImGui::TextColored(kSectionErrorColor, "%s", errHit->second.c_str());
        return;
    }
    // One members fetch at a time — other expanded groups queue until it drains
    // (re-attempted here every frame).
    launchMembersFetch(app, groupName);
    if (membersLoading_ && membersFetchGroup_ == groupName) {
        ImGui::TextDisabled("Loading members...");
    } else {
        ImGui::TextDisabled("Queued...");
    }
}

void SmatchetUserInfoUi::drawIssueKeyContextMenu(AppController& app, UiDrawSession& d, const char* popupId,
                                                 const std::string& issueKey, const std::string& issueUrl) {
    if (!ImGui::BeginPopupContextItem(popupId)) {
        return;
    }
    ImGui::TextDisabled("%s", issueKey.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Add to view query")) {
        if (d.onUserInfoAddToQuery) {
            d.onUserInfoAddToQuery(paneId_, issueKey);
        }
    }
    if (ImGui::MenuItem("Open in browser")) {
        const std::string url = !issueUrl.empty() ? issueUrl : app.BuildIssueBrowseUrl(d.cfg, issueKey);
        if (!url.empty()) {
            app.OpenUrl(url);
        }
    }
    if (ImGui::MenuItem("Copy key")) {
        ImGui::SetClipboardText(issueKey.c_str());
    }
    ImGui::EndPopup();
}
