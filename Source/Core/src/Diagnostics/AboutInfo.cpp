// AboutInfo — see the header. THE ONLY TU IN THE REPO that includes the
// CMake-generated SmatchetBuildInfo.h. Keep it that way: the generated header
// lives in the build tree, is never committed, and is never packaged into the
// Unreal plugin's flat include dir (a copy there would go stale against the
// packaged .lib — the exact drift this feature exists to kill).
// docs/plans/active/about-dialog-help-menu.md.

#include "AboutInfo.h"

#include "ConfigManager.h"
#include "Diagnostics/HostMachineInfo.h"
#include "Logger.h"
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
#include "Ui/SmatchetImGuiHostC.h"
#endif

// Generated at build time by cmake/SmatchetGenerateBuildInfo.cmake. The
// __has_include guard is belt-and-braces: an out-of-tree build of Source/Core
// that never ran the generator still compiles, with every git field "unknown".
// (__has_include is formally C++17; the defined() wrapper keeps a bare C++14
// compiler happy — MSVC 2015u2+ and Clang both expose it as an extension.)
#if defined(__has_include)
#if __has_include(<SmatchetBuildInfo.h>)
#include <SmatchetBuildInfo.h>
#endif
#endif

#ifndef SMATCHET_BUILDINFO_AVAILABLE
#define SMATCHET_GIT_SHA "unknown"
#define SMATCHET_GIT_SHA_SHORT "unknown"
#define SMATCHET_GIT_BRANCH "unknown"
#define SMATCHET_GIT_DIRTY 0
#define SMATCHET_GIT_SHALLOW 0
#define SMATCHET_CXX_COMPILER_ID "unknown"
#define SMATCHET_CXX_COMPILER_VERSION "unknown"
#define SMATCHET_CMAKE_VERSION "unknown"
#define SMATCHET_DEPS_TEXT ""
#endif

// Build configuration arrives as a compile definition, NOT from the generated
// header: CMAKE_BUILD_TYPE is empty under the Visual Studio multi-config
// generator, so only a $<CONFIG> generator expression is correct there.
#ifndef SMATCHET_BUILD_CONFIG
#define SMATCHET_BUILD_CONFIG "unknown"
#endif

// ImGui is not on SmatchetCoreInterface's include path (only targets that link
// ImGuiLib see it), and this TU is compiled into the doctest rig too — so the
// version is best-effort.
#if defined(__has_include)
#if __has_include("imgui.h")
#include "imgui.h"
#endif
#endif

#include <sstream>

namespace smatchet {
namespace diagnostics {

namespace {

const char* const kUnknown = "unknown";

// Label column width for the report text ("data dir:" is the longest key).
const std::size_t kLabelWidth = 11;

void AppendField(std::ostringstream& out, const char* label, const std::string& value) {
    std::string key(label);
    key += ":";
    while (key.size() < kLabelWidth) {
        key += ' ';
    }
    out << key << (value.empty() ? std::string(kUnknown) : value) << "\n";
}

/// Split one manifest record on '|'. The generator refuses any record containing
/// a quote or backslash, so no unescaping is needed here.
std::vector<std::string> SplitRecord(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t bar = line.find('|', pos);
        if (bar == std::string::npos) {
            fields.push_back(line.substr(pos));
            return fields;
        }
        fields.push_back(line.substr(pos, bar - pos));
        pos = bar + 1;
    }
}

} // namespace

std::vector<AboutDep> ParseDepManifest(const std::string& text) {
    std::vector<AboutDep> deps;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;

        // Tolerate CRLF-normalised input — the generator emits \n, but a text-mode
        // round-trip through a Windows editor would not.
        while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ')) {
            line.erase(line.size() - 1);
        }
        if (line.empty()) {
            continue;
        }

        const std::vector<std::string> fields = SplitRecord(line);
        if (fields.size() != 4 || fields[0].empty() || fields[1].empty()) {
            // %s, never the record as the format string — a stray '%' in a dep URL
            // would otherwise walk the varargs (LOG_* is printf-style, not stream-style).
            LOG_WARN("AboutInfo: dropping malformed dep-manifest record: %s", line.c_str());
            continue;
        }
        AboutDep dep;
        dep.Name = fields[0];
        dep.Version = fields[1];
        dep.License = fields[2];
        dep.Url = fields[3];
        deps.push_back(dep);
    }
    return deps;
}

std::string FormatAboutGitLine(const AboutGitInfo& git) {
    const std::string sha = git.ShaShort.empty() ? git.Sha : git.ShaShort;
    if (sha.empty() || sha == kUnknown) {
        return kUnknown; // never a dangling "()" or a bare " +dirty"
    }
    std::string line = sha;
    if (!git.Branch.empty() && git.Branch != kUnknown) {
        line += " (" + git.Branch + ")";
    }
    if (git.Dirty) {
        line += " +dirty";
    }
    if (git.Shallow) {
        line += " [shallow clone]";
    }
    return line;
}

