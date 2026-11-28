#include "Persistence/LocalCacheLocationMigration.h"

#include "Logger.h"
#include "Persistence/SqliteOpenRecoveryPure.h"
#include "ScopeExit.h"

#include <SQLiteCpp/Backup.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <ghc/filesystem.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace smatchet {
namespace persistence {

namespace {

namespace fs = ghc::filesystem;

constexpr int kNoReplaceUnsupported =
    -2; // PublishNoReplace: fall back to a checked rename (callers hold the init-lock)

bool IsAborted(const LegacyCacheImportOptions& options) {
    return options.Abort != nullptr && options.Abort->load(std::memory_order_acquire);
}

std::string Sidecar(const std::string& path, const char* suffix) { return path + suffix; }

void RemoveFileQuietly(const std::string& path) {
    std::error_code ec;
    fs::remove(fs::u8path(path), ec);
}

void RemoveWithSidecars(const std::string& path) {
    RemoveFileQuietly(path);
    RemoveFileQuietly(Sidecar(path, "-wal"));
    RemoveFileQuietly(Sidecar(path, "-shm"));
    RemoveFileQuietly(Sidecar(path, "-journal"));
}

bool PathExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::u8path(path), ec) && !ec;
}

/// False when the file cannot be stat'ed; `out` is then untouched.
bool FileMtime(const std::string& path, std::int64_t& out) {
    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(fs::u8path(path), ec);
    if (ec) {
        return false;
    }
    out = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count());
    return true;
}

enum class CandidateGate { Eligible, Missing, Rejected };

/// Whether a candidate may be probed at all. A legacy file must be a regular file (never a FIFO or device, which would
/// block the open, and never a symlink), small enough, and on POSIX owned by the current user: a file planted by anyone
/// else is not the user's own cache and must neither be imported nor borrowed.
CandidateGate GateCandidate(const std::string& path, std::uintmax_t maxBytes, std::string& why) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(fs::u8path(path), ec);
    if (ec || status.type() == fs::file_type::not_found) {
        return CandidateGate::Missing;
    }
    if (status.type() != fs::file_type::regular) {
        why = "it is not a regular file";
        return CandidateGate::Rejected;
    }
    const std::uintmax_t bytes = fs::file_size(fs::u8path(path), ec);
    if (ec || bytes > maxBytes) {
        why = "it is larger than a cache can be";
        return CandidateGate::Rejected;
    }
#if !defined(_WIN32)
    struct stat info;
    if (::lstat(path.c_str(), &info) != 0 || info.st_uid != ::geteuid()) {
        why = "it belongs to another user";
        return CandidateGate::Rejected;
    }
#endif
    return CandidateGate::Eligible;
}

std::int64_t FileMtimeSec(const std::string& path) {
    std::int64_t mtime = 0;
    return FileMtime(path, mtime) ? mtime : 0;
}

#if defined(_WIN32)
// A wide path, with the extended-length prefix once it nears MAX_PATH (an imported cache can sit deep in a profile).
std::wstring ToWinPath(const std::string& utf8) {
    std::wstring wide = fs::u8path(utf8).wstring();
    if (wide.size() < MAX_PATH - 12 || wide.compare(0, 4, L"\\\\?\\") == 0) {
        return wide;
    }
    if (wide.compare(0, 2, L"\\\\") == 0) {
        return L"\\\\?\\UNC\\" + wide.substr(2); // \\server\share\x -> \\?\UNC\server\share\x
    }
    if (wide.size() > 2 && wide[1] == L':') {
        return L"\\\\?\\" + wide;
    }
    return wide;
}
#endif

/// Creates `path` exclusively (fails when it exists) and writes `note` into it. 0 on success, else an errno-style code
/// (EEXIST when the file is already there).
int CreateExclusive(const std::string& path, const std::string& note) {
#if defined(_WIN32)
    const HANDLE handle =
        ::CreateFileW(ToWinPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        return (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) ? EEXIST : EIO;
    }
    DWORD written = 0;
    if (!note.empty()) {
        (void)::WriteFile(handle, note.data(), static_cast<DWORD>(note.size()), &written, nullptr);
    }
    ::CloseHandle(handle);
    return 0;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return errno != 0 ? errno : EIO;
    }
    if (!note.empty()) {
        const ssize_t written = ::write(fd, note.data(), note.size());
        (void)written; // the note is informational; the file's existence is the lock
    }
    ::close(fd);
    return 0;
