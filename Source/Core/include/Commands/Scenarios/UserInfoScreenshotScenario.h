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

// NOTE: the Ui-layer includes (SmatchetUiSession.h + Ui/SmatchetToast.h) and the
// g_ui extern live in the .cpp, NOT this header. A Commands/Scenarios *header*
// that includes a Ui header is an include-cycle layer back-edge (the gate is
// header->header by design; a .cpp->Ui edge is the sanctioned Scenario seam the
// sibling screenshot scenarios use). The Ui-touching method bodies are therefore
// defined out-of-line in UserInfoScreenshotScenarios.cpp.

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

    // Ui-touching lifecycle — defined out-of-line in the .cpp (see header NOTE):
    // they read/write g_ui (Ui/SmatchetUiSession.h) + SmatchetToastManager
    // (Ui/SmatchetToast.h), which must not be included from this header.
    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& outErr) override;
    void OnFrame(IAppScenarioHost& app, int frameIndex) override;

    bool IsDone(int frameIndex) const override { return frameIndex >= warmupFrames_; }

    void OnCancel(IAppScenarioHost& app) override;

    nlohmann::json OnFinish(IAppScenarioHost& app) override;

  private:
    // Write the deterministic identity + config pins into g_ui. Called from
    // OnStart AND from every OnFrame: the pins are idempotent, and re-applying
    // them each frame makes the capture immune to anything that lands on
    // g_ui.cfg after OnStart (a late first-launch config load, a Preferences
    // default write). A capture that lost the pins renders the first-run
    // whisper-consent banner over the top of the frame and an empty User Info
    // body — the exact off-state one bucket-C golden was bootstrapped from.
    void applyPinnedState();

    void restoreState();

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
#if defined(SMATCHET_WITH_WHISPER)
    bool savedWhisperSetupCompleted_ = false;
#endif
};

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_USER_INFO_SCREENSHOT_SCENARIO_H
