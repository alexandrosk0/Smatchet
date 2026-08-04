// SmatchetAboutUi — the "About Smatchet" modal. Renders the diagnostics::AboutInfo
// snapshot (version / build / git / runtime / third-party) and offers the three
// actions a user actually needs from an About box: open the repo, check for
// updates, and copy the whole thing for a bug thread.
// This TU is render-only: every fact comes from AboutInfo.cpp, which is the sole
// includer of the generated SmatchetBuildInfo.h. Nothing here touches CMake
// codegen, and nothing here re-derives a value.
// docs/plans/shipped/about-dialog-help-menu.md Slice 4.

#include "SmatchetAboutUi.h"

// The three things this modal needs live on AppController and nowhere narrower:
// OpenUrl (which carries the scheme allowlist and the host-callback indirection
// that keep a link safe under Unreal), GetAppVersion, and GetGitHubReleaseRepo.
// DrawAboutModal takes an AppController reference by signature, exactly as every
// sibling modal TU already does, so there is no lighter dependency to swap in.
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=calls OpenUrl/GetAppVersion; owner=alexk; revisit=2026-12-31)
#include "AppController.h"
#include "Diagnostics/AboutInfo.h"
#include "Privacy/TextRedaction.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization wrapper.
#define ImGui SmatchetLocalizedImGui

#include <memory>
#include <string>

// Declared in SmatchetUI_MainMenu.cpp / SmatchetUI.cpp; reused verbatim so the
// About dialog's update button behaves exactly like the Help-menu entry.
namespace smatchet {
namespace ui_detail {
void StartAppUpdateCheck(UiDrawSession& d, AppController& app, bool manual);
} // namespace ui_detail
} // namespace smatchet

