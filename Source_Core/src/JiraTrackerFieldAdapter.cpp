#include "JiraTrackerFieldAdapter.h"

namespace JiraTrackerFieldAdapter {

TrackerFieldFamily ToTrackerFamily(JiraFieldFamily family) {
    return static_cast<TrackerFieldFamily>(static_cast<int>(family));
}

JiraFieldFamily ToJiraFamily(TrackerFieldFamily family) {
    return static_cast<JiraFieldFamily>(static_cast<int>(family));
}

TrackerFieldOption ToTrackerOption(const JiraFieldOption& option) {
    TrackerFieldOption out;
    out.Id = option.Id;
    out.Value = option.Value;
    out.SecondaryValue = option.SecondaryValue;
    out.PayloadJson = option.PayloadJson;
    out.Disabled = option.Disabled;
    out.Children = std::vector<TrackerFieldOption>();
    out.Children.reserve(option.Children.size());
    for (const auto& child : option.Children) {
        out.Children.push_back(ToTrackerOption(child));
    }
    return out;
}

JiraFieldOption ToJiraOption(const TrackerFieldOption& option) {
    JiraFieldOption out;
    out.Id = option.Id;
    out.Value = option.Value;
    out.SecondaryValue = option.SecondaryValue;
    out.PayloadJson = option.PayloadJson;
    out.Disabled = option.Disabled;
    out.Children.reserve(option.Children.size());
    for (const auto& child : option.Children) {
        out.Children.push_back(ToJiraOption(child));
    }
    return out;
}

TrackerField ToTrackerField(const JiraField& field) {
    TrackerField out;
    out.Id = field.Id;
    out.Name = field.Name;
    out.Type = field.Type;
    out.SchemaSystem = field.SchemaSystem;
    out.SchemaCustom = field.SchemaCustom;
    out.ReadOnly = field.ReadOnly;
    out.IsArray = field.IsArray;
    out.ItemsType = field.ItemsType;
    out.IsUserType = field.IsUserType;
    out.IsCustom = field.IsCustom;
    out.Family = ToTrackerFamily(field.Family);
    out.AllowedValues = field.AllowedValues;
    out.RawFieldDefinitionJson = field.RestFieldDefinitionJson;
    out.AllowedValueOptions.reserve(field.AllowedValueOptions.size());
    for (const auto& option : field.AllowedValueOptions) {
        out.AllowedValueOptions.push_back(ToTrackerOption(option));
    }
    return out;
}

JiraField ToJiraField(const TrackerField& field) {
    JiraField out;
    out.Id = field.Id;
    out.Name = field.Name;
    out.Type = field.Type;
    out.SchemaSystem = field.SchemaSystem;
    out.SchemaCustom = field.SchemaCustom;
    out.ReadOnly = field.ReadOnly;
    out.IsArray = field.IsArray;
    out.ItemsType = field.ItemsType;
    out.IsUserType = field.IsUserType;
    out.IsCustom = field.IsCustom;
    out.Family = ToJiraFamily(field.Family);
    out.AllowedValues = field.AllowedValues;
    out.RestFieldDefinitionJson = field.RawFieldDefinitionJson;
    out.AllowedValueOptions.reserve(field.AllowedValueOptions.size());
    for (const auto& option : field.AllowedValueOptions) {
        out.AllowedValueOptions.push_back(ToJiraOption(option));
    }
    return out;
}

TrackerComponent ToTrackerComponent(const JiraComponent& component) {
    return TrackerComponent{component.Id, component.Name};
}

JiraComponent ToJiraComponent(const TrackerComponent& component) { return JiraComponent{component.Id, component.Name}; }

TrackerUser ToTrackerUser(const JiraUser& user) {
    return TrackerUser{user.AccountId, user.DisplayName, user.EmailAddress, user.Active};
}

JiraUser ToJiraUser(const TrackerUser& user) {
    return JiraUser{user.AccountId, user.DisplayName, user.EmailAddress, user.Active};
}

TrackerIssueTypeCreateMeta ToTrackerIssueTypeMeta(const IssueTypeCreateMeta& meta) {
    TrackerIssueTypeCreateMeta out;
    out.ProjectKey = meta.ProjectKey;
    out.IssueTypeId = meta.IssueTypeId;
    out.IssueTypeName = meta.IssueTypeName;
    out.IsSubtask = meta.IsSubtask;
    out.RequiredFieldIds = meta.RequiredFieldIds;
    return out;
}

IssueTypeCreateMeta ToJiraIssueTypeMeta(const TrackerIssueTypeCreateMeta& meta) {
    IssueTypeCreateMeta out;
    out.ProjectKey = meta.ProjectKey;
    out.IssueTypeId = meta.IssueTypeId;
    out.IssueTypeName = meta.IssueTypeName;
    out.IsSubtask = meta.IsSubtask;
    out.RequiredFieldIds = meta.RequiredFieldIds;
    return out;
}

std::vector<TrackerField> ToTrackerFields(const std::vector<JiraField>& fields) {
    std::vector<TrackerField> out;
    out.reserve(fields.size());
    for (const auto& field : fields) {
        out.push_back(ToTrackerField(field));
    }
    return out;
}

std::vector<JiraField> ToJiraFields(const std::vector<TrackerField>& fields) {
    std::vector<JiraField> out;
    out.reserve(fields.size());
    for (const auto& field : fields) {
        out.push_back(ToJiraField(field));
    }
    return out;
}

std::vector<TrackerComponent> ToTrackerComponents(const std::vector<JiraComponent>& components) {
    std::vector<TrackerComponent> out;
    out.reserve(components.size());
    for (const auto& component : components) {
        out.push_back(ToTrackerComponent(component));
    }
    return out;
}

std::vector<JiraComponent> ToJiraComponents(const std::vector<TrackerComponent>& components) {
    std::vector<JiraComponent> out;
    out.reserve(components.size());
    for (const auto& component : components) {
        out.push_back(ToJiraComponent(component));
    }
    return out;
}

std::vector<TrackerUser> ToTrackerUsers(const std::vector<JiraUser>& users) {
    std::vector<TrackerUser> out;
    out.reserve(users.size());
    for (const auto& user : users) {
        out.push_back(ToTrackerUser(user));
    }
    return out;
}

std::vector<JiraUser> ToJiraUsers(const std::vector<TrackerUser>& users) {
    std::vector<JiraUser> out;
    out.reserve(users.size());
    for (const auto& user : users) {
        out.push_back(ToJiraUser(user));
    }
    return out;
}

std::vector<TrackerIssueTypeCreateMeta> ToTrackerIssueTypeMeta(const std::vector<IssueTypeCreateMeta>& meta) {
    std::vector<TrackerIssueTypeCreateMeta> out;
    out.reserve(meta.size());
    for (const auto& entry : meta) {
        out.push_back(ToTrackerIssueTypeMeta(entry));
    }
    return out;
}

std::vector<IssueTypeCreateMeta> ToJiraIssueTypeMeta(const std::vector<TrackerIssueTypeCreateMeta>& meta) {
    std::vector<IssueTypeCreateMeta> out;
    out.reserve(meta.size());
    for (const auto& entry : meta) {
        out.push_back(ToJiraIssueTypeMeta(entry));
    }
    return out;
}

} // namespace JiraTrackerFieldAdapter
