#ifndef SMATCHET_DIAGNOSTICS_ABOUT_INFO_H
#define SMATCHET_DIAGNOSTICS_ABOUT_INFO_H

#include <string>
#include <vector>

// AboutInfo — the pure data layer behind Help > About. Assembles the version /
// build / git / runtime / third-party facts into plain structs, and renders the
// copy-to-clipboard report text. ImGui-free and headlessly testable, so the UI
// (SmatchetAboutUi.cpp) and the command surface (app.version) both consume the
// same values and cannot disagree.
// Deliberately does NOT include the generated SmatchetBuildInfo.h — that header
// is included by exactly one TU (AboutInfo.cpp). Keeping it out of every public
// header is what stops it leaking into the packaged Unreal plugin's flat include
// dir, where a copy would go stale against the packaged .lib.
// Also takes no AppController — the two facts it needs from one (app version,
// release repo) come in as strings, so the doctest rig can link this TU without
// dragging in AppController's cpr/SQLite closure (the same reason
// BugReportBody.cpp is split out of BugReportService.cpp).
// docs/plans/shipped/about-dialog-help-menu.md.

namespace smatchet {
namespace diagnostics {

/// One third-party dependency, as emitted by the CMake dep manifest.
struct AboutDep {
    std::string Name;
    std::string Version;
    std::string License;
    std::string Url;
};

/// Compile/CMake-time build provenance.
struct AboutBuildInfo {
    std::string Config;          ///< SMATCHET_BUILD_CONFIG ($<CONFIG>) — "unknown" if absent
    std::string CompilerId;      ///< "MSVC" / "Clang" / "GNU" / "unknown"
    std::string CompilerVersion; ///< e.g. "19.38.33145.0"
    std::string CMakeVersion;
    std::string DateTime;     ///< __DATE__ " " __TIME__ of the AboutInfo TU
    std::string ImGuiVersion; ///< IMGUI_VERSION, or "unknown" where imgui.h is off the path
    std::string TargetArch;   ///< compile-time target ("x86_64" / "arm64" / ...)
    std::string BuildTag;     ///< Unreal host build tag, or "standalone"
    /// SMATCHET_AGENT_DEBUG as this binary was compiled: true means
    /// LOG_AGENT_DEBUG / SMATCHET_AGENT_DEBUG_LOG emit NDJSON to
    /// <userData>/agent-debug/, false means they compile out (the macro degrades
    /// to LOG_DEBUG / ((void)0)). Externally the two states look identical — an
    /// absent agent-debug dir means "no instrumented call site fired" in one and
    /// "the define never reached the compiler" in the other, and only the build's
    /// DEFINES separated them. Reported per-binary, as seen by THIS TU: the define
    /// is target-level today (Source/Standalone/CMakeLists.txt), so it agrees with
    /// Logger.cpp's bridge; a future per-source-file define would break that.
    bool AgentDebug;

    AboutBuildInfo() : AgentDebug(false) {}
};

/// Git provenance from the generated header. All-"unknown" is a valid state (no
/// git, no repo, unborn HEAD) — never an error.
struct AboutGitInfo {
    std::string Sha;
    std::string ShaShort;
    std::string Branch;
    bool Dirty;
    bool Shallow;

    AboutGitInfo() : Dirty(false), Shallow(false) {}
};

/// Runtime host + install facts.
struct AboutRuntimeInfo {
    std::string Os;
    std::string Arch;       ///< compile-time build target (same as AboutBuildInfo::TargetArch)
    std::string NativeArch; ///< native silicon; empty when unresolved/unrecognised
    bool Emulated;
    bool EmulationResolved; ///< false on pre-1709 Windows + non-Windows — omit the field
    std::string Tracker;    ///< active backend, or "(none)"
    std::string UserDataDir;
    std::string ConfigPath;

    AboutRuntimeInfo() : Emulated(false), EmulationResolved(false) {}
};

struct AboutInfo {
    std::string AppName;
    std::string Version;
    std::string RepoUrl; ///< https URL built from AppController::GetGitHubReleaseRepo()
    AboutBuildInfo Build;
    AboutGitInfo Git;
    AboutRuntimeInfo Runtime;
    std::vector<AboutDep> Deps;
};

/// Parse the CMake-generated dep manifest: one `name|version|license|url` record
/// per line. Malformed lines are dropped with a LOG_WARN rather than throwing —
/// a bad credits box must never take the About dialog down.
std::vector<AboutDep> ParseDepManifest(const std::string& text);

/// Render the git provenance as one human line: `<sha> (<branch>) +dirty`.
/// Returns "unknown" — never a dangling "()" — when the SHA is unavailable.
std::string FormatAboutGitLine(const AboutGitInfo& git);

/// Assemble everything. `appVersion` / `githubRepo` come from
/// AppController::GetAppVersion() / GetGitHubReleaseRepo() ("owner/repo").
/// Reads ConfigManager (disk-backed config) — call once on dialog open and
/// cache, never per frame.
AboutInfo GatherAboutInfo(const std::string& appVersion, const std::string& githubRepo);

/// Render the clipboard / bug-thread report. Plain `\n` line endings, terminated
/// by exactly one `\n`. Caller is responsible for running it through
/// privacy::RedactLogText before it leaves the machine.
std::string BuildAboutReportText(const AboutInfo& info);

} // namespace diagnostics
} // namespace smatchet

#endif // SMATCHET_DIAGNOSTICS_ABOUT_INFO_H
