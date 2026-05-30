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

#include <chrono>
#include <cstring>
#include <memory>
#include <string>

// NOTE: no <filesystem> here on purpose — sync disk I/O on the UI thread is a
// CRITICAL violation (Pillar 2). The screenshot capture + staging happens on the
// standalone capture path (Source/Standalone/main.cpp), which signals completion
// back via g_ui.bugReportShotReady; this TU never touches the filesystem.

namespace {

const std::size_t kDescBufCap = 64u * 1024u;
const double kShotCaptureTimeoutSec = 8.0;

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
    // WYSIWYG consent: if the user opened (and possibly edited) the egress preview,
    // that exact text is the issue body.
    if (d.bugReportPreviewSeeded && !d.bugReportPreviewBuf.empty()) {
        opts.BodyOverride = std::string(d.bugReportPreviewBuf.data());
    }

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
                d.bugReportPreviewBuf.clear();
                d.bugReportPreviewSeeded = false;
                d.bugReportPreviewUserEdited = false;
            }
        });
    });
}

} // namespace

void SmatchetBugReportUi_Draw(AppController& app, UiDrawSession& d) {
    // Per-frame: if a screenshot was requested, the standalone capture path sets
    // bugReportShotReady once the PNG is written (no UI-thread filesystem poll).
    // Launch the worker exactly once on the ready edge; time out if it never lands.
    if (d.bugReportInFlight && d.bugReportShotPending) {
        if (d.bugReportShotReady) {
            d.bugReportShotReady = false;
            d.bugReportShotPending = false;
            LaunchSubmit(app, d, d.bugReportStagedShotPath);
        } else if (d.bugReportShotArmed) {
            // The frame after Submit. The modal is suppressed THIS frame (see the early
            // return below), so the captured frame won't contain the modal. Request the
            // capture now; main.cpp writes the PNG this frame and sets bugReportShotReady.
            d.bugReportShotArmed = false;
            d.requestScreenshotPath = d.bugReportStagedShotPath;
            d.requestScreenshotCensor = (d.bugReportShotMode == 1);
            d.requestScreenshotCensorBlock = d.cfg.BugReportCensorBlock; // 0 = auto
            d.requestScreenshot = true;
            d.requestScreenshotBugReport = true;
            d.bugReportShotReady = false;
            d.bugReportShotDeadline = ImGui::GetTime() + kShotCaptureTimeoutSec;
        } else if (ImGui::GetTime() > d.bugReportShotDeadline) {
            // Capture never landed (minimized window, zero-size framebuffer, …) — free the modal.
            d.bugReportShotPending = false;
            d.bugReportInFlight = false;
            d.requestScreenshot = false;
            d.requestScreenshotBugReport = false;
            auto fail = std::make_shared<smatchet::diagnostics::SubmitResult>();
            fail->Error = "Screenshot capture timed out — try again, or submit without a screenshot.";
            d.bugReportResult = fail;
        }
    }

    if (!d.showBugReport) {
        return;
    }

    // While a screenshot capture is pending, do NOT draw the modal — that keeps it out
    // of the captured frame (the user wants the app as it looks without this dialog).
    if (d.bugReportShotPending) {
        d.bugReportOpenLatch = false;
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
        // NOTE: bugReportInclScreenshot / bugReportShotMode are NOT seeded here —
        // the opener owns them (hotkey + menu seed from config; the `bug.report`
        // command sets them from --screenshot/--censored). Re-seeding here would
        // clobber command-provided modal args.
        d.bugReportResult.reset();
        d.bugReportInFlight = false;
        d.bugReportShotPending = false;
        d.bugReportShotArmed = false;
        d.bugReportShotReady = false;
        d.bugReportPreviewDirty = true;
        d.bugReportPreviewSeeded = false;
        d.bugReportPreviewUserEdited = false;
        d.bugReportPreviewBuf.clear();
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
    if (target.Ok && target.UseRelay) {
        ImGui::TextDisabled("Destination: relay (%s)", target.RelayUrl.c_str());
    } else if (target.Ok) {
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
        ImGui::RadioButton("Censored (blur fine text)", &mode, 1);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Light pixelation: blurs fine print but keeps layout + larger text\n"
                              "legible for debugging. Not a privacy guarantee — use the editable\n"
                              "preview below to remove anything sensitive in the text.");
        }
        d.bugReportShotMode = mode;
        ImGui::Unindent();
    }
#endif

    // Egress preview = exactly what will be sent, and EDITABLE so the user can
    // remove anything they don't want to share. (Re)generated from the current
    // inputs only until the user starts editing it.
    if (ImGui::CollapsingHeader("Preview / edit what will be sent")) {
        if (d.bugReportPreviewDirty && !d.bugReportPreviewUserEdited) {
            smatchet::diagnostics::BugReportOptions opts;
            opts.UserDescription = DescriptionText(d);
            opts.IncludeScreenshot = d.bugReportInclScreenshot;
            const smatchet::diagnostics::ContextBundle bundle = smatchet::diagnostics::GatherContext(app, opts);
            const std::string shotNote =
                d.bugReportInclScreenshot ? "_(screenshot attached on submit)_" : std::string();
            const std::string text = smatchet::diagnostics::BuildMarkdownBody(opts, bundle, shotNote);
            // Editable copy with headroom (the user mostly trims, but allow some growth).
            const std::size_t cap = text.size() + 16384u;
            d.bugReportPreviewBuf.assign(cap, '\0');
            std::memcpy(d.bugReportPreviewBuf.data(), text.data(), text.size());
            d.bugReportPreviewSeeded = true;
            d.bugReportPreviewDirty = false;
        }
        if (d.bugReportPreviewBuf.empty()) {
            d.bugReportPreviewBuf.assign(16384u, '\0');
            d.bugReportPreviewSeeded = true;
        }
        ImGui::TextDisabled("This exact text is sent. Edit to remove anything you don't want to share.");
        if (ImGui::InputTextMultiline("##bugpreview", d.bugReportPreviewBuf.data(), d.bugReportPreviewBuf.size(),
                                      ImVec2(-1.0f, 220.0f))) {
            d.bugReportPreviewUserEdited = true;
        }
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
            // ARM the capture — don't request it on this frame (the modal is still drawn
            // this frame, so capturing now would include it). The handshake requests the
            // capture next frame, where the modal is suppressed → a clean screenshot.
            d.bugReportStagedShotPath =
                ConfigManager::GetUserDataDirectory() + "bug_reports/_pending_" + PendingShotStamp() + ".png";
            d.bugReportShotReady = false;
            d.bugReportShotPending = true;
            d.bugReportShotArmed = true;
            // Generous fallback deadline (the handshake resets it once the capture is requested).
            d.bugReportShotDeadline = ImGui::GetTime() + kShotCaptureTimeoutSec + 2.0;
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
