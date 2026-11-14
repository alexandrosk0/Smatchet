#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <string>
#include <vector>

// Recent picks for value dropdowns (docs/plans/shared-dropdown-recents.md): for each scope, the last few
// option ids the user picked, newest first. One store serves every picker that draws through
// smatchet::dropdown::DrawBody, so no picker keeps its own list.
//
// A scope is "<backend cache key>|<field id>" (MakeScopeKey): one list per field per tracker instance,
// shared across projects. The dropdown hides a recent id that is not in the option list it is showing.
//
// Layering: ImGui-free and backend-agnostic (string keys only); lives at the include/ root so both the
// domain-side cell editors and the Ui/ panels may use it.
namespace smatchet {
namespace recents {

/// Recent ids kept per scope.
constexpr std::size_t kMaxPerScope = 5;
/// Scopes kept; recording into a new scope past this drops the scope touched longest ago.
constexpr std::size_t kMaxScopes = 128;
/// Longest id or scope key stored, in bytes. Longer values are not recorded.
constexpr std::size_t kMaxKeyBytes = 256;

/// The store itself: plain data, no locking, no I/O. Unit-tested in tests/Core/RecentSelections.test.cpp.
class RecentSelectionStore {
  public:
    /// Puts `id` first in `scope`'s list (moving it there if already present) and trims the list to
    /// kMaxPerScope. Returns true if the stored state changed. An empty or over-long scope or id is ignored.
    bool Record(const std::string& scope, const std::string& id);

    /// `scope`'s ids, newest first; empty when the scope is unknown.
    std::vector<std::string> Get(const std::string& scope) const;

    std::size_t ScopeCount() const { return scopes_.size(); }
    void Clear() { scopes_.clear(); }

    /// JSON array of {"scope": "...", "ids": ["..."]}, most recently touched scope first.
    nlohmann::json ToJson() const;

    /// Replaces the contents with what `j` holds. Anything that is not the ToJson shape is skipped, entry
    /// by entry, and the caps are re-applied, so a hand-edited or damaged value can never over-fill the store.
    void FromJson(const nlohmann::json& j);

  private:
    struct Scope {
        std::string Key;
        std::vector<std::string> Ids; // newest first
    };
    std::vector<Scope> scopes_; // most recently touched first
};

/// "<backendKey>|<fieldId>".
std::string MakeScopeKey(const std::string& backendKey, const std::string& fieldId);

/// Top-level key of the merged config JSON that holds the shared store.
extern const char* const kConfigJsonKey;

/// Fills the process-wide store from the merged config JSON (reads `kConfigJsonKey`) and turns on saving
/// for RecordAndPersist. Called once at AppController init. A missing or malformed value loads as empty.
void LoadSharedFromConfigJson(const nlohmann::json& mergedConfig);

/// Records a pick in the process-wide store. When saving is on and the pick changed the store, the whole
/// store is handed to the config-save worker (smatchet::config_save::EnqueueConfigJsonKey), so the caller
/// (the frame thread) never waits on the disk. Before LoadSharedFromConfigJson the pick is kept in
/// memory only, which keeps unit and UI tests away from the user's config file.
void RecordAndPersist(const std::string& scope, const std::string& id);

/// `scope`'s ids from the process-wide store, newest first. Memory only: safe per frame and offline.
std::vector<std::string> SharedGet(const std::string& scope);

/// Test hook: empties the process-wide store and turns saving off.
void ResetSharedForTests();

/// Test hooks for a suite that can run inside a live session (the UI tests). Begin sets the store and
/// its saving switch aside and starts empty with saving off; called again before End it only empties
/// the store. End puts the store and the switch back; without a Begin it does nothing.
void BeginSharedTestScope();
void EndSharedTestScope();

} // namespace recents
} // namespace smatchet
