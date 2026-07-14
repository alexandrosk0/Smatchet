#ifndef SMATCHET_COMMANDS_SCENARIOS_USER_INFO_SCREENSHOT_SCENARIO_H
#define SMATCHET_COMMANDS_SCENARIOS_USER_INFO_SCREENSHOT_SCENARIO_H

// Shared driver for the four bucket-C User Info screenshot scenarios
// (user-info-{desktop,narrow}-{unified,separate}). Each of the four TUs is a
// thin instantiation of UserInfoScreenshotScenario with a fixed layout string
// ("unified" / "separate") and a fixed capture width (desktop vs the narrow
// breakpoint), so the identity-seed + config-clear + open + capture logic lives
// in ONE place (DRY Engineering Pillar 5) rather than copy-pasted four times.
//
// DETERMINISM (the golden must re-capture bytewise-stable within L∞<=4 on the
// same machine, regardless of the dev's real p4/git config or network):
//   * Empty userInfoEmail. launchVcsFetch's worker calls P4UserForEmail(email)
//     which returns EARLY for an empty email (no `p4 users` subprocess), then
//     Vcs::P4UserFromEmail("") is also empty, so the p4 feed resolves INSTANTLY
//     to the fixed string "No Perforce user (email unknown)." — no subprocess,
//     no variable latency, no machine-specific p4 data in frame.
//   * GitHubPat / GitCommitRepos / GitHubOwner / GitHubRepo cleared on g_ui.cfg.
//     GitHubCommitsForUser then fails INSTANTLY ("no git repos configured" /
//     "GitHub PAT not configured") with no HTTP round-trip — again fixed text,
//     no network content in frame.
//   * The fixture/headless spawn has no activity-capable backend, so
//     supportsActivity_ is false and the Activity + Groups sections are compiled
//     out at runtime (their `if (!supportsActivity_) return;` guards). The only
//     async source is the VCS feed, and both of its legs now fast-fail
//     synchronously — so by the time the warm-up frames elapse the section has
//     long since reached its stable resolved state ("(no submissions in
//     window)" + the two fixed error lines). No Loading-vs-resolved race.
//   * No clock-dependent content reaches the frame: the resolved feed is empty,
//     so no per-row ISO dates render; the identity block is fixed strings.
//
// LAYOUT (desktop vs narrow): User Info docks into the full-width kBottomPanel,
// so its content width tracks the framebuffer width. A wide framebuffer yields
// GetContentRegionAvail().x > kNarrowLayoutWidthPx (600) -> wide rows; a narrow
// (~480px) framebuffer drops it below 600 -> the stacked narrow-row path. Each
// scenario forces its framebuffer width via RequestScenarioCaptureWindowResize.
//
// cfg.UiMode is PINNED to Desktop for the whole capture. In the default Auto
// mode a framebuffer <= 720px logical width flips SmatchetUI::Draw into the
// fullscreen Mobile shell (SmatchetMobileShellUi), whose early-return SKIPS the
// entire docked-window path -> the User Info window is never drawn at the narrow
// width. Pinning Desktop makes resolveEffectiveUiMode ignore the width so the
// docked window renders its OWN narrow-branch layout at 480px, which is the
// responsive behaviour under test (a narrow docked pane on desktop, not the
// separate mobile shell).

#include <nlohmann/json.hpp>
#include <string>

#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/ScenarioArgs.h"
#include "Commands/Scenarios/ScenarioCaptureSizing.h"
#include "Commands/Scenarios/ScenarioScreenshotPath.h"
#include "SmatchetUiModeIds.h"
#include "SmatchetUiSession.h"
#include "Ui/SmatchetToast.h"

// g_ui — unconditional extern. Defined in SmatchetUI.cpp without a
// SMATCHET_WITH_LUA_AUTOMATION guard; the header-side extern in
// SmatchetUiSession.h is gated, so scenarios that need the singleton re-declare
// it, matching the DockGapSentinelScenario / CodeSyntaxColoringScenario shim.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

