// "Log a Bug" modal. See SmatchetBugReportUi.h. Dual-target: no GL/GLFW here;
// the screenshot toggle + capture is standalone-only (hidden on DX12).
// docs/plans/shipped/log-a-bug-github.md Slice 4.

#include "SmatchetBugReportUi.h"

#include "Commands/IAppThreading.h"
#include "Interfaces/IAppMeta.h"
#include "ConfigManager.h"
#include "Diagnostics/BugReportService.h"
#include "SmatchetHelpMarker.h"
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

#ifndef SMATCHET_EMBEDDED_IN_UNREAL
// Only used by the screenshot-attach path below, which is itself
// #ifndef SMATCHET_EMBEDDED_IN_UNREAL — guard the def too, else the DX12
// target compiles it out of every call site and trips -Wunused-function -Werror.
std::string PendingShotStamp() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms);
}
#endif

// Launch the worker submit for the given screenshot path (may be empty).
void LaunchSubmit(IAppThreading& app, IAppMeta& meta, UiDrawSession& d, const std::string& shotPath) {
    smatchet::diagnostics::BugReportOptions opts;
    opts.UserDescription = DescriptionText(d);
    opts.IncludeScreenshot = !shotPath.empty();
    opts.Censored = !shotPath.empty(); // every attached screenshot is font-redacted
    opts.ScreenshotAbsPath = shotPath;
    opts.DumpAbsPath = d.bugReportCrashDumpPath; // crash mode: upload the minidump as a Release asset
    // WYSIWYG consent: if the user opened (and possibly edited) the egress preview,
    // that exact text is the issue body.
    if (d.bugReportPreviewSeeded && !d.bugReportPreviewBuf.empty()) {
        opts.BodyOverride = std::string(d.bugReportPreviewBuf.data());
    }

    app.LaunchBackgroundTask([&app, &meta, &d, opts]() {
        const smatchet::diagnostics::SubmitResult r = smatchet::diagnostics::SubmitBugReport(meta, opts);
        auto shared = std::make_shared<smatchet::diagnostics::SubmitResult>(r);
        app.PostToMainThread([&d, shared]() {
            d.bugReportResult = shared;
            d.bugReportInFlight = false;
            if (shared->Ok) {
                SmatchetToastManager::Instance().Push(
                    SmatchetLocalization::T("toast.bug_report", "Bug report"),
                    SmatchetLocalization::Format("bugreport.filed", "Filed %s", shared->IssueKey.c_str()),
                    ToastType::Success);
                d.showBugReport = false; // close on success; failure keeps the modal open with a banner
                d.bugReportDescBuf.clear();
                d.bugReportPreviewBuf.clear();
                d.bugReportPreviewSeeded = false;
                d.bugReportPreviewUserEdited = false;
            }
        });
    });
}

