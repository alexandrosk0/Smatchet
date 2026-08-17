// AboutInfo doctests — the pure data layer behind Help > About.
// docs/plans/shipped/about-dialog-help-menu.md Slice 3.
//
// Deliberately asserts NOTHING machine-specific: never the real SHA, branch,
// build date, config dir or compiler version — those differ per checkout and per
// CI runner. What IS asserted is shape: the manifest parses, malformed records
// are dropped rather than crashing, the report text is well-formed, and the
// "git unknown" path never renders a dangling "()".

#include "Diagnostics/AboutInfo.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace smatchet::diagnostics;

namespace {

AboutInfo MakeSyntheticInfo() {
    AboutInfo info;
    info.AppName = "Smatchet";
    info.Version = "9.9.9";
    info.RepoUrl = "https://github.com/example/repo";
    info.Build.Config = "RelWithDebInfo";
    info.Build.BuildTag = "standalone";
    info.Build.DateTime = "Jan  1 2026 00:00:00";
    info.Build.CompilerId = "MSVC";
    info.Build.CompilerVersion = "19.38.33145.0";
    info.Build.CMakeVersion = "3.28.1";
    info.Build.ImGuiVersion = "1.92.7";
    info.Build.TargetArch = "x86_64";
    info.Git.Sha = "0123456789abcdef0123456789abcdef01234567";
    info.Git.ShaShort = "0123456789ab";
    info.Git.Branch = "develop";
    info.Runtime.Os = "Windows";
    info.Runtime.Arch = "x86_64";
    info.Runtime.Tracker = "jira";
    info.Runtime.UserDataDir = "C:/data";
    info.Runtime.ConfigPath = "C:/data/config.json";
    return info;
}

} // namespace

// --------------------------------------------------------------------------
// ParseDepManifest
// --------------------------------------------------------------------------

TEST_CASE("ParseDepManifest — splits name|version|license|url") {
    const std::vector<AboutDep> deps =
        ParseDepManifest("Dear ImGui|v1.92.7-docking|MIT|https://github.com/ocornut/imgui\n"
                         "nlohmann/json|v3.11.3|MIT|https://github.com/nlohmann/json\n");
    REQUIRE(deps.size() == 2);
    CHECK(deps[0].Name == "Dear ImGui");
    CHECK(deps[0].Version == "v1.92.7-docking");
    CHECK(deps[0].License == "MIT");
    CHECK(deps[0].Url == "https://github.com/ocornut/imgui");
    CHECK(deps[1].Name == "nlohmann/json");
}

TEST_CASE("ParseDepManifest — drops malformed records, keeps good ones") {
    const std::vector<AboutDep> deps = ParseDepManifest("good|1.0|MIT|https://x\n"
                                                        "too|few|fields\n"       // 3 fields
                                                        "a|b|c|d|e\n"            // 5 fields
                                                        "|1.0|MIT|https://x\n"   // empty name
                                                        "named||MIT|https://x\n" // empty version
                                                        "\n"                     // blank
                                                        "also-good|2.0|Zlib|https://y\n");
    REQUIRE(deps.size() == 2);
    CHECK(deps[0].Name == "good");
    CHECK(deps[1].Name == "also-good");
}

TEST_CASE("ParseDepManifest — tolerates CRLF and a missing trailing newline") {
    const std::vector<AboutDep> deps = ParseDepManifest("a|1|MIT|https://x\r\nb|2|MIT|https://y");
    REQUIRE(deps.size() == 2);
    CHECK(deps[0].Url == "https://x"); // no stray \r glued to the last field
    CHECK(deps[1].Name == "b");
}

TEST_CASE("ParseDepManifest — empty input yields no records") {
    CHECK(ParseDepManifest("").empty());
    CHECK(ParseDepManifest("\n\n\n").empty());
}

// --------------------------------------------------------------------------
// FormatAboutGitLine
// --------------------------------------------------------------------------

TEST_CASE("FormatAboutGitLine — renders sha, branch, dirty and shallow markers") {
    AboutGitInfo git;
    git.ShaShort = "0123456789ab";
    git.Branch = "develop";
    CHECK(FormatAboutGitLine(git) == "0123456789ab (develop)");

    git.Dirty = true;
    CHECK(FormatAboutGitLine(git) == "0123456789ab (develop) +dirty");

    git.Shallow = true;
    CHECK(FormatAboutGitLine(git) == "0123456789ab (develop) +dirty [shallow clone]");
}

TEST_CASE("FormatAboutGitLine — unknown/absent sha never renders a dangling ()") {
    AboutGitInfo unknown;
    unknown.Sha = "unknown";
    unknown.ShaShort = "unknown";
    unknown.Branch = "unknown";
    unknown.Dirty = true; // must not leak a bare " +dirty" with no sha
    CHECK(FormatAboutGitLine(unknown) == "unknown");

    AboutGitInfo empty;
    CHECK(FormatAboutGitLine(empty) == "unknown");

    // Detached HEAD: a real sha with no usable branch — no empty parens.
    AboutGitInfo detached;
    detached.ShaShort = "0123456789ab";
    detached.Branch = "";
    CHECK(FormatAboutGitLine(detached) == "0123456789ab");
}

// --------------------------------------------------------------------------
// BuildAboutReportText
// --------------------------------------------------------------------------

TEST_CASE("BuildAboutReportText — carries version, git and compiler lines") {
    const std::string text = BuildAboutReportText(MakeSyntheticInfo());
    CHECK(text.find("Smatchet 9.9.9") != std::string::npos);
    CHECK(text.find("git:") != std::string::npos);
    CHECK(text.find("compiler:") != std::string::npos);
    CHECK(text.find("MSVC 19.38.33145.0") != std::string::npos);
    CHECK(text.find("0123456789ab (develop)") != std::string::npos);
}

