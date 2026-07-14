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
// spawn, no clock-dependent content in frame). Each factory below just supplies
// the name + layout + width/height pair; the bash driver
// (scripts/dev/test-screenshot-diff.sh) diffs each capture against
// tests/golden/<name>.png at L_inf <= 4.

#include "Commands/Scenarios/UserInfoScreenshotScenario.h"

#include <memory>

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
