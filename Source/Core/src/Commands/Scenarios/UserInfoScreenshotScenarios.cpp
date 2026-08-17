// UserInfoScreenshotScenarios — the four bucket-C screenshot-diff goldens for the
// dockable User Info window, captured at the 2x2 matrix of framebuffer width
// (desktop / narrow) x cfg.VcsFeedLayout (unified / separate):
//   user-info-desktop-unified   user-info-desktop-separate
//   user-info-narrow-unified    user-info-narrow-separate
//
// All four share one scenario body (Commands/Scenarios/UserInfoScreenshotScenario.h)
// parametrised by the layout string + the fixed capture width — see that header
// for the full determinism rationale (empty-email p4 fast-fail, cleared GitHub
// config for an instant git fail, activity/groups compiled out under the headless
// spawn, no clock-dependent content in frame). The Ui-touching method bodies are
// defined HERE (not the header): a Commands/Scenarios header including a Ui header
// is an include-cycle layer back-edge, so the Ui includes + g_ui extern live in
// this .cpp (the sanctioned Scenario->Ui seam, like the sibling scenarios). Each
// factory below supplies the name + layout + width/height pair; the bash driver
// (scripts/dev/test-screenshot-diff.sh) diffs each capture against
// tests/golden/<name>.png at L_inf <= 4.

#include "Commands/Scenarios/UserInfoScreenshotScenario.h"

#include <memory>

#include "Commands/Scenarios/ScenarioCaptureQuiesce.h"
#include "Config/TrackerConfigSaveRepair.h" // persisted-field repair hook — see OnStart (#2047)
#include "SmatchetUiSession.h"

// g_ui — unconditional extern. Defined in SmatchetUI.cpp without a
// SMATCHET_WITH_LUA_AUTOMATION guard; the header-side extern in SmatchetUiSession.h
// is gated, so re-declare it here, matching the DockGapSentinelScenario /
// CodeSyntaxColoringScenario shim.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

void UserInfoScreenshotScenario::OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& outErr) {
    // warmupFrames is caller-tunable; the width/height are FIXED per scenario
    // (the bash driver passes one shared --screenshotPath to every scenario and
    // no per-scenario size, so the desktop-vs-narrow distinction must be baked
    // in here, not read from args). 16 warm-up frames is the driver's default
    // and is far more than the (synchronous) VCS fast-fail needs to settle.
    warmupFrames_ = IntArg(args, "warmupFrames", 16);
    if (warmupFrames_ < 2) {
        warmupFrames_ = 2;
    }
    screenshotPath_ = StringArg(args, "screenshotPath", std::string());
    if (screenshotPath_.empty()) {
        outErr = name_ + ": screenshotPath is required";
        return;
    }
    // Confine under <userData>/screenshots/ (the #1566 MCP/Lua arbitrary-file-write
    // class). A reject returns before ANY session/global state mutates below.
    if (!ConfineScenarioScreenshotPathInPlace(name_.c_str(), screenshotPath_, outErr)) {
        return;
    }

    // Force the framebuffer to the scenario's fixed capture size (drives the
    // docked bottom-panel content width across / under the narrow breakpoint).
    ScenarioCaptureSize size;
    size.Width = captureWidth_;
    size.Height = captureHeight_;
    RequestScenarioCaptureWindowResize(size);

    // Save the g_ui fields we mutate so a long-lived dev session re-using one
    // Smatchet process is left as we found it (ephemeral --spawn never persists).
    savedShowUserInfo_ = g_ui.showUserInfo;
    savedRequestPending_ = g_ui.userInfoRequestPending;
    savedRequestFocus_ = g_ui.requestUserInfoFocus;
    savedPaneId_ = g_ui.userInfoSourcePaneId;
    savedDisplayName_ = g_ui.userInfoDisplayName;
    savedEmail_ = g_ui.userInfoEmail;
    savedAccountId_ = g_ui.userInfoAccountId;
    savedVcsFeedLayout_ = g_ui.cfg.VcsFeedLayout;
    savedGitHubPat_ = g_ui.cfg.GitHubPat;
    savedGitCommitRepos_ = g_ui.cfg.GitCommitRepos;
    savedGitHubOwner_ = g_ui.cfg.GitHubOwner;
    savedGitHubRepo_ = g_ui.cfg.GitHubRepo;
    savedUiMode_ = g_ui.cfg.UiMode;
#if defined(SMATCHET_WITH_WHISPER)
    savedWhisperSetupCompleted_ = g_ui.cfg.WhisperSetupCompleted;
#endif

    // Seven of the fields saved above are PERSISTED config, not session state — GitHubPat most of
    // all. Their unwind cannot run inline in OnFinish (it would un-pin them before the captured
    // frame is drawn; see the comment there), so it is deferred by a frame — and a config save
    // enqueued inside that window, or a process that exits with the unwind still queued, used to
    // write the CLEARED PAT to the user's real config (#2047). Arm a repair at the config-save
    // chokepoint for the whole run instead of reasoning about how narrow the window is: every
    // outgoing snapshot carries the user's values for exactly these fields, so the destructive
    // write cannot be issued — ConfigManager::Save is the seam EVERY TrackerConfig write funnels
    // through, including the dozens of direct `ConfigManager::Save(g_ui.cfg)` calls across the UI.
    // Dropped by restoreState() — via OnCancel, or via the deferred unwind.
    configRepairToken_ = smatchet::config_repair::RegisterTrackerConfigRepair(
        [self = *this](TrackerConfig& cfg) { self.restorePersistedConfigFields(cfg); });

    applyPinnedState();
    // Stage the open request; DrawWindow adopts it on the next frame.
    g_ui.userInfoRequestPending = true;
    g_ui.showUserInfo = true;
    g_ui.requestUserInfoFocus = true;
}

