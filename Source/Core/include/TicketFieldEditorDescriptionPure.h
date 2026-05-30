#pragma once

#include <string>

// Returns true for field IDs that carry multi-paragraph markdown content
// (Jira/Plane description, GitHub body). These fields get a rich markdown
// tooltip regardless of whether the cell text clips horizontally.
inline bool IsDescriptionLikeFieldId(const std::string& fieldId) {
    if (fieldId.empty()) {
        return false;
    }
    return fieldId.find("description") != std::string::npos || fieldId.find("Description") != std::string::npos ||
           fieldId == "body" || fieldId == "Body";
}
