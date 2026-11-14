#include "SmatchetNewIssueDraftUi_detail.h"

#include <string>

namespace SmatchetNewIssueDraft {
namespace detail {

bool FieldUsesOptionDropdown(const std::string& fieldId, const TrackerField* field) {
    if (fieldId == "issuetype") {
        return true;
    }
    return field != nullptr && !field->IsArray && !field->AllowedValueOptions.empty() &&
           (field->Family == TrackerFieldFamily::SelectSingle ||
            field->Family == TrackerFieldFamily::StructuredSingle || field->Family == TrackerFieldFamily::IssueType ||
            field->Family == TrackerFieldFamily::Status || field->Family == TrackerFieldFamily::Sprint ||
            field->IsUserType);
}

} // namespace detail
} // namespace SmatchetNewIssueDraft