void UserInfoScreenshotScenario::applyPinnedState() {
    // Deterministic identity: fixed display name + account id, EMPTY email so
    // the p4 feed fast-fails without a `p4 users` subprocess (see header).
    g_ui.userInfoSourcePaneId = "main";
    g_ui.userInfoDisplayName = "Ada Lovelace";
    g_ui.userInfoEmail.clear();
    g_ui.userInfoAccountId = "user-0001";
    // Clear every GitHub config leg so the git feed fast-fails with no HTTP.
    g_ui.cfg.GitHubPat.clear();
    g_ui.cfg.GitCommitRepos.clear();
    g_ui.cfg.GitHubOwner.clear();
    g_ui.cfg.GitHubRepo.clear();
    // Pin Desktop so a narrow (<=720px) framebuffer never flips into the mobile
    // shell, which would skip the docked User Info window entirely (see header).
    g_ui.cfg.UiMode = UiMode::Desktop;
    // Suppress the first-run whisper dictation-consent banner: under the isolated
    // empty config the consent is unanswered, so SmatchetWhisperSetupBanner pins a
    // full-width banner at the top of the frame (unrelated to the window under
    // test). Marking setup "completed" makes the banner return early — the golden
    // captures the User Info window against a clean chrome, not the consent prompt.
    // WhisperSetupCompleted only exists in the SMATCHET_WITH_WHISPER build (the
    // field + the banner are compiled out otherwise), so guard the access — this
    // TU compiles in every config (light / POSIX / Android / sanitizer). In a
    // whisper-OFF build the banner code is itself compiled out, so the capture is
    // already banner-free — the golden stays consistent across both builds.
#if defined(SMATCHET_WITH_WHISPER)
    g_ui.cfg.WhisperSetupCompleted = true;
#endif
    // The layout under test.
    g_ui.cfg.VcsFeedLayout = vcsFeedLayout_;
}

void UserInfoScreenshotScenario::OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) {
    // requestUserInfoFocus is consumed (cleared) each frame by SmatchetUI::Draw,
    // so re-arm it every warm-up frame to keep the window open + focused right
    // through to the captured frame (mirrors the sibling bucket-E open recipe).
    g_ui.requestUserInfoFocus = true;
    // Re-assert the identity + config pins every warm-up frame. They are
    // idempotent, and re-applying them keeps a late write to g_ui.cfg (a
    // first-launch config load landing after OnStart) from reaching the
    // captured frame as the first-run whisper-consent banner plus an empty
    // User Info body.
    applyPinnedState();
    // Determinism: the startup connectivity poll pushes timed "Syncing..." /
    // "Sync Warning" toasts whose fade animation is wall-clock-driven and so
    // differs frame-to-frame between two captures (the exact transient-state
    // hazard ThemeSwitchRoundtripScenario documents). The shared chokepoint
    // dismisses every live toast; the three ambient bucket-C scenarios call the
    // same helper (see ScenarioCaptureQuiesce.h). History is untouched.
    QuiesceCaptureFrame();
}

void UserInfoScreenshotScenario::OnCancel(IAppScenarioHost& /*app*/) { restoreState(); }

