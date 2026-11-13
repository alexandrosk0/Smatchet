#pragma once

#include "StringUtil.h"

#include <algorithm>
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
    /// A status transition target the tracker offers from any status (Jira `isGlobal`) or back to the
    /// same one (`isLooped`), so it appearing as the issue's current status says nothing about where the
    /// issue is. Set only by transition parsing; never persisted.
    bool ReachableFromAnyStatus = false;
    /// Other tracker ids of this one row: a Jira status name that several workflows define with their
    /// own id (StatusOptionMergePure). Lookups by id accept them, so a target from an issue's own
    /// transitions still resolves after the catalog shows the name once.
    std::vector<std::string> MergedIds;

    bool HasId(const std::string& id) const {
        return Id == id || std::find(MergedIds.begin(), MergedIds.end(), id) != MergedIds.end();
    }
    /// A merged row also answers to the other spellings it replaced, which differ only by case.
    bool HasName(const std::string& name) const {
        return Value == name || (!MergedIds.empty() && EqualsIgnoreCaseAscii(Value, name));
    }
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

struct RemoteProject {
    std::string id;          // Jira numeric id or Plane UUID
    std::string key;         // Jira key (e.g., "PROJ"); empty for Plane
    std::string displayName; // human-readable
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
