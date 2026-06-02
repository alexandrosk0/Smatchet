#include "JiraEditMetaPure.h"

#include "StringUtil.h"

#include <unordered_set>

namespace smatchet {
namespace jira {

bool JiraEditMetaFieldCanEdit(const std::string& fieldId, const nlohmann::json& meta) {
    bool canEdit = false;
    const bool hasOperationsArray = meta.is_object() && meta.contains("operations") && meta["operations"].is_array();
    if (hasOperationsArray) {
        for (const auto& op : meta["operations"]) {
            if (op.is_string()) {
                const std::string opLower = ToLowerAsciiCopy(op.get<std::string>());
                if (opLower == "set" || opLower == "add" || opLower == "remove") {
                    canEdit = true;
                    break;
                }
                continue;
            }
            if (!op.is_object()) {
                continue;
            }
            static const char* kOpNameKeys[] = {"operation", "name", "type"};
            for (const char* key : kOpNameKeys) {
                if (!op.contains(key) || !op[key].is_string()) {
                    continue;
                }
                const std::string opLower = ToLowerAsciiCopy(op[key].get<std::string>());
                if (opLower == "set" || opLower == "add" || opLower == "remove") {
                    canEdit = true;
                    break;
                }
            }
            if (canEdit) {
                break;
            }
        }
    }
    // Jira sometimes omits the operations list for nullable date or datetime fields (for
    // example an unset due date) while still listing the field in editmeta, so a missing or
    // non-array operations entry still counts as editable, whereas an explicit empty list does not.
    if (!canEdit && meta.is_object() && !hasOperationsArray) {
        static const std::unordered_set<std::string> kSchemaOnlyDateDenylist = {"created", "updated", "resolutiondate"};
        if (kSchemaOnlyDateDenylist.find(fieldId) == kSchemaOnlyDateDenylist.end() && meta.contains("schema") &&
            meta["schema"].is_object()) {
            const auto& schema = meta["schema"];
            if (schema.contains("type") && schema["type"].is_string()) {
                const std::string t = ToLowerAsciiCopy(schema["type"].get<std::string>());
                if (t == "date" || t == "datetime") {
                    canEdit = true;
                }
            }
        }
    }
    return canEdit;
}

} // namespace jira
} // namespace smatchet
