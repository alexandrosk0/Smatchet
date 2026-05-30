// "Log a Bug" modal. See SmatchetBugReportUi.h. Dual-target: no GL/GLFW here;
// the screenshot toggle + capture is standalone-only (hidden on DX12).
// docs/plans/active/log-a-bug-github.md Slice 4.

#include "SmatchetBugReportUi.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Diagnostics/BugReportService.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization wrapper.
#define ImGui SmatchetLocalizedImGui

#include <ghc/filesystem.hpp>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>

namespace fs = ghc::filesystem;

namespace {

const std::size_t kDescBufCap = 64u * 1024u;

std::string DescriptionText(const UiDrawSession& d) {
    if (d.bugReportDescBuf.empty()) {
        return std::string();
    }
    // Buffer is NUL-terminated by InputTextMultiline.
    return std::string(d.bugReportDescBuf.data());
}

std::string PendingShotStamp() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms);
}

// Launch the worker submit for the given screenshot path (may be empty).
void LaunchSubmit(AppController& app, UiDrawSession& d, const std::string& shotPath) {
    smatchet::diagnostics::BugReportOptions opts;
    opts.UserDescription = DescriptionText(d);
    opts.IncludeScreenshot = !shotPath.empty();
    opts.Censored = (d.bugReportShotMode == 1);
    opts.ScreenshotAbsPath = shotPath;

    app.LaunchBackgroundTask([&app, &d, opts]() {
        const smatchet::diagnostics::SubmitResult r = smatchet::diagnostics::SubmitBugReport(app, opts);
        auto shared = std::make_shared<smatchet::diagnostics::SubmitResult>(r);
        app.mainThreadDispatcher.PostToMainThread([&d, shared]() {
            d.bugReportResult = shared;
            d.bugReportInFlight = false;
            if (shared->Ok) {
                SmatchetToastManager::Instance().Push("Bug report", "Filed " + shared->IssueKey, ToastType::Success);
                d.showBugReport = false; // close on success; failure keeps the modal open with a banner
                d.bugReportDescBuf.clear();
                d.bugReportPreviewText.clear();
            }
        });
    });
}

} // namespace

void SmatchetBugReportUi_Draw(AppController& app, UiDrawSession& d) {
    // Per-frame: if a screenshot was requested, wait for the capture file to land
    // (standalone writes it post-swap), then launch the worker exactly once.
    if (d.bugReportInFlight && d.bugReportShotPending) {
        std::error_code ec;
        if (!d.bugReportStagedShotPath.empty() && fs::exists(fs::path(d.bugReportStagedShotPath), ec) && !ec) {
            d.bugReportShotPending = false;
            LaunchSubmit(app, d, d.bugReportStagedShotPath);
        }
    }

    if (!d.showBugReport) {
        return;
    }

    // First-frame open: lazily size the buffer, seed crash-mode prefill, clear stale result.
    if (d.bugReportOpenLatch) {
        if (d.bugReportDescBuf.size() < kDescBufCap) {
            d.bugReportDescBuf.assign(kDescBufCap, '\0');
            if (d.bugReportCrashMode) {
                const char* seed = "Smatchet closed unexpectedly. What were you doing?";
                std::strncpy(d.bugReportDescBuf.data(), seed, kDescBufCap - 1);
            }
        }
        d.bugReportInclScreenshot = d.cfg.BugReportScreenshotDefault;
        d.bugReportResult.reset();
        d.bugReportInFlight = false;
        d.bugReportShotPending = false;
        d.bugReportPreviewDirty = true;
    }

    ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Report a Bug", &d.showBugReport)) {
        ImGui::End();
        return;
    }

    // Destination indicator (read-only) — resolve from the in-memory config copy (no disk I/O).
    const char* envTok = std::getenv("SMATCHET_BUGREPORT_GITHUB_TOKEN");
    const smatchet::diagnostics::ResolvedBugTarget target =
        smatchet::diagnostics::ResolveBugReportTarget(d.cfg, envTok ? std::string(envTok) : std::string());
    if (target.Ok) {
        ImGui::TextDisabled("Destination: %s/%s", target.Owner.c_str(), target.Repo.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.2f, 1.0f), "Not configured: %s", target.Error.c_str());
    }
    ImGui::Separator();

    // Description.
    if (d.bugReportDescBuf.size() < kDescBufCap) {
        d.bugReportDescBuf.assign(kDescBufCap, '\0');
    }
    ImGui::TextUnformatted("Describe the problem:");
    if (d.bugReportOpenLatch) {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::InputTextMultiline("##bugdesc", d.bugReportDescBuf.data(), d.bugReportDescBuf.size(),
                                  ImVec2(-1.0f, 160.0f))) {
        d.bugReportPreviewDirty = true;
    }

