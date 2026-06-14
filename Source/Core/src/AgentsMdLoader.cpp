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

// SECURITY (audit 2026-06-13 H1, P1): the AgentsMd override paths
// (cfg.AgentsMdGlobalPath / cfg.ProjectAgentsMdPath) are config-controlled, and
// their content is injected VERBATIM into every outbound LLM system prompt. A
// config-write attacker repointing an override at any readable file (id_rsa,
// cookies, known_hosts) would exfiltrate its first 64 KB off-host. Containment:
// the path must resolve — symlinks + `..` fully canonicalized — to a real file
// with a `.md` extension. fs::canonical resolves symlinks, so a `evil.md`
// symlink pointing at a secret resolves to the secret's real (non-.md) name here
// and is rejected; a direct repoint to a credential file (no `.md`) is rejected
// by the same extension pin. `.md` (not the literal `agents.md`) is the pin
// because the override contract legitimately allows any markdown instructions
// file (the defaults + tests use global.md / proj.md / my-instructions.md, not
// only agents.md) — an attacker cannot turn id_rsa into a `.md` without already
// holding its content, so the `.md` pin closes the repoint vector without
// breaking valid configs. Returns the canonical path on success.
bool ContainAgentsMdPath(const std::string& path, fs::path& realOut) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const fs::path real = fs::canonical(fs::path(path), ec);
    if (ec || real.empty()) {
        // Missing / unresolvable — not an attack signal (a default path that was
        // never created), so the caller treats this as a silent "no layer".
        return false;
    }
    std::string name = real.filename().string();
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    const bool isMarkdown = name.size() >= 3 && name.compare(name.size() - 3, 3, ".md") == 0;
    if (!isMarkdown) {
        LOG_WARN("AgentsMdLoader: refusing AgentsMd path '%s' — resolves to '%s', not a .md "
                 "file (containment guard, security audit H1).",
                 path.c_str(), real.generic_string().c_str());
        return false;
    }
    realOut = real;
    return true;
}

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
    // SECURITY (audit H1): contain the path BEFORE reading — refuse anything that
    // does not canonicalize to a recognised agents.md file. Read the canonical
    // (symlink-resolved) path so no symlink is re-traversed at open (TOCTOU).
    fs::path real;
    if (!ContainAgentsMdPath(path, real)) {
        return std::string();
    }
    std::string body;
    bool oversize = false;
    if (!ReadFileBytes(real.generic_string(), body, capBytes, oversize)) {
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

std::string LoadLayered(const std::string& globalPath, const std::string& projectPathOverride,
                        bool autoDiscoverProject) {
    std::string globalBody;
    if (!globalPath.empty()) {
        globalBody = LoadOneCapped(globalPath, kDefaultLayerCapBytes);
    }

    std::string projectPath = projectPathOverride;
    if (projectPath.empty() && autoDiscoverProject) {
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
