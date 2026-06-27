#pragma once

// PathConfinement.h — confine a caller-supplied file path to an allow-listed base
// directory. Used to bound MCP-reachable file commands (perf.dump outPath,
// whisper.transcribe-once --file, scenario.run outPath/outLog) so a caller cannot
// read or write an arbitrary filesystem location (SECURITY_AUDIT.md — MCP-reachable
// arbitrary file read/write). Header-only so the few command TUs that need it can
// include it without a CMake/link change; ghc::filesystem is already linked into
// those targets.

#include <string>
#include <system_error>

#include <ghc/filesystem.hpp>

namespace smatchet {
namespace cmd {

// Confine `candidate` under `baseDir`. On success, `resolvedOut` holds the absolute,
// normalized path inside `baseDir` and the function returns true. On any escape —
// empty/absolute path, a `..` traversal component, or a resolution (incl. symlinks)
// that lands outside `baseDir` — it sets `errOut` to a stable, input-free message
// and returns false. Callers reject the command on false.
inline bool ConfinePathUnderBase(const std::string& baseDir, const std::string& candidate, std::string& resolvedOut,
                                 std::string& errOut) {
    namespace fs = ghc::filesystem;
    resolvedOut.clear();
    errOut.clear();
    if (baseDir.empty()) {
        errOut = "no base directory configured for confinement";
        return false;
    }
    if (candidate.empty()) {
        errOut = "empty path";
        return false;
    }

    const fs::path cand(candidate);
    if (cand.is_absolute()) {
        errOut = "absolute paths are not allowed";
        return false;
    }
    for (const auto& part : cand) {
        if (part.string() == "..") {
            errOut = "path traversal ('..') is not allowed";
            return false;
        }
    }

    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::path(baseDir), ec);
    if (ec) {
        base = fs::path(baseDir).lexically_normal();
        ec.clear();
    }

    const fs::path combined = (base / cand).lexically_normal();
    fs::path resolved = fs::weakly_canonical(combined, ec);
    if (ec) {
        resolved = combined;
        ec.clear();
    }

    // Final containment: the resolved path, expressed relative to base, must not be
    // empty (unrelated roots) and must not begin with a `..` component (escapes up).
    const fs::path rel = resolved.lexically_relative(base);
    if (rel.empty() || rel.begin()->string() == "..") {
        errOut = "resolved path escapes the allowed base directory";
        return false;
    }

    resolvedOut = resolved.string();
    return true;
}

// Confine `candidate` under `<baseDir>/<subdir>` — a dedicated per-feature directory
// rather than the user-data root. The root itself holds smatchet_config.json and other
// program state, so confining a caller-supplied write target to the root still leaves a
// primitive to clobber those files; a dedicated subdir (e.g. "perf", "ui-tests",
// "whisper-import") removes that overlap. Creates the subdir if missing so the caller can
// write into it immediately. Returns false (with `errOut` set) on a confinement escape or
// if the subdir cannot be created.
inline bool ConfinePathUnderSubdir(const std::string& baseDir, const std::string& subdir,
                                   const std::string& candidate, std::string& resolvedOut, std::string& errOut) {
    namespace fs = ghc::filesystem;
    resolvedOut.clear();
    errOut.clear();
    if (baseDir.empty()) {
        errOut = "no base directory configured for confinement";
        return false;
    }
    const fs::path subBase = fs::path(baseDir) / subdir;
    std::error_code ec;
    fs::create_directories(subBase, ec); // idempotent; ignore "already exists"
    if (ec && !fs::exists(subBase)) {
        errOut = "could not create confinement directory";
        return false;
    }
    return ConfinePathUnderBase(subBase.string(), candidate, resolvedOut, errOut);
}

} // namespace cmd
} // namespace smatchet