#ifndef SMATCHET_EMBEDDED_IN_UNREAL
    // Screenshot toggle — standalone only (DX12 has no GL capture path).
    if (ImGui::Checkbox("Attach screenshot", &d.bugReportInclScreenshot)) {
        d.bugReportPreviewDirty = true;
    }
    if (d.bugReportInclScreenshot) {
        ImGui::Indent();
        int mode = d.bugReportShotMode;
        ImGui::RadioButton("Full", &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Censored (no readable text)", &mode, 1);
        d.bugReportShotMode = mode;
        ImGui::Unindent();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The screenshot is uploaded as a repo asset and embedded inline.\n"
                              "GitHub's drag-drop image store is not accessible to API tokens.");
        }
    }
#endif

    // Egress preview (consent surface). Rebuild only when dirty.
    if (ImGui::CollapsingHeader("Preview what will be sent")) {
        if (d.bugReportPreviewDirty) {
            smatchet::diagnostics::BugReportOptions opts;
            opts.UserDescription = DescriptionText(d);
            opts.IncludeScreenshot = d.bugReportInclScreenshot;
            const smatchet::diagnostics::ContextBundle bundle = smatchet::diagnostics::GatherContext(app, opts);
            const std::string shotNote =
                d.bugReportInclScreenshot ? "_(screenshot attached on submit)_" : std::string();
            d.bugReportPreviewText = smatchet::diagnostics::BuildMarkdownBody(opts, bundle, shotNote);
            d.bugReportPreviewDirty = false;
        }
        ImGui::InputTextMultiline("##bugpreview", const_cast<char*>(d.bugReportPreviewText.c_str()),
                                  d.bugReportPreviewText.size() + 1, ImVec2(-1.0f, 180.0f),
                                  ImGuiInputTextFlags_ReadOnly);
    }

    // Error banner (failure keeps the modal open).
    if (d.bugReportResult && !d.bugReportResult->Ok && !d.bugReportResult->Error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Submit failed: %s", d.bugReportResult->Error.c_str());
    }

    ImGui::Separator();

    const std::string desc = DescriptionText(d);
    const bool canSubmit = target.Ok && !desc.empty() && !d.bugReportInFlight;

    const bool ctrlEnter = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    const bool submitKey = ctrlEnter && ImGui::IsKeyPressed(ImGuiKey_Enter, false);

    if (!canSubmit) {
        ImGui::BeginDisabled();
    }
    const bool submitClicked = ImGui::Button(d.bugReportInFlight ? "Submitting…" : "Submit");
    if (!canSubmit) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const bool cancelClicked = ImGui::Button("Cancel");

    if ((submitClicked || (submitKey && canSubmit))) {
        d.bugReportInFlight = true;
        d.bugReportResult.reset();
#ifndef SMATCHET_EMBEDDED_IN_UNREAL
        if (d.bugReportInclScreenshot) {
            // Request a capture; the per-frame handshake above launches the worker
            // once the PNG lands.
            const std::string path =
                ConfigManager::GetUserDataDirectory() + "bug_reports/_pending_" + PendingShotStamp() + ".png";
            std::error_code ec;
            fs::create_directories(fs::path(ConfigManager::GetUserDataDirectory() + "bug_reports"), ec);
            d.bugReportStagedShotPath = path;
            d.requestScreenshotPath = path;
            d.requestScreenshotCensor = (d.bugReportShotMode == 1);
            d.requestScreenshot = true;
            d.bugReportShotPending = true;
        } else {
            d.bugReportShotPending = false;
            LaunchSubmit(app, d, std::string());
        }
#else
        d.bugReportShotPending = false;
        LaunchSubmit(app, d, std::string());
#endif
    }
    if (cancelClicked && !d.bugReportInFlight) {
        d.showBugReport = false;
    }
    // Esc cancels when not in flight.
    if (!d.bugReportInFlight && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        d.showBugReport = false;
    }

    d.bugReportOpenLatch = false;
    ImGui::End();
}

#undef ImGui
