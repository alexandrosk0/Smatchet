#include "DefaultTrackerBackendFactory.h"

#include "ConfigManager.h"
#include "GitHubClient.h"
#include "ITrackerBackend.h"
#include "JiraClient.h"
#include "PlaneClient.h"
#include "StringUtil.h"

std::unique_ptr<ITrackerBackend> DefaultTrackerBackendFactory::Create(const std::string& trackerType,
                                                                      const TrackerConfig& cfg) {
    // Case-insensitive match against the three shipped backends. Anything we don't recognise
    // falls back to Jira so existing configs with stale / empty TrackerType values keep
    // booting (this matches the pre-factory behaviour where `Initialize` defaulted to
    // Jira via the `else` branch).
    const std::string lower = ToLowerAsciiCopy(trackerType);
    if (lower == "plane") {
        return std::make_unique<PlaneClient>();
    }
    if (lower == "github") {
        // PR2 of docs/plans/shipped/github-tracker-backend.md — GitHub as third tracker.
        // Built from the CALLER's live cfg, never a ConfigManager::Load() disk re-read —
        // the prefs "Save & Sync" path reaches here while the debounced config save is
        // still pending, so a disk read would latch a stale/empty PAT (issue #979).
        return std::make_unique<GitHubClient>(cfg.GitHubBaseUrl, cfg.GitHubPat);
    }
    return std::make_unique<JiraClient>();
}