nlohmann::json UserInfoScreenshotScenario::OnFinish(IAppScenarioHost& /*app*/) {
    // Same capture-trigger pattern as the sibling screenshot scenarios — the
    // post-swap handler in Source/Standalone/main.cpp writes the PNG after the
    // next SwapBuffers.
    g_ui.requestScreenshotPath = screenshotPath_;
    g_ui.requestScreenshot = true;

    nlohmann::json out;
    out["scenario"] = name_;
    out["warmupFrames"] = warmupFrames_;
    out["windowWidth"] = captureWidth_;
    out["windowHeight"] = captureHeight_;
    out["vcsFeedLayout"] = vcsFeedLayout_;
    out["screenshotPath"] = screenshotPath_;
    out["captureRequested"] = true;

    // Restore on the NEXT frame, not here. OnFinish runs from the runner tick
    // inside SmatchetUI::drawPreWindowOverlays — ahead of every window draw — so an
    // inline restoreState() un-pins showUserInfo, the identity fields and
    // WhisperSetupCompleted BEFORE the frame this capture records is drawn, and the
    // PNG lands showing the first-run whisper-consent banner over an empty User Info
    // body. (Restoring inline only looked safe while a second Scenarios().Tick ran
    // post-render in the standalone bootstrap and absorbed OnFinish there; that
    // duplicate tick is gone.) The runner destroys this scenario via active_.reset()
    // the moment OnFinish returns, so the closure gets a by-value copy, not `this`.
    QueuePostCaptureRestore([self = *this]() mutable { self.restoreState(); });
    return out;
}

void UserInfoScreenshotScenario::restorePersistedConfigFields(TrackerConfig& cfg) const {
#if defined(SMATCHET_WITH_WHISPER)
    cfg.WhisperSetupCompleted = savedWhisperSetupCompleted_;
#endif
    cfg.UiMode = savedUiMode_;
    cfg.VcsFeedLayout = savedVcsFeedLayout_;
    cfg.GitHubPat = savedGitHubPat_;
    cfg.GitCommitRepos = savedGitCommitRepos_;
    cfg.GitHubOwner = savedGitHubOwner_;
    cfg.GitHubRepo = savedGitHubRepo_;
}

void UserInfoScreenshotScenario::restoreState() {
    restorePersistedConfigFields(g_ui.cfg);
    // The live config no longer carries the pins, so the save-chokepoint repair has nothing left to
    // protect — drop it before touching the session fields, so a save racing the rest of this
    // unwind is never repaired back to a value the user has since changed.
    smatchet::config_repair::UnregisterTrackerConfigRepair(configRepairToken_);
    configRepairToken_ = 0;
    g_ui.userInfoSourcePaneId = savedPaneId_;
    g_ui.userInfoDisplayName = savedDisplayName_;
    g_ui.userInfoEmail = savedEmail_;
    g_ui.userInfoAccountId = savedAccountId_;
    g_ui.userInfoRequestPending = savedRequestPending_;
    g_ui.requestUserInfoFocus = savedRequestFocus_;
    // Leave showUserInfo restored last so the close-edge cleanup in DrawWindow
    // runs against a valid app on the next frame (matches the bucket-E guard).
    g_ui.showUserInfo = savedShowUserInfo_;
    // Unwind the shared quiesce too — its update-check suppression is global
    // state, not per-scenario (see ScenarioCaptureQuiesce.h).
    RestoreCaptureQuiesce();
}

} // namespace cmd
} // namespace smatchet

namespace {

// Desktop capture size — the screenshot-diff driver's default framebuffer. The
// full-width bottom-panel content region sits comfortably above the 600px narrow
// breakpoint, so the wide (single-line) VCS rows render.
const int kDesktopWidth = 1920;
const int kDesktopHeight = 1009;

// Narrow capture size — a ~480px framebuffer drops the docked bottom-panel
// content region below kNarrowLayoutWidthPx (600), driving the stacked
// narrow-row layout. Height matches desktop so the two width variants differ in
// exactly one axis.
const int kNarrowWidth = 480;
const int kNarrowHeight = 1009;

} // namespace

std::unique_ptr<smatchet::cmd::IScenario> MakeUserInfoDesktopUnifiedScenario() {
    return std::make_unique<smatchet::cmd::UserInfoScreenshotScenario>("user-info-desktop-unified", "unified",
                                                                       kDesktopWidth, kDesktopHeight);
}

std::unique_ptr<smatchet::cmd::IScenario> MakeUserInfoDesktopSeparateScenario() {
    return std::make_unique<smatchet::cmd::UserInfoScreenshotScenario>("user-info-desktop-separate", "separate",
                                                                       kDesktopWidth, kDesktopHeight);
}

std::unique_ptr<smatchet::cmd::IScenario> MakeUserInfoNarrowUnifiedScenario() {
    return std::make_unique<smatchet::cmd::UserInfoScreenshotScenario>("user-info-narrow-unified", "unified",
                                                                       kNarrowWidth, kNarrowHeight);
}

std::unique_ptr<smatchet::cmd::IScenario> MakeUserInfoNarrowSeparateScenario() {
    return std::make_unique<smatchet::cmd::UserInfoScreenshotScenario>("user-info-narrow-separate", "separate",
                                                                       kNarrowWidth, kNarrowHeight);
}
