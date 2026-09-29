#pragma once

// LookupPayloadsPure — `lookup_cache` kinds and payload codecs for the offline copies of the field
// catalog's side lookups (Quality Pillar 6): per-project component options, the user roster, the
// per-issue-type edit permissions and the project list. Every row is keyed by the backend namespace,
// so two trackers never share one. Pure: no I/O, no Logger.h.

#include "Tracker/TrackerFieldSchema.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace smatchet {
namespace lookup {

/// Per-project component options; key = the project key, payload = SerializeFieldOptions.
constexpr const char* kProjectComponentsKind = "project_components";
/// The tracker's user roster; one row per backend under kUsersKey, payload = SerializeUsers.
constexpr const char* kUsersKind = "users";
constexpr const char* kUsersKey = "all";
/// Edit permissions learned per issue type; key = the lower-cased issue type, payload =
/// SerializeEditPermissions.
constexpr const char* kEditMetaTypeKind = "editmeta_type";
/// The projects visible to the credentials (the project picker's "All projects"); one row per backend
/// under kProjectsKey, payload = SerializeProjects.
constexpr const char* kProjectsKind = "projects";
constexpr const char* kProjectsKey = "all";

/// Most users one roster row holds: the Jira roster fetch stops at the same bound.
constexpr std::size_t kMaxStoredUsers = 50000;
/// A roster payload above this size is not stored, so every stored row stays parseable.
constexpr std::size_t kMaxUsersPayloadBytes = 16u * 1024u * 1024u;
/// Most projects one row holds: Plane's paginated listing stops at 100 pages of 100.
constexpr std::size_t kMaxStoredProjects = 10000;
/// A project-list payload above this size is not stored.
constexpr std::size_t kMaxProjectsPayloadBytes = 4u * 1024u * 1024u;

/// A JSON array of {accountId, displayName, email, accountType, active}, at most kMaxStoredUsers
/// entries. "" when the result would exceed kMaxUsersPayloadBytes (the caller then stores nothing).
std::string SerializeUsers(const std::vector<TrackerUser>& users);

/// Inverse of SerializeUsers. False on a parse error or a non-array; never throws. Entries without an
/// account id are skipped.
bool ParseUsers(const std::string& json, std::vector<TrackerUser>& out);

/// A JSON object {fieldId: canEdit}.
std::string SerializeEditPermissions(const std::unordered_map<std::string, bool>& fieldCanEdit);

/// Inverse of SerializeEditPermissions. False on a parse error or a non-object; never throws. Members
/// whose value is not a bool are skipped.
bool ParseEditPermissions(const std::string& json, std::unordered_map<std::string, bool>& out);

/// A JSON array of {id, key, name}, at most kMaxStoredProjects entries. "" when the result would exceed
/// kMaxProjectsPayloadBytes (the caller then stores nothing).
std::string SerializeProjects(const std::vector<RemoteProject>& projects);

/// Inverse of SerializeProjects. False on a parse error or a non-array; never throws. Entries with
/// neither an id nor a key are skipped.
bool ParseProjects(const std::string& json, std::vector<RemoteProject>& out);

} // namespace lookup
} // namespace smatchet