// One shared scenario body parametrised by the requested VcsFeedLayout + the
// fixed capture width. The four bucket-C User Info goldens differ ONLY in those
// two values, so the four Make*Scenario() factories just construct this with the
// right pair (UserInfoScreenshotScenarios.cpp).
class UserInfoScreenshotScenario : public IScenario {
  public:
    UserInfoScreenshotScenario(std::string name, std::string vcsFeedLayout, int captureWidth, int captureHeight)
        : name_(std::move(name)), vcsFeedLayout_(std::move(vcsFeedLayout)), captureWidth_(captureWidth),
          captureHeight_(captureHeight) {}

    std::string Name() const override { return name_; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& outErr) override {
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
        savedWhisperSetupCompleted_ = g_ui.cfg.WhisperSetupCompleted;

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
        g_ui.cfg.WhisperSetupCompleted = true;
        // The layout under test.
        g_ui.cfg.VcsFeedLayout = vcsFeedLayout_;
        // Stage the open request; DrawWindow adopts it on the next frame.
        g_ui.userInfoRequestPending = true;
        g_ui.showUserInfo = true;
        g_ui.requestUserInfoFocus = true;
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {
        // requestUserInfoFocus is consumed (cleared) each frame by SmatchetUI::Draw,
        // so re-arm it every warm-up frame to keep the window open + focused right
        // through to the captured frame (mirrors the sibling bucket-E open recipe).
        g_ui.requestUserInfoFocus = true;
        // Determinism: the startup connectivity poll pushes timed "Syncing..." /
        // "Sync Warning" toasts whose fade animation is wall-clock-driven and so
        // differs frame-to-frame between two captures (the exact transient-state
        // hazard ThemeSwitchRoundtripScenario documents). Dismiss every live toast
        // each warm-up frame so the captured frame carries none — the toast draw in
        // drawGlobalOverlays runs AFTER this scenario tick within SmatchetUI::Draw,
        // so a same-frame clear keeps them off the capture. History is untouched.
        SmatchetToastManager::Instance().DismissAllLive();
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= warmupFrames_; }

    void OnCancel(IAppScenarioHost& /*app*/) override { restoreState(); }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
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

        // Restore config AFTER staging the capture: showUserInfo stays true through
        // this frame so the window is still drawn when the post-swap capture fires.
        // The VcsFeedLayout / GitHub / identity fields no longer affect the already-
        // rendered frame, so restoring them here is safe and leaves the session clean.
        restoreState();
        return out;
    }

  private:
    void restoreState() {
        g_ui.cfg.WhisperSetupCompleted = savedWhisperSetupCompleted_;
        g_ui.cfg.UiMode = savedUiMode_;
        g_ui.cfg.VcsFeedLayout = savedVcsFeedLayout_;
        g_ui.cfg.GitHubPat = savedGitHubPat_;
        g_ui.cfg.GitCommitRepos = savedGitCommitRepos_;
        g_ui.cfg.GitHubOwner = savedGitHubOwner_;
        g_ui.cfg.GitHubRepo = savedGitHubRepo_;
        g_ui.userInfoSourcePaneId = savedPaneId_;
        g_ui.userInfoDisplayName = savedDisplayName_;
        g_ui.userInfoEmail = savedEmail_;
        g_ui.userInfoAccountId = savedAccountId_;
        g_ui.userInfoRequestPending = savedRequestPending_;
        g_ui.requestUserInfoFocus = savedRequestFocus_;
        // Leave showUserInfo restored last so the close-edge cleanup in DrawWindow
        // runs against a valid app on the next frame (matches the bucket-E guard).
        g_ui.showUserInfo = savedShowUserInfo_;
    }

    std::string name_;
    std::string vcsFeedLayout_;
    int captureWidth_ = 1920;
    int captureHeight_ = 1009;
    int warmupFrames_ = 16;
    std::string screenshotPath_;

    bool savedShowUserInfo_ = false;
    bool savedRequestPending_ = false;
    bool savedRequestFocus_ = false;
    std::string savedPaneId_;
    std::string savedDisplayName_;
    std::string savedEmail_;
    std::string savedAccountId_;
    std::string savedVcsFeedLayout_;
    std::string savedGitHubPat_;
    std::string savedGitCommitRepos_;
    std::string savedGitHubOwner_;
    std::string savedGitHubRepo_;
    UiMode savedUiMode_ = UiMode::Auto;
    bool savedWhisperSetupCompleted_ = false;
};

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_USER_INFO_SCREENSHOT_SCENARIO_H