#endif
}

/// Renames `from` to `to` without ever replacing `to`. 0 on success, EEXIST when `to` exists, kNoReplaceUnsupported
/// when the platform or filesystem has no atomic no-replace rename, else an errno-style failure.
int PublishNoReplace(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    if (::MoveFileExW(ToWinPath(from).c_str(), ToWinPath(to).c_str(), MOVEFILE_WRITE_THROUGH)) {
        return 0;
    }
    const DWORD error = ::GetLastError();
    return (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) ? EEXIST : EIO;
#elif defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int kRenameNoReplace = 1; // RENAME_NOREPLACE, absent from older headers
    // The libc wrapper is missing before glibc 2.28, so go through the syscall.
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), kRenameNoReplace) == 0) {
        return 0;
    }
    const int error = errno;
    if (error == EINVAL || error == ENOTSUP || error == ENOSYS) {
        return kNoReplaceUnsupported; // the filesystem or kernel lacks the flag
    }
    return error;
#elif defined(__APPLE__)
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) {
        return 0;
    }
    const int error = errno;
    return (error == ENOTSUP || error == EINVAL) ? kNoReplaceUnsupported : error;
#else
    (void)from;
    (void)to;
    return kNoReplaceUnsupported;
#endif
}

/// The rename that is only safe because the caller holds the init-lock: every first creator of the target does.
int PublishCheckedRename(const std::string& from, const std::string& to) {
#if !defined(_WIN32)
    // link() fails with EEXIST when `to` exists, so even here a published target is never replaced.
    if (::link(from.c_str(), to.c_str()) == 0) {
        (void)::unlink(from.c_str());
        return 0;
    }
    const int error = errno != 0 ? errno : EIO;
    if (error != EPERM && error != ENOTSUP && error != EOPNOTSUPP && error != ENOSYS) {
        return error; // EEXIST (a published target), a real failure
    }
    // A volume without hard links (exFAT, some network shares): the caller holds the init-lock, so the checked rename
    // is safe here.
    if (PathExists(to)) {
        return EEXIST;
    }
    return std::rename(from.c_str(), to.c_str()) == 0 ? 0 : (errno != 0 ? errno : EIO);
#else
    if (PathExists(to)) {
        return EEXIST;
    }
    return std::rename(from.c_str(), to.c_str()) == 0 ? 0 : (errno != 0 ? errno : EIO);
#endif
}

void SyncFileBestEffort(const std::string& path) {
#if !defined(_WIN32)
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (::fsync(fd) != 0) {
            LOG_WARN("LocalCacheLocationMigration: fsync of '%s' failed errno=%d", path.c_str(), errno);
        }
        ::close(fd);
    }
#else
    (void)path; // MoveFileExW(MOVEFILE_WRITE_THROUGH) flushes on publish
#endif
}

void SyncParentDirBestEffort(const std::string& path) {
#if !defined(_WIN32)
    const std::string::size_type slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string(".") : (slash == 0 ? "/" : path.substr(0, slash));
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        if (::fsync(fd) != 0) {
            LOG_WARN("LocalCacheLocationMigration: fsync of directory '%s' failed errno=%d", dir.c_str(), errno);
        }
        ::close(fd);
    }
#else
    (void)path;
#endif
}