AboutInfo GatherAboutInfo(const std::string& appVersion, const std::string& githubRepo) {
    AboutInfo info;
    info.AppName = "Smatchet";
    info.Version = appVersion.empty() ? std::string(kUnknown) : appVersion;
    if (!githubRepo.empty()) {
        info.RepoUrl = "https://github.com/" + githubRepo;
    }

    info.Build.Config = SMATCHET_BUILD_CONFIG;
    info.Build.CompilerId = SMATCHET_CXX_COMPILER_ID;
    info.Build.CompilerVersion = SMATCHET_CXX_COMPILER_VERSION;
    info.Build.CMakeVersion = SMATCHET_CMAKE_VERSION;
    info.Build.DateTime = std::string(__DATE__) + " " + __TIME__;
#if defined(IMGUI_VERSION)
    info.Build.ImGuiVersion = IMGUI_VERSION;
#else
    info.Build.ImGuiVersion = kUnknown;
#endif
    info.Build.TargetArch = HostArchName();
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
    // Mirrors BugReportService::GatherContext so the two provenance surfaces
    // never disagree about which host binary is running.
    const char* buildTag = SmatchetHost_GetBuildTag();
    info.Build.BuildTag = (buildTag != nullptr && buildTag[0] != '\0') ? buildTag : "unreal";
#else
    info.Build.BuildTag = "standalone";
#endif

    info.Git.Sha = SMATCHET_GIT_SHA;
    info.Git.ShaShort = SMATCHET_GIT_SHA_SHORT;
    info.Git.Branch = SMATCHET_GIT_BRANCH;
    info.Git.Dirty = SMATCHET_GIT_DIRTY != 0;
    info.Git.Shallow = SMATCHET_GIT_SHALLOW != 0;

    const TrackerConfig cfg = ConfigManager::Load();
    info.Runtime.Os = HostOsName();
    info.Runtime.Arch = HostArchName();
    const HostMachineInfo machine = DetectHostMachine();
    info.Runtime.EmulationResolved = machine.Resolved;
    if (machine.Resolved) {
        info.Runtime.Emulated = machine.Emulated;
        info.Runtime.NativeArch = machine.NativeArch;
    }
    info.Runtime.Tracker = cfg.TrackerType.empty() ? std::string("(none)") : cfg.TrackerType;
    info.Runtime.UserDataDir = ConfigManager::GetUserDataDirectory();
    info.Runtime.ConfigPath = ConfigManager::GetConfigPath();

    info.Deps = ParseDepManifest(SMATCHET_DEPS_TEXT);
    return info;
}

std::string BuildAboutReportText(const AboutInfo& info) {
    std::ostringstream out;
    out << info.AppName << " " << info.Version << "\n";

    AppendField(out, "build cfg", info.Build.Config);
    AppendField(out, "build tag", info.Build.BuildTag);
    AppendField(out, "built", info.Build.DateTime);
    AppendField(out, "compiler", info.Build.CompilerId + " " + info.Build.CompilerVersion);
    AppendField(out, "cmake", info.Build.CMakeVersion);
    AppendField(out, "imgui", info.Build.ImGuiVersion);
    AppendField(out, "arch", info.Build.TargetArch);
    AppendField(out, "git", FormatAboutGitLine(info.Git));
    AppendField(out, "os", info.Runtime.Os);
    if (info.Runtime.EmulationResolved && !info.Runtime.NativeArch.empty()) {
        // Only meaningful when the probe resolved — otherwise omit rather than guess.
        AppendField(out, "host", info.Runtime.NativeArch + (info.Runtime.Emulated ? " (emulated)" : ""));
    }
    AppendField(out, "tracker", info.Runtime.Tracker);
    if (!info.RepoUrl.empty()) {
        AppendField(out, "repo", info.RepoUrl);
    }
    AppendField(out, "data dir", info.Runtime.UserDataDir);
    AppendField(out, "config", info.Runtime.ConfigPath);

    if (!info.Deps.empty()) {
        out << "third-party:\n";
        for (std::size_t i = 0; i < info.Deps.size(); ++i) {
            const AboutDep& dep = info.Deps[i];
            out << "  - " << dep.Name << " " << dep.Version;
            if (!dep.License.empty()) {
                out << " (" << dep.License << ")";
            }
            if (!dep.Url.empty()) {
                out << " " << dep.Url;
            }
            out << "\n";
        }
    }
    return out.str();
}

} // namespace diagnostics
} // namespace smatchet
