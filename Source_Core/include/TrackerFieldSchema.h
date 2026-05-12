#pragma once

#include <string>
#include <unordered_set>
#include <vector>

enum class TrackerFieldFamily {
    Unknown,
    Text,
    Number,
    Date,
    DateTime,
    Labels,
    UserSingle,
    UserMulti,
    SelectSingle,
    SelectMulti,
    CascadingSelect,
    StructuredSingle,
    StructuredMulti,
    Sprint,
    Status,
    IssueType
};

struct TrackerFieldOption {
    std::string Id;
    std::string Value;
    std::string SecondaryValue;
    std::string PayloadJson;
    std::vector<TrackerFieldOption> Children;
    bool Disabled = false;
};

struct TrackerField {
    std::string Id;
    std::string Name;
    std::string Type;
    std::string SchemaSystem;
    std::string SchemaCustom;
    bool ReadOnly = false;
    bool IsArray = false;
    std::string ItemsType;
    bool IsUserType = false;
    bool IsCustom = false;
    /// Plane: per-property `is_required` from the schema. Jira surfaces required-ness
    /// per-issue-type via `TrackerIssueTypeCreateMeta::RequiredFieldIds` instead.
    bool IsRequired = false;
    TrackerFieldFamily Family = TrackerFieldFamily::Unknown;
    std::vector<std::string> AllowedValues;
    std::vector<TrackerFieldOption> AllowedValueOptions;
    std::string RawFieldDefinitionJson;
};

struct TrackerComponent {
    std::string Id;
    std::string Name;
};

struct TrackerUser {
    std::string AccountId;
    std::string DisplayName;
    std::string EmailAddress;
    /// Jira `accountType` from `/rest/api/3/users/search`: "atlassian" (human),
    /// "app" (Connect / Forge integrations), "customer" (Jira Service Management
    /// portal users). Empty when the source backend didn't surface this metadata.
    /// Used by JQL autocomplete to suppress non-human accounts.
    std::string AccountType;
    bool Active = true;
};

struct TrackerIssueTypeCreateMeta {
    std::string ProjectKey;
    std::string IssueTypeId;
    std::string IssueTypeName;
    bool IsSubtask = false;
    std::unordered_set<std::string> RequiredFieldIds;
};

struct TrackerFieldCatalogResult {
    std::vector<TrackerField> Fields;
    std::vector<TrackerComponent> Components;
    std::vector<TrackerIssueTypeCreateMeta> IssueTypeMeta;
    std::vector<TrackerUser> Users;
    std::string Warning;
};
