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
    // Validate the subdir FIRST — before any mkdir — so an absolute, empty, or
    // dot-dot value can never anchor (or create) a directory outside the base. Every
    // caller passes a fixed literal today, yet validating up front keeps the helper
    // safe should a variable ever flow in. The canonical-containment check further
    // below remains as a secondary guard.
    if (subdir.empty()) {
        errOut = "empty confinement directory";
        return false;
    }
    // Normalize first so "perf/.." or "./perf" collapse before validation, then check
    // the collapsed form. Comparing against the normalized subdir (rather than the
    // joined base path) avoids a platform-dependent trailing-separator mismatch — on
    // Windows `(base / ".").lexically_normal()` keeps a trailing separator, so an
    // equality test against `base` would wrongly accept a "." subdir.
    const fs::path subdirPath = fs::path(subdir).lexically_normal();
    if (subdirPath.is_absolute()) {
        errOut = "absolute confinement directories are not allowed";
        return false;
    }
    for (const auto& part : subdirPath) {
        if (part.string() == "..") {
            errOut = "confinement directory traversal ('..') is not allowed";
            return false;
        }
    }
    if (subdirPath.empty() || subdirPath == fs::path(".")) {
        errOut = "confinement directory must be a real subdirectory"; // e.g. "." / "perf/.."
        return false;
    }
    std::error_code ec;
    // Anchor the subdir to the canonical user-data dir so the base itself can't be
    // redirected through a symlink before we ever look at the candidate.
    fs::path canonBase = fs::weakly_canonical(fs::path(baseDir), ec);
    if (ec) {
        canonBase = fs::path(baseDir).lexically_normal();
        ec.clear();
    }

    const fs::path subBase = (canonBase / subdirPath).lexically_normal();
    fs::create_directories(subBase, ec); // idempotent; ignore "already exists"
    if (ec && !fs::exists(subBase)) {
        errOut = "could not create confinement directory";
        return false;
    }
    // Reject a symlinked subdir: if <base>/<subdir> is a symlink, its target could
    // point outside the user-data root, and ConfinePathUnderBase would then resolve
    // (and accept) candidates relative to that escaped target. Require a real dir.
    ec.clear();
    if (fs::is_symlink(subBase, ec)) {
        errOut = "confinement directory must not be a symlink";
        return false;
    }
    ec.clear();
    if (!fs::is_directory(subBase, ec) || ec) {
        errOut = "confinement directory is not a directory";
        return false;
    }
    // Defense-in-depth: the canonical subdir must still sit under the canonical base
    // (catches a symlinked ancestor component too).
    ec.clear();
    fs::path canonSub = fs::weakly_canonical(subBase, ec);
    if (ec) {
        canonSub = subBase.lexically_normal();
        ec.clear();
    }
    const fs::path rel = canonSub.lexically_relative(canonBase);
    if (rel.empty() || rel.begin()->string() == "..") {
        errOut = "confinement directory escapes the allowed base directory";
        return false;
    }
    return ConfinePathUnderBase(canonSub.string(), candidate, resolvedOut, errOut);
}

} // namespace cmd
} // namespace smatchet