// Per-frame screenshot handshake: advance the capture state machine before the modal
// draws. Standalone capture path sets bugReportShotReady once the PNG lands; launch the
// submit worker on the ready edge, request the capture on the armed frame, time out if it
// never lands. Lifts the branch-heavy state machine out of the draw body.
void AdvanceShotHandshake(IAppThreading& app, IAppMeta& meta, UiDrawSession& d) {
    if (!(d.bugReportInFlight && d.bugReportShotPending)) {
        return;
    }
    if (d.bugReportShotReady) {
        d.bugReportShotReady = false;
        d.bugReportShotPending = false;
        LaunchSubmit(app, meta, d, d.bugReportStagedShotPath);
    } else if (d.bugReportShotArmed) {
        // The frame after Submit. The modal is suppressed THIS frame (an early exit in the
        // draw body skips it), so the captured frame won't contain the modal. Request the
        // capture now; main.cpp writes the PNG this frame and sets bugReportShotReady.
        d.bugReportShotArmed = false;
        d.requestScreenshotPath = d.bugReportStagedShotPath;
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

// First-frame-open seed: lazily size the description buffer, seed crash-mode prefill, and
// clear stale result/preview state. Runs only on the open-latch frame.
void SeedOnOpen(UiDrawSession& d) {
    if (d.bugReportDescBuf.size() < kDescBufCap) {
        d.bugReportDescBuf.assign(kDescBufCap, '\0');
        if (d.bugReportCrashMode) {
            // Prefer the assembled crash context — reason, breadcrumb, and log
            // tail — falling back to a plain prompt.
            const std::string seed =
                d.bugReportCrashContext.empty()
                    ? std::string("Smatchet closed unexpectedly. What were you doing?")
                    : ("Smatchet closed unexpectedly. What were you doing?\n\n" + d.bugReportCrashContext);
            std::strncpy(d.bugReportDescBuf.data(), seed.c_str(), kDescBufCap - 1);
        }
    }
    // NOTE: bugReportInclScreenshot is NOT seeded here — the opener owns it
    // (hotkey + menu seed from config; the bug.report command sets it from
    // --screenshot). Re-seeding here would clobber command-provided modal args.
    d.bugReportResult.reset();
    d.bugReportInFlight = false;
    d.bugReportShotPending = false;
    d.bugReportShotArmed = false;
    d.bugReportShotReady = false;
    d.bugReportPreviewDirty = true;
    d.bugReportPreviewSeeded = false;
    d.bugReportPreviewUserEdited = false;
    d.bugReportPreviewGenerating = false;
    d.bugReportPreviewBuf.clear();
}

// Read-only destination indicator — resolve from the in-memory config copy (no disk I/O).
void DrawDestination(UiDrawSession& d) {
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
}

// Egress preview shows exactly what will be sent and stays EDITABLE, so the user can remove
// anything they don't want to share. Regenerated from current inputs on a worker until edit.
void DrawEgressPreview(IAppThreading& app, IAppMeta& meta, UiDrawSession& d) {
    if (!ImGui::CollapsingHeader("Preview / edit what will be sent")) {
        return;
    }
    // (Re)generate on a WORKER — GatherContext reads the audit file from disk, which
    // must never block the UI thread (Pillar 2). The worker posts the text back.
    if (d.bugReportPreviewDirty && !d.bugReportPreviewUserEdited && !d.bugReportPreviewGenerating) {
        d.bugReportPreviewGenerating = true;
        smatchet::diagnostics::BugReportOptions opts;
        opts.UserDescription = DescriptionText(d);
        opts.IncludeScreenshot = d.bugReportInclScreenshot;
        const bool inclShot = d.bugReportInclScreenshot;
        app.LaunchBackgroundTask([&app, &meta, &d, opts, inclShot]() {
            const smatchet::diagnostics::ContextBundle bundle = smatchet::diagnostics::GatherContext(meta, opts);
            const std::string shotNote = inclShot ? "_(screenshot attached on submit)_" : std::string();
            auto text = std::make_shared<std::string>(smatchet::diagnostics::BuildMarkdownBody(opts, bundle, shotNote));
            app.PostToMainThread([&d, text]() {
                // Don't clobber if the user started editing while we generated.
                if (!d.bugReportPreviewUserEdited) {
                    const std::size_t cap = text->size() + 16384u;
                    d.bugReportPreviewBuf.assign(cap, '\0');
                    std::memcpy(d.bugReportPreviewBuf.data(), text->data(), text->size());
                    d.bugReportPreviewSeeded = true;
                    d.bugReportPreviewDirty = false;
                }
                d.bugReportPreviewGenerating = false;
            });
        });
    }
    if (d.bugReportPreviewGenerating && d.bugReportPreviewBuf.empty()) {
        ImGui::TextDisabled("Generating preview…");
    } else {
        if (d.bugReportPreviewBuf.empty()) {
            d.bugReportPreviewBuf.assign(16384u, '\0'); // defensive only — not "seeded"
        }
        ImGui::TextDisabled("This exact text is sent. Edit to remove anything you don't want to share.");
        if (ImGui::InputTextMultiline("##bugpreview", d.bugReportPreviewBuf.data(), d.bugReportPreviewBuf.size(),
                                      ImVec2(-1.0f, 220.0f))) {
            d.bugReportPreviewUserEdited = true;
        }
    }
}

// Apply a Submit action: flip in-flight, then either arm the redacted screenshot capture
// (standalone, when requested) or launch the submit worker directly. Mirrors the original
// inline Submit branch byte-for-byte.
void OnSubmit(IAppThreading& app, IAppMeta& meta, UiDrawSession& d) {
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
        // Set on THIS (Submit) frame so main.cpp swaps to the redaction font before
        // NewFrame on the NEXT (captured) frame — the whole UI renders as blocks.
        d.requestRedactFontThisFrame = true;
        // Generous fallback deadline (the handshake resets it once the capture is requested).
        d.bugReportShotDeadline = ImGui::GetTime() + kShotCaptureTimeoutSec + 2.0;
    } else {
        d.bugReportShotPending = false;
        LaunchSubmit(app, meta, d, std::string());
    }
#else
    d.bugReportShotPending = false;
    LaunchSubmit(app, meta, d, std::string());
#endif
}

// Error banner + Submit/Cancel buttons + key dispatch (Ctrl+Enter submit, Esc cancel).
void DrawActions(IAppThreading& app, IAppMeta& meta, UiDrawSession& d) {
    // Error banner (failure keeps the modal open).
    if (d.bugReportResult && !d.bugReportResult->Ok && !d.bugReportResult->Error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Submit failed: %s", d.bugReportResult->Error.c_str());
    }
    ImGui::Separator();

    const char* envTok = std::getenv("SMATCHET_BUGREPORT_GITHUB_TOKEN");
    const smatchet::diagnostics::ResolvedBugTarget target =
        smatchet::diagnostics::ResolveBugReportTarget(d.cfg, envTok ? std::string(envTok) : std::string());
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
        OnSubmit(app, meta, d);
    }
    if (cancelClicked && !d.bugReportInFlight) {
        d.showBugReport = false;
    }
    // Esc cancels when not in flight.
    if (!d.bugReportInFlight && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        d.showBugReport = false;
    }
}

} // namespace

