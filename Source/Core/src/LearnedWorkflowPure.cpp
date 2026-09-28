#include "LearnedWorkflowPure.h"

#include "FieldOptionsJsonPure.h"

#include <string>
#include <vector>

namespace smatchet {
namespace workflow {

std::string EscapeKeyPart(const std::string& part) {
    std::string escaped;
    escaped.reserve(part.size());
    for (const char c : part) {
        if (c == '\\' || c == '|') {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

std::string BuildScopedKey(const std::string& scope, const std::string& key) {
    return EscapeKeyPart(scope) + "|" + EscapeKeyPart(key);
}

std::string BuildLearnedTransitionsKey(const std::string& projectKey, const std::string& issueTypeKey,
                                       const std::string& fromStatusKey) {
    if (projectKey.empty() || issueTypeKey.empty() || fromStatusKey.empty()) {
        return std::string();
    }
    return EscapeKeyPart(projectKey) + "|" + EscapeKeyPart(issueTypeKey) + "|" + EscapeKeyPart(fromStatusKey);
}

std::string SerializeTransitionTargets(const std::vector<TrackerFieldOption>& options) {
    return smatchet::fieldoptions::SerializeOptionIdNames(options);
}

bool ParseTransitionTargets(const std::string& json, std::vector<TrackerFieldOption>& out) {
    return smatchet::fieldoptions::ParseOptionIdNames(json, out);
}

} // namespace workflow
} // namespace smatchet
