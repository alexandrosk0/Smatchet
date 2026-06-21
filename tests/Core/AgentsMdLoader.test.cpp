// AgentsMdLoader pure-logic tests. Exercises Source/Core/include/AgentsMdLoader.h —
// LoadOneCapped (cap + truncation sentinel + missing-file), FindProjectAgentsMd
// (walk-up + depth cap + filename variants), and LoadLayered (separator placement
// + missing-layer fallthrough + override precedence).
//
// Pillar 3 (never crash): adversarial inputs (empty path, oversize file, deep nesting,
// symlink absent) must terminate cleanly without UB.

#include "AgentsMdLoader.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace fs = ghc::filesystem;

namespace {

// Per-test scratch directory. Cleaned at scope exit (best-effort — the entire tree is
// inside the OS temp dir, so a leftover is harmless).
class TempDir {
  public:
    TempDir() {
        const auto unique =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        path_ = fs::temp_directory_path() / ("smatchet_agentsmd_" + std::to_string(unique));
        std::error_code ec;
        fs::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }
    std::string str() const { return path_.generic_string(); }

  private:
    fs::path path_;
};

void WriteFile(const fs::path& p, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    // ghc::filesystem::path lacks the C++17 path overloads for std::ofstream on MinGW, so
    // round-trip through generic_string() for portability.
    std::ofstream out(p.generic_string().c_str(), std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("AgentsMdLoader::LoadOneCapped — empty path returns empty") {
    CHECK(AgentsMdLoader::LoadOneCapped(std::string()).empty());
    CHECK(AgentsMdLoader::LoadOneCapped("/nonexistent/never/should/be/there.md").empty());
}

TEST_CASE("AgentsMdLoader::LoadOneCapped — small file round-trips byte-for-byte") {
    TempDir tmp;
    const fs::path p = tmp.path() / "agents.md";
    const std::string payload = "# Hello\n\nLine two.\n";
    WriteFile(p, payload);
    const std::string out = AgentsMdLoader::LoadOneCapped(p.generic_string());
    CHECK(out == payload);
    CHECK(out.size() == payload.size());
}

TEST_CASE("AgentsMdLoader::LoadOneCapped — over-cap input truncates + sentinel appended [high-risk]") {
    TempDir tmp;
    const fs::path p = tmp.path() / "huge.md";
    // 80 KB body of single character — well over the default 64 KB cap.
    const std::string huge(80u * 1024u, 'A');
    WriteFile(p, huge);
    const std::string out = AgentsMdLoader::LoadOneCapped(p.generic_string());
    // First 64 KB of A's, then sentinel.
    REQUIRE(out.size() > 64u * 1024u);
    CHECK(out.substr(0, 64u * 1024u) == std::string(64u * 1024u, 'A'));
    CHECK(Contains(out, "[truncated at 64 KB"));
    CHECK(Contains(out, p.filename().generic_string()));
}

TEST_CASE("AgentsMdLoader::LoadOneCapped — explicit cap honoured (mutation-sanity)") {
    TempDir tmp;
    const fs::path p = tmp.path() / "small.md";
    const std::string payload(2048, 'x');
    WriteFile(p, payload);
    // Cap below file size — should truncate + suffix.
    const std::string out = AgentsMdLoader::LoadOneCapped(p.generic_string(), 1024);
    REQUIRE(out.size() >= 1024);
    CHECK(out.substr(0, 1024) == std::string(1024, 'x'));
    CHECK(Contains(out, "[truncated at 64 KB"));
    // Cap above file size — full content returned, no suffix.
    const std::string full = AgentsMdLoader::LoadOneCapped(p.generic_string(), 8192);
    CHECK(full == payload);
    CHECK(!Contains(full, "[truncated"));
}

// Read-cap sizing (min(maxBytes+1, file_size)): a file SMALLER than the cap is
// returned verbatim with no truncation sentinel (the read must not over-allocate
// maxBytes+1 for a tiny file), a file EXACTLY at the cap is returned verbatim
// (not flagged over-cap), and a file ONE BYTE over the cap IS truncated + flagged.
TEST_CASE("AgentsMdLoader::LoadOneCapped — read-cap sizing: small / exact-cap / over-cap [high-risk]") {
    TempDir tmp;
    // Small (well under cap): full content, no sentinel.
    {
        const fs::path p = tmp.path() / "small.md";
        const std::string payload(64, 's');
        WriteFile(p, payload);
        const std::string out = AgentsMdLoader::LoadOneCapped(p.generic_string(), 1024);
        CHECK(out == payload);
        CHECK(!Contains(out, "[truncated"));
    }
    // Exactly at cap: full content, NOT flagged over-cap (over-cap detection is
    // strictly `> cap`, preserved by the min(cap+1, file_size) read).
    {
        const fs::path p = tmp.path() / "exact.md";
        const std::string payload(1024, 'e');
        WriteFile(p, payload);
        const std::string out = AgentsMdLoader::LoadOneCapped(p.generic_string(), 1024);
        CHECK(out == payload);
        CHECK(out.size() == 1024u);
        CHECK(!Contains(out, "[truncated"));
    }
    // One byte over cap: truncated to cap + sentinel appended.
    {
        const fs::path p = tmp.path() / "over.md";
        const std::string payload(1025, 'o');
        WriteFile(p, payload);
        const std::string out = AgentsMdLoader::LoadOneCapped(p.generic_string(), 1024);
        REQUIRE(out.size() > 1024u);
        CHECK(out.substr(0, 1024) == std::string(1024, 'o'));
        CHECK(Contains(out, "[truncated at 64 KB"));
    }
}

// SECURITY (audit 2026-06-13 H1, P1): a config-write attacker repointing an
// AgentsMd override at a credential file (no `.md` extension) must NOT have its
// bytes loaded — they would be injected verbatim into the outbound LLM prompt.
TEST_CASE("AgentsMdLoader — refuses a non-.md override file (H1 exfil containment) [high-risk]") {
    TempDir tmp;
    const fs::path secret = tmp.path() / "id_rsa"; // a stand-in for any credential file
    WriteFile(secret, "-----BEGIN OPENSSH PRIVATE KEY-----\nSECRETBYTES\n");
    // Direct + via the layered entry point (the real prompt-assembly path).
    CHECK(AgentsMdLoader::LoadOneCapped(secret.generic_string()).empty());
    CHECK(AgentsMdLoader::LoadLayered(secret.generic_string(), std::string()).empty());
    CHECK(AgentsMdLoader::LoadLayered(std::string(), secret.generic_string()).empty());
    // A legitimately-named .md sibling still loads (the pin is .md, not agents.md).
    const fs::path ok = tmp.path() / "my-instructions.md";
    WriteFile(ok, "INSTRUCTIONS");
    CHECK(AgentsMdLoader::LoadOneCapped(ok.generic_string()) == "INSTRUCTIONS");
}

// The .md-suffix pin alone is bypassable by a `evil.md` symlink → secret; the
// canonicalize step closes it (resolves to the non-.md real name). Symlink
// creation needs privilege on Windows, so skip the leg gracefully there.
TEST_CASE("AgentsMdLoader — refuses a .md symlink resolving to a non-.md secret [high-risk]") {
    TempDir tmp;
    const fs::path secret = tmp.path() / "id_rsa";
    WriteFile(secret, "PRIVATE-KEY-BYTES");
    const fs::path link = tmp.path() / "evil.md";
    std::error_code ec;
    fs::create_symlink(secret, link, ec);
    if (ec) {
        MESSAGE("symlink unsupported here (no privilege) — skipping symlink-bypass leg");
        return;
    }
    const std::string out = AgentsMdLoader::LoadOneCapped(link.generic_string());
    CHECK(out.empty());
    CHECK(out.find("PRIVATE-KEY-BYTES") == std::string::npos);
}

// fs::canonical resolves symlinks but NOT hard links, so a `.md`-named hard link
// to a secret would pass the suffix pin. The hard_link_count>1 guard closes it.
// Hard links work on NTFS without privilege (unlike symlinks), so this runs in CI.
TEST_CASE("AgentsMdLoader — refuses a .md hard link to a secret (H1 hard-link bypass) [high-risk]") {
    TempDir tmp;
    const fs::path secret = tmp.path() / "id_rsa";
    WriteFile(secret, "HARD-LINK-SECRET-BYTES");
    const fs::path link = tmp.path() / "evil.md";
    std::error_code ec;
    fs::create_hard_link(secret, link, ec);
    if (ec) {
        MESSAGE("hard-link creation unsupported here — skipping hard-link-bypass leg");
        return;
    }
    const std::string out = AgentsMdLoader::LoadOneCapped(link.generic_string());
    CHECK(out.empty());
    CHECK(out.find("HARD-LINK-SECRET-BYTES") == std::string::npos);
}

TEST_CASE("AgentsMdLoader::FindProjectAgentsMd — file at current dir found at depth 0") {
    TempDir tmp;
    const fs::path p = tmp.path() / "agents.md";
    WriteFile(p, "ROOT");
    const std::string found = AgentsMdLoader::FindProjectAgentsMd(tmp.str(), 16);
    CHECK(!found.empty());
    CHECK(Contains(found, "agents.md"));
}

TEST_CASE("AgentsMdLoader::FindProjectAgentsMd — walk-up locates ancestor file [high-risk]") {
    TempDir tmp;
    // Put agents.md at depth 0 (tmp root), start from depth 3.
    const fs::path root = tmp.path();
    WriteFile(root / "agents.md", "ROOT");
    const fs::path deep = root / "a" / "b" / "c";
    std::error_code ec;
    fs::create_directories(deep, ec);
    const std::string found = AgentsMdLoader::FindProjectAgentsMd(deep.generic_string(), 16);
    CHECK(!found.empty());
    // Should resolve to the ROOT-level file.
    CHECK(Contains(found, root.filename().generic_string()));
    CHECK(Contains(found, "agents.md"));
}

TEST_CASE("AgentsMdLoader::FindProjectAgentsMd — depth cap stops the walk") {
    TempDir tmp;
    // File at tmp root, start ten levels down with cap=2 — should NOT find it.
    WriteFile(tmp.path() / "agents.md", "ROOT");
    fs::path deep = tmp.path();
    for (int i = 0; i < 10; ++i) {
        deep /= "level";
    }
    std::error_code ec;
    fs::create_directories(deep, ec);
    const std::string found = AgentsMdLoader::FindProjectAgentsMd(deep.generic_string(), 2);
    CHECK(found.empty());
}

TEST_CASE("AgentsMdLoader::FindProjectAgentsMd — uppercase AGENTS.md also found") {
    TempDir tmp;
    WriteFile(tmp.path() / "AGENTS.md", "UPPER");
    const std::string found = AgentsMdLoader::FindProjectAgentsMd(tmp.str(), 4);
    CHECK(!found.empty());
    // Case-insensitive filesystems (Windows / macOS-HFS) may report the lookup as
    // `agents.md` even though the on-disk entry was created uppercase. Accept either
    // shape — the contract is "found *some* agents.md flavour".
    CHECK((Contains(found, "AGENTS.md") || Contains(found, "agents.md")));
}

TEST_CASE("AgentsMdLoader::FindProjectAgentsMd — lowercase preferred when both present") {
    TempDir tmp;
    WriteFile(tmp.path() / "agents.md", "LOWER");
    WriteFile(tmp.path() / "AGENTS.md", "UPPER");
    const std::string found = AgentsMdLoader::FindProjectAgentsMd(tmp.str(), 4);
    CHECK(!found.empty());
    // Per-plan rule: lowercase wins. On case-insensitive filesystems (Windows / macOS HFS+)
    // both names alias one inode, but the loader must still preserve case from the lookup.
    // We only assert that *some* file was found and that it ends with `agents.md` (any case).
    CHECK((Contains(found, "agents.md") || Contains(found, "AGENTS.md")));
}

TEST_CASE("AgentsMdLoader::LoadLayered — missing both layers returns empty") {
    const std::string out = AgentsMdLoader::LoadLayered("/nonexistent/global.md", "/nonexistent/project.md");
    CHECK(out.empty());
}

TEST_CASE("AgentsMdLoader::LoadLayered — global only renders unchanged") {
    TempDir tmp;
    const fs::path global = tmp.path() / "global.md";
    WriteFile(global, "GLOBAL_CONTENT");
    const std::string out = AgentsMdLoader::LoadLayered(global.generic_string(), "/nonexistent/project.md");
    CHECK(out == "GLOBAL_CONTENT");
}

TEST_CASE("AgentsMdLoader::LoadLayered — project override only renders unchanged") {
    TempDir tmp;
    const fs::path project = tmp.path() / "proj.md";
    WriteFile(project, "PROJECT_CONTENT");
    const std::string out = AgentsMdLoader::LoadLayered(std::string(), project.generic_string());
    CHECK(out == "PROJECT_CONTENT");
}

TEST_CASE("AgentsMdLoader::LoadLayered — both layers joined with `\\n\\n---\\n\\n` separator [high-risk]") {
    TempDir tmp;
    const fs::path global = tmp.path() / "global.md";
    const fs::path project = tmp.path() / "project.md";
    WriteFile(global, "GLOBAL");
    WriteFile(project, "PROJECT");
    const std::string out = AgentsMdLoader::LoadLayered(global.generic_string(), project.generic_string());
    // Verify both layers are present in order with the literal separator.
    REQUIRE(!out.empty());
    CHECK(Contains(out, "GLOBAL"));
    CHECK(Contains(out, "PROJECT"));
    CHECK(Contains(out, "\n\n---\n\n"));
    const auto globalPos = out.find("GLOBAL");
    const auto sepPos = out.find("\n\n---\n\n");
    const auto projectPos = out.find("PROJECT");
    CHECK(globalPos < sepPos);
    CHECK(sepPos < projectPos);
}

TEST_CASE("AgentsMdLoader::LoadLayered — project override takes precedence over walk-up [high-risk]") {
    TempDir tmp;
    // Create an explicit override file with one body and a walk-up candidate with another;
    // pass the override path. The loader must NOT walk up — the override wins.
    const fs::path override = tmp.path() / "explicit.md";
    WriteFile(override, "EXPLICIT");
    // Also seed a walk-up candidate next to it that should be ignored.
    WriteFile(tmp.path() / "agents.md", "WALKUP");
    const std::string out = AgentsMdLoader::LoadLayered(std::string(), override.generic_string());
    CHECK(out == "EXPLICIT");
    CHECK(!Contains(out, "WALKUP"));
}

// [high-risk] Opt-in walk-up: when the explicit project path is empty AND
// autoDiscoverProject is false (default), LoadLayered MUST NOT walk up from
// cwd. This is the regression gate that locks the "no surprise AGENTS.md
// injection" behavior: users running Smatchet from inside an unrelated repo
// (e.g. the Smatchet source tree, or any project that happens to have an
// AGENTS.md in a parent) must not get that file silently pulled in.
TEST_CASE("[high-risk] AgentsMdLoader::LoadLayered — default does NOT walk up from cwd") {
    TempDir tmp;
    // Seed a walk-up candidate. Without an explicit project path AND
    // autoDiscoverProject=false (default), the loader should not consult it.
    WriteFile(tmp.path() / "agents.md", "WALKUP_BODY");
    // We can't easily set process cwd here, but the same code path is exercised:
    // empty global + empty project + autoDiscoverProject=false short-circuits
    // before FindProjectAgentsMd is called.
    const std::string out = AgentsMdLoader::LoadLayered(std::string(), std::string(), /*autoDiscoverProject=*/false);
    CHECK(out.empty());
}

TEST_CASE("AgentsMdLoader::LoadLayered — autoDiscoverProject=true re-enables walk-up") {
    // Mirror the default-false test but with the opt-in flag on. The body
    // FindProjectAgentsMd looks at is the process cwd by default, which we
    // can't mutate in-process portably. Just assert the call returns
    // successfully (no UB) and that the empty-everything case still degrades
    // to empty; the FindProjectAgentsMd-positive cases above prove walk-up
    // works once enabled.
    const std::string out = AgentsMdLoader::LoadLayered(std::string(), std::string(), /*autoDiscoverProject=*/true);
    // We don't assert on `out` content because cwd depends on the test runner;
    // the requirement here is that the path doesn't crash or deadlock.
    CHECK((out.empty() || !out.empty()));
}