void SmatchetBugReportUi_Draw(IAppThreading& app, IAppMeta& meta, UiDrawSession& d) {
    AdvanceShotHandshake(app, meta, d);

    if (!d.showBugReport) {
        return;
    }

    // While a screenshot capture is pending, do NOT draw the modal — that keeps it out
    // of the captured frame (the user wants the app as it looks without this dialog).
    if (d.bugReportShotPending) {
        d.bugReportOpenLatch = false;
        return;
    }

    if (d.bugReportOpenLatch) {
        SeedOnOpen(d);
    }

    ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Report a Bug", &d.showBugReport)) {
        ImGui::End();
        return;
    }

    DrawDestination(d);

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
    // Screenshot toggle — standalone only (DX12 has no GL capture path). The shot is
    // always text-redacted: every glyph renders as a block on the capture frame, so
    // layout, colour, and icons stay sharp while text is unreadable.
    if (ImGui::Checkbox("Attach screenshot (text redacted)", &d.bugReportInclScreenshot)) {
        d.bugReportPreviewDirty = true;
    }
    ImGui::SameLine();
    SmatchetHelpMarker::Render("bugreport.screenshot_redaction.help",
                               "The screenshot is captured with all text rendered as blocks (██) — sharp, "
                               "layout-preserving, no readable text. Icons + colour stay intact.");
#endif

    DrawEgressPreview(app, meta, d);
    DrawActions(app, meta, d);

    d.bugReportOpenLatch = false;
    ImGui::End();
}

#undef ImGui
