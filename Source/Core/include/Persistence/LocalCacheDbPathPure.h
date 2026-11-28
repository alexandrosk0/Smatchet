#pragma once

// LocalCacheDbPathPure — the pure planner behind where the local cache file lives.
//
// The configured DbPath defaults to the bare name `Smatchet_LocalCache.sqlite` (SmatchetDefaults.h), which
// SQLite resolves against the process working directory: a launch from another directory silently starts an
// empty cache and strands queued offline writes in the old file. This planner turns the configured value into
// one absolute target (a relative or empty value resolves against the user-data directory) and names the
// legacy locations a one-time import may copy an older file from.
//
// Purely lexical: no filesystem access, so every rule is unit-testable without touching disk. The caller
// supplies each base directory; an empty base means "unknown" and is skipped.

#include "SmatchetDefaults.h"

#include <ghc/filesystem.hpp>

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace persistence {

struct LocalCacheDbPathInputs {
    /// The configured value (`TrackerConfig::DbPath`): empty, relative or absolute.
    std::string Configured;
    /// Directory a relative value resolves against (the active user-data directory).
    std::string UserDataDir;
    /// Process working directory captured once at startup; the last-resort base and a legacy location.
    std::string Cwd;
    /// Runtime asset directory (the executable's directory on a packaged build); a legacy location.
    std::string RuntimeAssetDir;
    /// The platform-shared user-data directory (differs from `UserDataDir` under a storage-mode override).
    std::string SharedUserDataDir;
    /// False disables the legacy candidate list (an explicit user-data override opts out of the import).
    bool AllowLegacyImport = true;
};

struct LocalCacheDbPathPlan {
    /// The path the cache opens. Absolute whenever any base was supplied.
    std::string Target;
    /// Existing-file candidates to import from, in priority order. Never contains `Target`.
    std::vector<std::string> LegacyCandidates;
    /// True when a relative path had no user-data directory and resolved against the working directory.
    bool BaseFellBackToCwd = false;
};

namespace local_cache_db_path_detail {

inline std::string LowerAscii(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return s;
}

inline std::string NormalizedGeneric(const std::string& p) {
    return ghc::filesystem::u8path(p).lexically_normal().generic_string();
}

} // namespace local_cache_db_path_detail

/// True when two paths name the same file lexically: `lexically_normal` on both, case-insensitive under
/// `_WIN32`. No 8.3-name, symlink or junction resolution (that needs the filesystem).
inline bool SameLocalPath(const std::string& a, const std::string& b) {
    std::string na = local_cache_db_path_detail::NormalizedGeneric(a);
    std::string nb = local_cache_db_path_detail::NormalizedGeneric(b);
#if defined(_WIN32)
    na = local_cache_db_path_detail::LowerAscii(std::move(na));
    nb = local_cache_db_path_detail::LowerAscii(std::move(nb));
#endif
    return na == nb;
}

/// Plan the cache location.
///  - empty `Configured` is `kDefaultDbPath`;
///  - an absolute value is the target as-is (normalized), with no legacy candidates;
///  - a relative value resolves against `UserDataDir`, else `Cwd` (`BaseFellBackToCwd`); the candidates are
///    `<Cwd | RuntimeAssetDir | SharedUserDataDir>/<configured>` minus empty bases, minus anything that equals
///    the target, deduplicated in order;
///  - with neither base known the target is the configured value untouched (the caller logs a warning).
inline LocalCacheDbPathPlan PlanLocalCacheDbPath(const LocalCacheDbPathInputs& in) {
    namespace fs = ghc::filesystem;
    LocalCacheDbPathPlan plan;

    const std::string configured =
        in.Configured.empty() ? std::string(SmatchetDefaults::kDefaultDbPath) : in.Configured;
    const fs::path configuredPath = fs::u8path(configured);

    if (configuredPath.is_absolute()) {
        plan.Target = configuredPath.lexically_normal().string();
        return plan;
    }

    const std::string* base = nullptr;
    if (!in.UserDataDir.empty()) {
        base = &in.UserDataDir;
    } else if (!in.Cwd.empty()) {
        base = &in.Cwd;
        plan.BaseFellBackToCwd = true;
    }
    if (base == nullptr) {
        plan.Target = configured;
        return plan;
    }

    plan.Target = (fs::u8path(*base) / configuredPath).lexically_normal().string();
    if (!in.AllowLegacyImport) {
        return plan;
    }

    const std::string* const bases[3] = {&in.Cwd, &in.RuntimeAssetDir, &in.SharedUserDataDir};
    for (std::size_t i = 0; i < 3; ++i) {
        if (bases[i]->empty()) {
            continue;
        }
        const std::string candidate = (fs::u8path(*bases[i]) / configuredPath).lexically_normal().string();
        if (SameLocalPath(candidate, plan.Target)) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t j = 0; j < plan.LegacyCandidates.size(); ++j) {
            if (SameLocalPath(plan.LegacyCandidates[j], candidate)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            plan.LegacyCandidates.push_back(candidate);
        }
    }
    return plan;
}

} // namespace persistence
} // namespace smatchet
