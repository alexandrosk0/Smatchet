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

} // namespace cmd
} // namespace smatchet
