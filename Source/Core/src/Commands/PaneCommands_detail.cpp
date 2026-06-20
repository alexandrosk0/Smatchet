// PaneCommands_detail — pure, ImGui/AppController-free decision core for the pane-add
// command family (pane.new / pane.duplicate / pane.split). Split out of PaneCommands.cpp
// so bucket-A tests (tests/Core/PaneAddRequest.test.cpp) cover the arm-only-on-success
// credential gate (issue #1458) without linking the UI session / AppController TUs — the
// same seam pattern as SmatchetGridPaneWindows_detail.cpp. Depends only on
// ConfigManager::BackendCredentialsPresent + the PaneAddRequest struct.

#include "Commands/PaneCommands.h"

#include "Config/ConfigManager.h"
#include "GridPane.h"

#include <string>

namespace smatchet {
namespace cmd {
namespace detail {

PaneAddDecision DecidePaneAddRequest(const std::string& sourceId, bool acceptBackend,
                                     const std::string& backendArg, const std::string& viewArg,
                                     const TrackerConfig& cfg) {
    PaneAddDecision decision;
    decision.Request.sourceId = sourceId;
    if (acceptBackend && !backendArg.empty()) {
        if (!ConfigManager::BackendCredentialsPresent(cfg, backendArg)) {
            decision.Ok             = false;
            decision.FailureMessage = "Backend '" + backendArg + "' has no credentials configured.";
            decision.Request        = PaneAddRequest{};
            return decision;
        }
        decision.Request.targetBackendKey = backendArg;
        decision.Request.targetViewId     = viewArg;
    }
    return decision;
}

}  // namespace detail
}  // namespace cmd
}  // namespace smatchet
