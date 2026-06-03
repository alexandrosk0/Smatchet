#include "SmatchetNewIssueDraftUi_detail.h"

#include "StringUtil.h"

#include <string>

namespace SmatchetNewIssueDraft {
namespace detail {

bool OptionMatchesComboFilter(const TrackerFieldOption& opt, const std::string& filterLower) {
    if (filterLower.empty()) {
        return true;
    }
    const std::string optId = opt.Id.empty() ? opt.Value : opt.Id;
    return ToLowerAsciiCopy(opt.Value).find(filterLower) != std::string::npos ||
           ToLowerAsciiCopy(opt.SecondaryValue).find(filterLower) != std::string::npos ||
           ToLowerAsciiCopy(optId).find(filterLower) != std::string::npos;
}

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
