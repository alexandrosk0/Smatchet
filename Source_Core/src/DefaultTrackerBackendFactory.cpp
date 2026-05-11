#include "DefaultTrackerBackendFactory.h"

#include "JiraClient.h"
#include "PlaneClient.h"
#include "StringUtil.h"

std::unique_ptr<ITrackerClient> DefaultTrackerBackendFactory::Create(const std::string& trackerType) {
    // Case-insensitive match against the two shipped backends. Anything we don't recognise
    // falls back to Jira so existing configs with stale / empty TrackerType values keep
    // booting (this matches the pre-factory behaviour where `Initialize` defaulted to
    // Jira via the `else` branch).
    const std::string lower = ToLowerAsciiCopy(trackerType);
    if (lower == "plane") {
        return std::make_unique<PlaneClient>();
    }
    return std::make_unique<JiraClient>();
}
