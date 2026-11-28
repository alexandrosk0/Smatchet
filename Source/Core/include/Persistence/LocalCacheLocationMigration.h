#pragma once

// LocalCacheLocationMigration — the one-time move of the local cache from where older builds left it (the process
// working directory, the runtime asset directory, the shared user-data directory) to the planned location
// (LocalCacheDbPathPure.h).
//
// The import never writes, moves or deletes the legacy file: it is opened read-only (with `trusted_schema` off), copied
// with SQLite's online backup into a temporary file next to the target, checked, and published with a rename that
// cannot replace an existing file. An older build or a storage-mode switch back keeps working on its own copy. Every
// first creator of the target (an import, an empty start) holds `<target>.init-lock`, so two Smatchet processes
// starting together cannot both create it. When the copy cannot be made now the caller may open the legacy file itself
// for the session. It must then switch off the RebuildCorruptFile open option, so a corrupt file is never moved aside.
//
// Worker thread only: it does file I/O and waits for locks.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace smatchet {
namespace persistence {

enum class LegacyCacheImportKind {
    NotNeeded,            ///< the target already exists, or another process won the race and published it
    Imported,             ///< the best legacy file was copied to the target
    CreatedEmpty,         ///< no legacy file exists; an empty target was created so later starts see it
    UseLegacyThisSession, ///< the copy could not be made now (a lock, a publish failure): open `OpenPath` instead
    LegacyUnreadable,     ///< every legacy file is corrupt: an empty target was created (under the lock) to start fresh
    Deferred ///< nothing was imported and the target must not be created this session (a lock that did not clear, an
             ///< I/O fault): open nothing durable; the next start tries again. `Detail` says why.
};

/// What probing one candidate (opened read-only) found.
struct LegacyCandidateInfo {
    std::string Path;
    std::int64_t PendingLive = 0; ///< rows in pending_creates + pending_field_edits + pending_actions
    std::int64_t TicketRows = 0;  ///< rows in tickets_v2 (or the legacy tickets table)
    std::int64_t MtimeSec = 0;
    std::string Error; ///< why it could not be read; empty when readable
    /// Unreadable for a reason other than corruption: a lock another process holds, permissions, an I/O fault, an
    /// interrupted probe. Such a file may be perfectly good, so it is never treated as corrupt.
    bool Busy = false;
};

struct LegacyCacheImportResult {
    LegacyCacheImportKind Kind = LegacyCacheImportKind::NotNeeded;
    /// The path the cache should open: the target, or (UseLegacyThisSession) the legacy file.
    std::string OpenPath;
    /// The file that was imported, or that is opened this session.
    std::string LegacyPath;
    /// Other readable legacy files that were not imported, for the user to be told about.
    std::vector<std::string> OtherCandidates;
    std::string Detail;
};

struct LegacyCacheImportOptions {
    /// How long a read waits for a lock another process holds, and how long to wait for another importer's lock.
    int BusyBudgetMs = 2000;
    /// Checked between steps and while waiting: set to end the import at once.
    const std::atomic<bool>* Abort = nullptr;
    /// A legacy file larger than this is not a cache of ours: it is ignored (and listed), never copied or borrowed.
    std::uintmax_t MaxFileBytes = std::uintmax_t(2) * 1024u * 1024u * 1024u;
    /// An `init-lock` older than this is a crashed importer's and is reclaimed once.
    std::chrono::seconds StaleLockAge{120};
    /// Test seams. Called after the temporary copy is verified and before it is published.
    std::function<void()> BeforePublishForTest;
    /// Test seam replacing the no-replace rename: returns 0 on success, else an errno-style code (EEXIST = lost the
    /// race).
    std::function<int(const std::string& from, const std::string& to)> PublishForTest;
};

/// Probe one legacy candidate read-only. Never throws.
LegacyCandidateInfo ProbeLegacyCandidate(const std::string& path, int busyBudgetMs,
                                         const std::atomic<bool>* abort = nullptr);

/// Import the best of `candidates` to `target` if `target` does not exist yet. Never throws.
LegacyCacheImportResult ImportLegacyLocalCacheIfNeeded(const std::string& target,
                                                       const std::vector<std::string>& candidates,
                                                       const LegacyCacheImportOptions& options);

} // namespace persistence
} // namespace smatchet
