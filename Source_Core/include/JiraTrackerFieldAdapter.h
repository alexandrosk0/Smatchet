#pragma once

#include "JiraClient.h"
#include "TrackerFieldSchema.h"

#include <vector>

namespace JiraTrackerFieldAdapter {

TrackerFieldFamily ToTrackerFamily(JiraFieldFamily family);
JiraFieldFamily ToJiraFamily(TrackerFieldFamily family);

TrackerFieldOption ToTrackerOption(const JiraFieldOption& option);
JiraFieldOption ToJiraOption(const TrackerFieldOption& option);

TrackerField ToTrackerField(const JiraField& field);
JiraField ToJiraField(const TrackerField& field);

TrackerComponent ToTrackerComponent(const JiraComponent& component);
JiraComponent ToJiraComponent(const TrackerComponent& component);

TrackerUser ToTrackerUser(const JiraUser& user);
JiraUser ToJiraUser(const TrackerUser& user);

TrackerIssueTypeCreateMeta ToTrackerIssueTypeMeta(const IssueTypeCreateMeta& meta);
IssueTypeCreateMeta ToJiraIssueTypeMeta(const TrackerIssueTypeCreateMeta& meta);

std::vector<TrackerField> ToTrackerFields(const std::vector<JiraField>& fields);
std::vector<JiraField> ToJiraFields(const std::vector<TrackerField>& fields);
std::vector<TrackerComponent> ToTrackerComponents(const std::vector<JiraComponent>& components);
std::vector<JiraComponent> ToJiraComponents(const std::vector<TrackerComponent>& components);
std::vector<TrackerUser> ToTrackerUsers(const std::vector<JiraUser>& users);
std::vector<JiraUser> ToJiraUsers(const std::vector<TrackerUser>& users);
std::vector<TrackerIssueTypeCreateMeta> ToTrackerIssueTypeMeta(const std::vector<IssueTypeCreateMeta>& meta);
std::vector<IssueTypeCreateMeta> ToJiraIssueTypeMeta(const std::vector<TrackerIssueTypeCreateMeta>& meta);

} // namespace JiraTrackerFieldAdapter
