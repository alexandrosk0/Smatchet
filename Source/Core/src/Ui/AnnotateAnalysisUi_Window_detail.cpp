#include "AnnotateAnalysisUi_Window_detail.h"

namespace AnnotateUiPure {

bool RowUserIsActionable(bool pending, const std::string& user) {
    return !pending && !user.empty() && user != "..." && user != "-";
}

bool RowUserCellDisabled(bool pending, const std::string& user) { return pending || user.empty() || user == "-"; }

bool RowClCellDisabled(bool pending, const std::string& changelist) { return pending || changelist.empty(); }

bool LineCanAssign(const std::string& user) { return !user.empty() && user != "-" && user != "..."; }

std::string PendingOrDash(bool pending, const std::string& value) {
    if (pending) {
        return std::string("...");
    }
    return value.empty() ? std::string("-") : value;
}

} // namespace AnnotateUiPure
