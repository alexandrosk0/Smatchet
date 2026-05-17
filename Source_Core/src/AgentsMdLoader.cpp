#include "AgentsMdLoader.h"

#include "Logger.h"

#include <ghc/filesystem.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = ghc::filesystem;

namespace AgentsMdLoader {

namespace {

constexpr const char* kLayerSeparator = "\n\n---\n\n";
constexpr const char* kSentinelPrefix = "\n\n---\n[truncated at 64 KB — see ";
constexpr const char* kSentinelSuffix = "]\n";

// File-name variants tried at every walk-up step. Lowercase first so it wins
// when both lowercase + uppercase coexist in the same directory (Phase C plan
// rule — "lowercase preferred").
const char* const kAgentsMdNames[] = {"agents.md", ".agents.md", "AGENTS.md"};

bool ReadFileBytes(const std::string& path, std::string& out, std::size_t maxBytes, bool& outOversize) {
    outOversize = false;
    out.clear();
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    // Read up to maxBytes+1 to detect over-cap inputs cheaply without scanning
    // the full file size via filesystem (symlink resolution differences across
    // platforms make `file_size` brittle for this).
    out.resize(maxBytes + 1);
    in.read(&out[0], static_cast<std::streamsize>(out.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) {
        out.clear();
        return true;
    }
    out.resize(static_cast<std::size_t>(got));
    if (out.size() > maxBytes) {
        out.resize(maxBytes);
        outOversize = true;
    }
    return true;
}

} // namespace

std::string LoadOneCapped(const std::string& path, std::size_t capBytes) {
    std::string body;
    bool oversize = false;
    if (!ReadFileBytes(path, body, capBytes, oversize)) {
        return std::string();
    }
    if (oversize) {
        // Resolve absolute path for the sentinel so multi-layer truncation messages
        // are distinguishable. Best-effort: a non-resolvable path is round-tripped
        // verbatim — the user still sees which layer was capped.
        std::error_code ec;
        const fs::path abs = fs::absolute(path, ec);
        const std::string shown = (ec || abs.empty()) ? path : abs.generic_string();
        body.append(kSentinelPrefix);
        body.append(shown);
        body.append(kSentinelSuffix);
    }
    return body;
}

std::string FindProjectAgentsMd(const std::string& startDir, std::size_t maxDepth) {
    std::error_code ec;
    fs::path dir;
    if (startDir.empty()) {
        dir = fs::current_path(ec);
        if (ec) {
            return std::string();
        }
    } else {
        dir = fs::path(startDir);
    }
    // Normalize to absolute up-front so the walk-up doesn't get stuck on `.` or `./foo`.
    if (dir.is_relative()) {
        dir = fs::absolute(dir, ec);
        if (ec) {
            return std::string();
        }
    }

    for (std::size_t depth = 0; depth <= maxDepth; ++depth) {
        for (const char* name : kAgentsMdNames) {
            fs::path candidate = dir / name;
            std::error_code statEc;
            if (fs::exists(candidate, statEc) && !statEc && fs::is_regular_file(candidate, statEc)) {
                return candidate.generic_string();
            }
        }
        if (!dir.has_parent_path()) {
            break;
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) {
            // Reached filesystem root (`C:\` on Windows is its own parent in some toolchains).
            break;
        }
        dir = parent;
    }
    return std::string();
}

std::string LoadLayered(const std::string& globalPath, const std::string& projectPathOverride) {
    std::string globalBody;
    if (!globalPath.empty()) {
        globalBody = LoadOneCapped(globalPath, kDefaultLayerCapBytes);
    }

    std::string projectPath = projectPathOverride;
    if (projectPath.empty()) {
        projectPath = FindProjectAgentsMd(std::string(), kDefaultWalkUpMaxDepth);
    }
    std::string projectBody;
    if (!projectPath.empty()) {
        projectBody = LoadOneCapped(projectPath, kDefaultLayerCapBytes);
    }

    if (globalBody.empty() && projectBody.empty()) {
        return std::string();
    }
    if (globalBody.empty()) {
        return projectBody;
    }
    if (projectBody.empty()) {
        return globalBody;
    }
    std::string merged;
    merged.reserve(globalBody.size() + projectBody.size() + 16);
    merged.append(globalBody);
    merged.append(kLayerSeparator);
    merged.append(projectBody);
    return merged;
}

} // namespace AgentsMdLoader