long long ProcessId() {
#if defined(_WIN32)
    return static_cast<long long>(::GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

enum class LockResult { Acquired, TimedOut, Aborted, Failed };

/// What this process writes into its init-lock, so a release removes only a lock that is still its own.
std::string MakeLockNote() {
    return "pid=" + std::to_string(ProcessId()) + " time=" + std::to_string(std::time(nullptr)) +
           " token=" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "\n";
}

/// Removes `lockPath` only when it still holds `note`: after a stale-lock reclaim race the file may be another
/// process's, and deleting it would let a third process in.
void ReleaseInitLock(const std::string& lockPath, const std::string& note) {
    fs::ifstream in(fs::u8path(lockPath), std::ios::binary);
    if (!in) {
        return;
    }
    std::string held(note.size() + 1, '\0'); // one byte more than ours, so a longer foreign note cannot compare equal
    in.read(&held[0], static_cast<std::streamsize>(held.size()));
    held.resize(static_cast<std::size_t>(in.gcount()));
    in.close();
    if (held == note) {
        RemoveFileQuietly(lockPath);
    }
}

/// Takes `<target>.init-lock`. A lock older than StaleLockAge is a crashed importer's and is reclaimed once; a live one
/// is waited on for at most BusyBudgetMs. A lock that cannot be stat'ed is never called stale.
LockResult AcquireInitLock(const std::string& lockPath, const std::string& note,
                           const LegacyCacheImportOptions& options) {
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool reclaimedStale = false;
    for (;;) {
        if (IsAborted(options)) {
            return LockResult::Aborted;
        }
        const int created = CreateExclusive(lockPath, note);
        if (created == 0) {
            return LockResult::Acquired;
        }
        if (created != EEXIST) {
            LOG_WARN("LocalCacheLocationMigration: could not create '%s' (errno %d)", lockPath.c_str(), created);
            return LockResult::Failed;
        }
        std::int64_t lockMtime = 0;
        if (!reclaimedStale && FileMtime(lockPath, lockMtime)) {
            const std::int64_t ageSec = static_cast<std::int64_t>(std::time(nullptr)) - lockMtime;
            if (std::llabs(static_cast<long long>(ageSec)) > static_cast<long long>(options.StaleLockAge.count())) {
                LOG_WARN("LocalCacheLocationMigration: reclaiming a stale init-lock '%s' (%lld s old)",
                         lockPath.c_str(), static_cast<long long>(ageSec));
                RemoveFileQuietly(lockPath);
                reclaimedStale = true;
                continue;
            }
        }
        const long long waitedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        if (waitedMs >= options.BusyBudgetMs) {
            return LockResult::TimedOut;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

/// Interrupts the running statement of a connection once `abort` is set (checked every few thousand VM steps).
int AbortProgress(void* abort) {
    return static_cast<const std::atomic<bool>*>(abort)->load(std::memory_order_acquire) ? 1 : 0;
}

/// A legacy file is untrusted input: its schema functions, views and triggers must never run, and a long statement
/// must be interruptible.
void HardenLegacyConnection(SQLite::Database& db, const std::atomic<bool>* abort) {
    db.exec("PRAGMA trusted_schema = OFF");
    if (abort != nullptr) {
        sqlite3_progress_handler(db.getHandle(), 4096, AbortProgress,
                                 const_cast<std::atomic<bool>*>(abort)); // read-only use; the API wants void*
    }
}

/// True when the file holds a trigger, a view or a virtual table. The cache never has one, and one in a file we would
/// adopt (copied or borrowed) would run on every later write: a trigger needs no SQL function, so trusted_schema does
/// not stop it.
bool HoldsForeignSchema(SQLite::Database& db) {
    SQLite::Statement foreign(db, "SELECT COUNT(*) FROM sqlite_master WHERE type IN ('trigger', 'view') OR "
                                  "(type = 'table' AND rootpage = 0)");
    return foreign.executeStep() && foreign.getColumn(0).getInt64() != 0;
}

constexpr const char* kForeignSchemaDetail = "it holds triggers, views or virtual tables that Smatchet never creates";

/// True for an ordinary table of that name (not a view, not a virtual table): counting a hostile file's view could run
/// arbitrary SQL, and a virtual table's module is not ours to drive.
bool IsPlainTable(SQLite::Database& db, const char* table) {
    SQLite::Statement q(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? AND rootpage > 0");
    q.bind(1, table);
    return q.executeStep();
}

std::int64_t CountRows(SQLite::Database& db, const char* table) {
    if (!IsPlainTable(db, table)) {
        return 0; // absent in this file's age of schema, or not a plain table
    }
    SQLite::Statement q(db, std::string("SELECT COUNT(*) FROM ") + table);
    return q.executeStep() ? q.getColumn(0).getInt64() : 0;
}

bool BetterCandidate(const LegacyCandidateInfo& a, const LegacyCandidateInfo& b) {
    if (a.PendingLive != b.PendingLive) {
        return a.PendingLive > b.PendingLive;
    }
    if (a.TicketRows != b.TicketRows) {
        return a.TicketRows > b.TicketRows;
    }
    return a.MtimeSec > b.MtimeSec;
}

enum class CopyOutcome { Done, Busy, Corrupt, Failed, Aborted };

/// Pages copied per backup step: small enough that an abort is noticed within milliseconds on a slow disk.
constexpr int kBackupPagesPerStep = 256;

/// Copies `src` into the new file `tmp` with SQLite's online backup and checks the copy. The source is opened read-only
/// and never written. On any outcome but Done the caller removes `tmp`.
CopyOutcome CopyCandidate(const std::string& src, const std::string& tmp, const LegacyCacheImportOptions& options,
                          std::string& detail) {
    try {
        SQLite::Database source(src, SQLite::OPEN_READONLY);
        source.setBusyTimeout(options.BusyBudgetMs);
        HardenLegacyConnection(source, options.Abort);
        {
            SQLite::Database destination(tmp, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
            HardenLegacyConnection(destination, options.Abort);
            {
                SQLite::Backup backup(destination, source);
                int rc = SQLITE_OK;
                do {
                    if (IsAborted(options)) {
                        detail = "aborted";
                        return CopyOutcome::Aborted;
                    }
                    rc = backup.executeStep(kBackupPagesPerStep);
                } while (rc == SQLITE_OK);
                if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
                    detail = "the legacy file is locked by another process";
                    return CopyOutcome::Busy;
                }
                if (rc != SQLITE_DONE) {
                    detail = "the copy stopped early (code " + std::to_string(rc) + ")";
                    return CopyOutcome::Failed;
                }
            }
            SQLite::Statement check(destination, "PRAGMA quick_check");
            if (!check.executeStep() || std::string(check.getColumn(0).getText()) != "ok") {
                detail = "the copy failed its integrity check";
                return CopyOutcome::Corrupt;
            }
            if (HoldsForeignSchema(destination)) {
                detail = kForeignSchemaDetail;
                return CopyOutcome::Corrupt;
            }
        }
        SyncFileBestEffort(tmp);
        return CopyOutcome::Done;
    } catch (const SQLite::Exception& ex) {
        detail = ex.what();
        if (IsAborted(options) || ex.getErrorCode() == SQLITE_INTERRUPT) {
            return CopyOutcome::Aborted;
        }
        if (IsRebuildableCorruptCode(ex.getErrorCode())) {
            return CopyOutcome::Corrupt;
        }
        return IsTransientBusyCode(ex.getErrorCode()) ? CopyOutcome::Busy : CopyOutcome::Failed;
    }
}

LegacyCacheImportResult UseLegacy(const LegacyCandidateInfo& candidate, const char* why) {
    LegacyCacheImportResult result;
    result.Kind = LegacyCacheImportKind::UseLegacyThisSession;
    result.OpenPath = candidate.Path;
    result.LegacyPath = candidate.Path;
    result.Detail = why;
    return result;
}

std::vector<std::string> OthersThan(const std::vector<LegacyCandidateInfo>& readable, const std::string& chosen) {
    std::vector<std::string> others;
    for (std::size_t i = 0; i < readable.size(); ++i) {
        if (readable[i].Path != chosen) {
            others.push_back(readable[i].Path);
        }
    }
    return others;
}

struct ProbeSet {
    std::vector<LegacyCandidateInfo> Readable; ///< best first
    std::vector<LegacyCandidateInfo> Busy;
    std::vector<std::string> Rejected; ///< not eligible (not ours, not a file, too large): listed, never used
    std::size_t Corrupt = 0;
    bool AnyExists = false;
};

ProbeSet ProbeAll(const std::vector<std::string>& candidates, const LegacyCacheImportOptions& options) {
    ProbeSet probes;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (IsAborted(options)) {
            continue;
        }
        std::string why;
        const CandidateGate gate = GateCandidate(candidates[i], options.MaxFileBytes, why);
        if (gate == CandidateGate::Missing) {
            continue;
        }
        if (gate == CandidateGate::Rejected) {
            LOG_WARN("LocalCacheLocationMigration: ignoring the legacy cache '%s': %s", candidates[i].c_str(),
                     why.c_str());
            probes.Rejected.push_back(candidates[i]);
            continue;
        }
        probes.AnyExists = true;
        const LegacyCandidateInfo info = ProbeLegacyCandidate(candidates[i], options.BusyBudgetMs, options.Abort);
        if (info.Error.empty()) {
            probes.Readable.push_back(info);
        } else if (info.Busy) {
            probes.Busy.push_back(info);
            LOG_WARN("LocalCacheLocationMigration: legacy cache '%s' cannot be read right now: %s", info.Path.c_str(),
                     info.Error.c_str());
        } else {
            ++probes.Corrupt;
            LOG_WARN("LocalCacheLocationMigration: legacy cache '%s' is corrupt: %s", info.Path.c_str(),
                     info.Error.c_str());
        }
    }
    std::stable_sort(probes.Readable.begin(), probes.Readable.end(), BetterCandidate);
    std::stable_sort(
        probes.Busy.begin(), probes.Busy.end(),
        [](const LegacyCandidateInfo& a, const LegacyCandidateInfo& b) { return a.MtimeSec > b.MtimeSec; });
    return probes;
}

/// Removes the temporary copies (and their sidecars) an earlier crashed import left beside `target`. Only called under
/// the init-lock, so none of them can belong to a running importer.
void SweepStaleImportFiles(const std::string& target) {
    std::error_code ec;
    const fs::path targetPath = fs::u8path(target);
    const fs::path dir = targetPath.has_parent_path() ? targetPath.parent_path() : fs::path(".");
    const std::string prefix = targetPath.filename().string() + ".import-";
    std::vector<fs::path> stale;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0) {
            stale.push_back(it->path());
        }
    }
    for (std::size_t i = 0; i < stale.size(); ++i) {
        fs::remove(stale[i], ec);
    }
}

/// An empty target created under the init-lock, so a later start (or another process) sees the location as taken.
/// False when it could not be created (the caller must not let the open create it unlocked).
bool CreateEmptyTarget(const std::string& target) {
    const int created = CreateExclusive(target, std::string());
    return created == 0 || created == EEXIST;
}

/// Publishes the verified copy `tmp` as `target`. 0 on success, EEXIST when another process published first.
int Publish(const std::string& tmp, const std::string& target, const LegacyCacheImportOptions& options) {
    if (options.BeforePublishForTest) {
        options.BeforePublishForTest();
    }
    int rc = options.PublishForTest ? options.PublishForTest(tmp, target) : PublishNoReplace(tmp, target);
    if (rc == kNoReplaceUnsupported || rc == ENOTSUP || rc == ENOSYS || rc == EINVAL) {
        rc = PublishCheckedRename(tmp, target); // safe: every first creator of the target holds the init-lock
    }
    return rc;
}

/// Every legacy file is corrupt: start fresh, creating the target here under the lock.
LegacyCacheImportResult FinishUnreadable(const std::string& target, const char* why) {
    LegacyCacheImportResult result;
    result.OpenPath = target;
    result.Detail = why;
    result.Kind = CreateEmptyTarget(target) ? LegacyCacheImportKind::LegacyUnreadable : LegacyCacheImportKind::Deferred;
    return result;
}

/// Pick, copy, verify and publish, under the init-lock.
LegacyCacheImportResult ImportWithProbes(const std::string& target, const ProbeSet& probes,
                                         const LegacyCacheImportOptions& options) {
    LegacyCacheImportResult result;
    result.OpenPath = target;
    if (IsAborted(options)) {
        result.Detail = "aborted";
        return result;
    }
    SweepStaleImportFiles(target);
    if (!probes.AnyExists) {
        // Nothing to import: leave an empty file so later starts (and other processes) see the target as taken.
        if (!CreateEmptyTarget(target)) {
            result.Kind = LegacyCacheImportKind::Deferred;
            result.Detail = "the cache location could not be created";
            return result;
        }
        result.Kind = LegacyCacheImportKind::CreatedEmpty;
        return result;
    }
    if (probes.Readable.empty()) {
        if (!probes.Busy.empty()) {
            // Its probe stopped at the lock before the schema check could run; the open of a borrowed file never
            // rebuilds it, and a file that turns out to hold triggers or views is a risk only a user-owned file can
            // pose here.
            return UseLegacy(probes.Busy.front(), "every legacy cache is locked by another process");
        }
        return FinishUnreadable(target, "every legacy cache is corrupt");
    }

    const std::string tmpBase = target + ".import-" + std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
                                std::to_string(ProcessId());
    std::string tmp = tmpBase;
    ScopeExit removeTmp([&tmp]() { RemoveWithSidecars(tmp); });
    for (std::size_t i = 0; i < probes.Readable.size(); ++i) {
        const LegacyCandidateInfo& best = probes.Readable[i];
        std::string detail;
        tmp = tmpBase + "-" + std::to_string(i); // a fresh name per candidate: a leftover cannot block the next copy
        // Pre-created exclusively with owner-only permissions, so the copy never lands in a file someone else made
        // first.
        if (CreateExclusive(tmp, std::string()) != 0) {
            return UseLegacy(best, "the temporary copy could not be created");
        }
        const CopyOutcome copied = CopyCandidate(best.Path, tmp, options, detail);
        if (copied == CopyOutcome::Aborted) {
            result.Detail = "aborted";
            return result;
        }
        if (copied == CopyOutcome::Corrupt) {
            LOG_WARN("LocalCacheLocationMigration: legacy cache '%s' is corrupt (%s); trying the next one",
                     best.Path.c_str(), detail.c_str());
            RemoveWithSidecars(tmp);
            continue;
        }
        if (copied != CopyOutcome::Done) {
            return UseLegacy(best, detail.c_str());
        }
        const int published = Publish(tmp, target, options);
        if (published == EEXIST) {
            result.Kind = LegacyCacheImportKind::NotNeeded; // another process published first: never overwrite it
            result.Detail = "the target appeared while importing";
            return result;
        }
        if (published != 0) {
            LOG_WARN("LocalCacheLocationMigration: could not publish the import (errno %d)", published);
            return UseLegacy(best, "the copy could not be published");
        }
        SyncParentDirBestEffort(target);
        result.Kind = LegacyCacheImportKind::Imported;
        result.LegacyPath = best.Path;
        LOG_INFO("LocalCacheLocationMigration: imported '%s' into '%s'", best.Path.c_str(), target.c_str());
        return result;
    }
    return FinishUnreadable(target, "every legacy cache failed its copy");
}

/// Steps 4-7 under the init-lock, then the report of every other candidate the user should hear about: readable ones
/// that were not used, ones that could not be read just now, and ones that were not eligible. Whatever the outcome.
LegacyCacheImportResult ImportUnderLock(const std::string& target, const std::vector<std::string>& candidates,
                                        const LegacyCacheImportOptions& options) {
    const ProbeSet probes = ProbeAll(candidates, options);
    LegacyCacheImportResult result = ImportWithProbes(target, probes, options);
    std::vector<std::string> others = OthersThan(probes.Readable, result.LegacyPath);
    for (std::size_t i = 0; i < probes.Busy.size(); ++i) {
        if (probes.Busy[i].Path != result.LegacyPath) {
            others.push_back(probes.Busy[i].Path);
        }
    }
    others.insert(others.end(), probes.Rejected.begin(), probes.Rejected.end());
    result.OtherCandidates = others;
    return result;
}

} // namespace

LegacyCandidateInfo ProbeLegacyCandidate(const std::string& path, int busyBudgetMs, const std::atomic<bool>* abort) {
    LegacyCandidateInfo info;
    info.Path = path;
    info.MtimeSec = FileMtimeSec(path);
    try {
        SQLite::Database db(path, SQLite::OPEN_READONLY);
        db.setBusyTimeout(busyBudgetMs);
        HardenLegacyConnection(db, abort);
        {
            // A file that is not a database fails here (NOTADB / CORRUPT), before any table is counted.
            SQLite::Statement schema(db, "SELECT COUNT(*) FROM sqlite_master");
            (void)schema.executeStep();
        }
        if (HoldsForeignSchema(db)) {
            info.Error = kForeignSchemaDetail;
            return info; // not corrupt as SQLite means it, but not a cache of ours: Busy stays false
        }
        info.PendingLive =
            CountRows(db, "pending_creates") + CountRows(db, "pending_field_edits") + CountRows(db, "pending_actions");
        info.TicketRows = CountRows(db, "tickets_v2");
        if (info.TicketRows == 0) {
            info.TicketRows = CountRows(db, "tickets");
        }
    } catch (const SQLite::Exception& ex) {
        info.Error = ex.what();
        // Only a file SQLite calls damaged is "corrupt". A lock, a permission or I/O fault, an interrupted probe: the
        // file may be fine, and calling it corrupt would let the empty target be created and strand its queued writes.
        info.Busy = !IsRebuildableCorruptCode(ex.getErrorCode());
    } catch (const std::exception& ex) {
        info.Error = ex.what();
        info.Busy = true;
    }
    return info;
}

LegacyCacheImportResult ImportLegacyLocalCacheIfNeeded(const std::string& target,
                                                       const std::vector<std::string>& candidates,
                                                       const LegacyCacheImportOptions& options) {
    LegacyCacheImportResult result;
    result.OpenPath = target;
    try {
        if (target.empty() || PathExists(target)) {
            return result; // NotNeeded
        }
        std::error_code ec;
        const fs::path parent = fs::u8path(target).parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec); // a failure surfaces as the lock or the open failing
        }
        const std::string lockPath = target + ".init-lock";
        const std::string note = MakeLockNote();
        const LockResult locked = AcquireInitLock(lockPath, note, options);
        if (locked == LockResult::Aborted) {
            result.Detail = "aborted";
            return result;
        }
        if (locked == LockResult::TimedOut) {
            // A live process is importing right now. Borrowing the file it is copying would leave this session's writes
            // in a file the next start never imports from (the target will exist), so this session waits it out with a
            // memory-only cache and the next start finds the published target.
            result.Kind = LegacyCacheImportKind::Deferred;
            result.Detail = "another Smatchet process is moving the cache";
            return result;
        }
        if (locked != LockResult::Acquired) {
            // The lock file cannot be made at all (an unwritable directory): the target cannot be created either, so
            // the best legacy file serves this session, and nothing is created.
            ProbeSet probes = ProbeAll(candidates, options);
            if (!probes.Readable.empty()) {
                return UseLegacy(probes.Readable.front(), "the import lock could not be created");
            }
            result.Kind = LegacyCacheImportKind::Deferred;
            result.Detail = "the import lock could not be created";
            return result;
        }
        ScopeExit releaseLock([lockPath, note]() { ReleaseInitLock(lockPath, note); });
        if (PathExists(target)) {
            return result; // lost the race before the lock: the winner's file stands
        }
        return ImportUnderLock(target, candidates, options);
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheLocationMigration: the import failed: %s", ex.what());
        result.Kind = LegacyCacheImportKind::Deferred;
        result.Detail = ex.what();
    } catch (...) { // catch-all-ok: this runs on a worker and must never throw; the legacy files are untouched
        LOG_ERROR("LocalCacheLocationMigration: the import failed: unknown exception");
        result.Kind = LegacyCacheImportKind::Deferred;
        result.Detail = "unknown exception";
    }
    return result;
}

} // namespace persistence
} // namespace smatchet
