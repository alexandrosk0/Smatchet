#include "DefaultTrackerBackendFactory.h"

#include "ConfigManager.h"
#include "GitHubClient.h"
#include "ITrackerBackend.h"
#include "JiraClient.h"
#include "PlaneClient.h"
#include "StringUtil.h"

std::unique_ptr<ITrackerBackend> DefaultTrackerBackendFactory::Create(const std::string& trackerType) {
    // Case-insensitive match against the three shipped backends. Anything we don't recognise
    // falls back to Jira so existing configs with stale / empty TrackerType values keep
    // booting (this matches the pre-factory behaviour where `Initialize` defaulted to
    // Jira via the `else` branch).
    const std::string lower = ToLowerAsciiCopy(trackerType);
    if (lower == "plane") {
        return std::make_unique<PlaneClient>();
    }
    if (lower == "github") {
        // PR2 of docs/design/archive/github-tracker-backend.md — GitHub as third tracker.
        // Construct with current Config snapshot; runtime PAT/URL rotation
        // requires AppController to recreate the backend via the factory.
        const TrackerConfig cfg = ConfigManager::Load();
        return std::make_unique<GitHubClient>(cfg.GitHubBaseUrl, cfg.GitHubPat);
    }
    return std::make_unique<JiraClient>();
}