namespace {

using smatchet::diagnostics::AboutDep;
using smatchet::diagnostics::AboutInfo;

const char* const kAboutPopupId = "About Smatchet";

/// Per-frame state shared by the section helpers. Mirrors the MainMenuDrawCtx
/// pattern (docs/guides/imgui-draw-pattern.md) so each helper stays short and
/// none of them re-fetches the snapshot.
struct AboutDrawCtx {
    AppController& app;
    UiDrawSession& d;
    const AboutInfo& info;
};

/// Two-column key/value table. Keys are localized; values NEVER are — a SHA, a
/// compiler version or a filesystem path must stay copy-pasteable verbatim.
bool BeginFactTable(const char* id) {
    if (!::ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        return false;
    }
    ::ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 96.0f);
    ::ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void FactRow(const char* label, const std::string& value) {
    ::ImGui::TableNextRow();
    ::ImGui::TableSetColumnIndex(0);
    // "%s" as the format string, never `label` itself — a non-literal format
    // string is both a -Wformat-security warning and a varargs hazard.
    ::ImGui::TextDisabled("%s", SmatchetLocalization::TranslateSource(label));
    ::ImGui::TableSetColumnIndex(1);
    ::ImGui::TextUnformatted(value.empty() ? "unknown" : value.c_str());
}

/// Same as FactRow but wraps the value — for filesystem paths and URLs, which
/// would otherwise stretch the modal off-screen.
void FactRowWrapped(const char* label, const std::string& value) {
    ::ImGui::TableNextRow();
    ::ImGui::TableSetColumnIndex(0);
    ::ImGui::TextDisabled("%s", SmatchetLocalization::TranslateSource(label));
    ::ImGui::TableSetColumnIndex(1);
    ::ImGui::TextWrapped("%s", value.empty() ? "unknown" : value.c_str());
}

void DrawAboutIdentity(const AboutDrawCtx& ctx) {
    ImGui::Text("%s", ctx.info.AppName.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ctx.info.Version.c_str());
    if (!ctx.info.RepoUrl.empty()) {
        ImGui::TextDisabled("%s", ctx.info.RepoUrl.c_str());
    }
}

void DrawAboutBuild(const AboutDrawCtx& ctx) {
    ImGui::SeparatorText("Build");
    if (!BeginFactTable("AboutBuildFacts")) {
        return;
    }
    const smatchet::diagnostics::AboutBuildInfo& b = ctx.info.Build;
    FactRow("configuration", b.Config);
    FactRow("target", b.BuildTag);
    FactRow("built", b.DateTime);
    FactRow("compiler", b.CompilerId + " " + b.CompilerVersion);
    FactRow("cmake", b.CMakeVersion);
    FactRow("imgui", b.ImGuiVersion);
    FactRow("architecture", b.TargetArch);
    ::ImGui::EndTable();
}

void DrawAboutGit(const AboutDrawCtx& ctx) {
    ImGui::SeparatorText("Source");
    if (!BeginFactTable("AboutGitFacts")) {
        return;
    }
    FactRow("commit", smatchet::diagnostics::FormatAboutGitLine(ctx.info.Git));
    // Full SHA on its own row: the one-line form above is abbreviated, and a bug
    // thread wants the unambiguous 40-char value.
    if (!ctx.info.Git.Sha.empty() && ctx.info.Git.Sha != ctx.info.Git.ShaShort) {
        FactRowWrapped("full sha", ctx.info.Git.Sha);
    }
    ::ImGui::EndTable();
}

void DrawAboutRuntime(const AboutDrawCtx& ctx) {
    ImGui::SeparatorText("Runtime");
    if (!BeginFactTable("AboutRuntimeFacts")) {
        return;
    }
    const smatchet::diagnostics::AboutRuntimeInfo& r = ctx.info.Runtime;
    FactRow("os", r.Os);
    if (r.EmulationResolved && !r.NativeArch.empty()) {
        // Omitted rather than guessed when the probe never resolved (pre-1709
        // Windows, non-Windows) — matches BuildAboutReportText.
        FactRow("host cpu", r.NativeArch + (r.Emulated ? " (emulated)" : ""));
    }
    FactRow("tracker", r.Tracker);
    FactRowWrapped("data dir", r.UserDataDir);
    FactRowWrapped("config", r.ConfigPath);
    ::ImGui::EndTable();
}

void DrawAboutCredits(const AboutDrawCtx& ctx) {
    if (ctx.info.Deps.empty()) {
        return;
    }
    ImGui::SeparatorText("Third-party");
    // Fixed height + border: the list is generated from the CMake dependency
    // manifest, so its length changes with the build's feature flags.
    ::ImGui::BeginChild("AboutCredits", ImVec2(0.0f, 132.0f), ImGuiChildFlags_Borders);
    for (std::size_t i = 0; i < ctx.info.Deps.size(); ++i) {
        const AboutDep& dep = ctx.info.Deps[i];
        ImGui::Text("%s %s", dep.Name.c_str(), dep.Version.c_str());
        if (!dep.License.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", dep.License.c_str());
        }
    }
    ::ImGui::EndChild();
}

void DrawAboutActions(const AboutDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;

    ::ImGui::BeginDisabled(ctx.info.RepoUrl.empty());
    if (ImGui::Button("Open on GitHub")) {
        // OpenUrl carries the scheme allowlist + the host-callback indirection
        // that keeps this safe under Unreal.
        ctx.app.OpenUrl(ctx.info.RepoUrl);
    }
    ::ImGui::EndDisabled();

    ImGui::SameLine();
    ::ImGui::BeginDisabled(d.appUpdateCheckInFlight);
    if (ImGui::Button("Check for Updates")) {
        smatchet::ui_detail::StartAppUpdateCheck(d, ctx.app, true);
    }
    ::ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Copy to Clipboard")) {
        // Same egress scrubber the bug-report path uses. It matches by shape
        // (emails, tokens, URL userinfo) — not usernames — so it will not touch
        // the filesystem paths; it is here so a tracker URL or address that ever
        // reaches this report cannot be pasted into a public thread. Git SHAs are
        // deliberately preserved by RedactLogLine.
        const std::string report =
            smatchet::privacy::RedactLogText(smatchet::diagnostics::BuildAboutReportText(ctx.info));
        ::ImGui::SetClipboardText(report.c_str());
        SmatchetToastManager::Instance().Push("About", "Build info copied to clipboard.", ToastType::Success, 2500);
    }

    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        d.showAbout = false;
        ::ImGui::CloseCurrentPopup();
    }
}

} // namespace

void DrawAboutModal(AppController& app, UiDrawSession& d) {
    if (!d.showAbout) {
        d.aboutInfo.reset();
        return;
    }

    if (d.aboutOpenLatch || !d.aboutInfo) {
        d.aboutOpenLatch = false;
        // GatherAboutInfo reads the disk-backed config — snapshot once per open
        // and cache it, never per frame (Pillar 2).
        d.aboutInfo = std::make_shared<AboutInfo>(
            smatchet::diagnostics::GatherAboutInfo(app.GetAppVersion(), app.GetGitHubReleaseRepo()));
        ImGui::OpenPopup(kAboutPopupId);
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal(kAboutPopupId, &d.showAbout, ImGuiWindowFlags_NoSavedSettings)) {
        // Dismissed via Escape or the title-bar close button — drop the flag and
        // the snapshot so the next open re-reads config (and so the menu item is
        // not re-opening a popup that ImGui has already closed).
        d.showAbout = false;
        d.aboutInfo.reset();
        return;
    }

    const AboutDrawCtx ctx{app, d, *d.aboutInfo};
    DrawAboutIdentity(ctx);
    DrawAboutBuild(ctx);
    DrawAboutGit(ctx);
    DrawAboutRuntime(ctx);
    DrawAboutCredits(ctx);
    ImGui::Separator();
    DrawAboutActions(ctx);

    ::ImGui::EndPopup();
}

#undef ImGui
