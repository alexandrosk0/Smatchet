#include "EditMetaScopePure.h"

#include "LearnedWorkflowPure.h"
#include "Tracker/ProjectResolver.h"

#include <utility>

namespace smatchet {
namespace editmeta {

std::string ProjectOfIssue(const std::string& issueId) { return ExtractIssueKeyPrefix(issueId); }

std::string BuildScopeStoreKey(const std::string& project, const std::string& issueTypeKey) {
    if (issueTypeKey.empty()) {
        return std::string();
    }
    return workflow::BuildScopedKey(project, issueTypeKey);
}

bool SplitScopeStoreKey(const std::string& storeKey, std::string& project, std::string& issueTypeKey) {
    std::string parts[2];
    std::size_t part = 0;
    for (std::size_t i = 0; i < storeKey.size(); ++i) {
        const char c = storeKey[i];
        if (c == '\\') {
            if (i + 1 >= storeKey.size()) {
                return false; // a trailing escape: not a key BuildScopeStoreKey wrote
            }
            parts[part] += storeKey[++i];
        } else if (c == '|') {
            if (part == 1) {
                return false; // a third part
            }
            part = 1;
        } else {
            parts[part] += c;
        }
    }
    if (part != 1 || parts[1].empty()) {
        return false;
    }
    project = std::move(parts[0]);
    issueTypeKey = std::move(parts[1]);
    return true;
}

bool IsRepresentativeEditPermissions(const std::unordered_map<std::string, bool>& fieldCanEdit) {
    for (const auto& entry : fieldCanEdit) {
        if (entry.second) {
            return true;
        }
    }
    return false;
}

} // namespace editmeta
} // namespace smatchet