TEST_CASE("BuildAboutReportText — LF-only, terminated by exactly one newline") {
    const std::string text = BuildAboutReportText(MakeSyntheticInfo());
    REQUIRE(!text.empty());
    CHECK(text.find('\r') == std::string::npos);
    CHECK(text[text.size() - 1] == '\n');
    CHECK(text[text.size() - 2] != '\n');
}

TEST_CASE("BuildAboutReportText — empty fields render 'unknown', never blank") {
    AboutInfo bare;
    bare.AppName = "Smatchet";
    bare.Version = "0.0.0";
    const std::string text = BuildAboutReportText(bare);
    CHECK(text.find("compiler:") != std::string::npos);
    CHECK(text.find("unknown") != std::string::npos);
    // No repo line at all when the URL is absent — better than "repo: unknown".
    CHECK(text.find("repo:") == std::string::npos);
    // Host line is omitted (not guessed) when the emulation probe never resolved.
    CHECK(text.find("host:") == std::string::npos);
}

TEST_CASE("BuildAboutReportText — third-party section lists every dep") {
    AboutInfo info = MakeSyntheticInfo();
    info.Deps = ParseDepManifest("Dear ImGui|v1.92.7|MIT|https://github.com/ocornut/imgui\n");
    const std::string text = BuildAboutReportText(info);
    CHECK(text.find("third-party:") != std::string::npos);
    CHECK(text.find("  - Dear ImGui v1.92.7 (MIT) https://github.com/ocornut/imgui") != std::string::npos);
    // Raw record separators must never reach the rendered text.
    CHECK(text.find('|') == std::string::npos);
}

TEST_CASE("BuildAboutReportText — agent-debug line appears only on an instrumented build") {
    // The OFF build is the normal one, so it carries no row at all — a permanent
    // "agent-dbg: off" would be noise on every bug report. app.version's JSON
    // carries the field unconditionally instead (machine readers need the false).
    AboutInfo off = MakeSyntheticInfo();
    off.Build.AgentDebug = false;
    CHECK(BuildAboutReportText(off).find("agent-dbg") == std::string::npos);

    AboutInfo on = MakeSyntheticInfo();
    on.Build.AgentDebug = true;
    // One pad space: AppendField pads "agent-dbg:" (10) out to kLabelWidth (11).
    CHECK(BuildAboutReportText(on).find("agent-dbg: on\n") != std::string::npos);
}

TEST_CASE("GatherAboutInfo — AgentDebug mirrors the compile-time define, never a runtime probe") {
    const AboutInfo info = GatherAboutInfo("1.2.3", "example/repo");
    // The doctest rig never gets SMATCHET_AGENT_DEBUG on AboutInfo.cpp: the option
    // is wired PRIVATE to SmatchetStandalone, and tests/CMakeLists.txt force-sets
    // it on SmatchetAgentDebug.test.cpp's own TU only. So false is the expected
    // value here — and a true would mean the define leaked target-wide.
    CHECK(info.Build.AgentDebug == false);
}

// --------------------------------------------------------------------------
// GatherAboutInfo — the real build-generated manifest
// --------------------------------------------------------------------------

TEST_CASE("GatherAboutInfo — passes through version and builds the repo URL") {
    const AboutInfo info = GatherAboutInfo("1.2.3", "example/repo");
    CHECK(info.AppName == "Smatchet");
    CHECK(info.Version == "1.2.3");
    CHECK(info.RepoUrl == "https://github.com/example/repo");

    const AboutInfo noRepo = GatherAboutInfo("", "");
    CHECK(noRepo.Version == "unknown"); // sentinel, never an empty version
    CHECK(noRepo.RepoUrl.empty());
}

TEST_CASE("GatherAboutInfo — build fields are populated, never empty") {
    const AboutInfo info = GatherAboutInfo("1.2.3", "example/repo");
    // Values are machine-specific; only non-emptiness is stable.
    CHECK(!info.Build.Config.empty());
    CHECK(!info.Build.CompilerId.empty());
    CHECK(!info.Build.DateTime.empty());
    CHECK(!info.Build.TargetArch.empty());
    CHECK(!info.Build.BuildTag.empty());
    CHECK(!info.Runtime.Os.empty());
    CHECK(!info.Runtime.Arch.empty());
    CHECK(!info.Runtime.Tracker.empty());
    // Git may legitimately be "unknown" (no git / shallow / out-of-tree build),
    // but the field is always set to something renderable.
    CHECK(!info.Git.Sha.empty());
    CHECK(!FormatAboutGitLine(info.Git).empty());
}

TEST_CASE("GatherAboutInfo — the generated dep manifest is present and well-formed") {
    const AboutInfo info = GatherAboutInfo("1.2.3", "example/repo");
    // 10 always-on deps + up to 5 conditional ones. A regression in the CMake
    // codegen (empty SMATCHET_DEPS_TEXT) shows up here and nowhere else.
    REQUIRE(info.Deps.size() >= 9);
    for (std::size_t i = 0; i < info.Deps.size(); ++i) {
        const AboutDep& dep = info.Deps[i];
        CHECK(!dep.Name.empty());
        CHECK(!dep.Version.empty());
        CHECK(dep.Name.find('|') == std::string::npos);
        CHECK(dep.Version.find('|') == std::string::npos);
        CHECK(dep.License.find('|') == std::string::npos);
        CHECK(dep.Url.find('|') == std::string::npos);
    }
}
