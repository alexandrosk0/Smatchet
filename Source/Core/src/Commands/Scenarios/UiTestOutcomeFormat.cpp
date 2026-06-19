// UiTestOutcomeFormat — pure, engine-free implementation of the bucket-E
// ui_test.run outcome text. See header for rationale (ImGui-free => testable +
// no header pollution). The ImGui-dependent extraction lives in
// UiTestScenario.cpp; this TU only formats already-extracted POD rows.

#include "Commands/Scenarios/UiTestOutcomeFormat.h"

#include <algorithm>
#include <cstdio>

namespace smatchet {
namespace cmd {

std::string FormatUiTestOutcome(int tested, int passed, const std::string& filter, bool queued,
                                const std::vector<UiTestOutcomeRow>& failures) {
    std::string out;
    out.reserve(256);

    char line[512];
    const char* filterText = filter.empty() ? "(all)" : filter.c_str();
    const int failed = (std::max)(0, tested - passed);

    std::snprintf(line, sizeof(line), "[ui_test] run complete: filter='%s' tested=%d passed=%d failed=%d%s\n",
                  filterText, tested, passed, failed, queued ? "" : " (no tests queued/registered)");
    out += line;

    for (const UiTestOutcomeRow& row : failures) {
        std::snprintf(line, sizeof(line), "[ui_test] KO: %s/%s\n", row.category.c_str(), row.name.c_str());
        out += line;
        if (!row.errorLog.empty()) {
            out += row.errorLog;
            out += '\n';
        }
    }

    if (failures.empty() && tested > passed) {
        // Defensive: counts say a failure happened but no Error-status test row
        // was extracted (e.g. queued-but-undrained). Surface it rather than
        // leave the log silent. Mirrors the engine-walk branch in the scenario.
        out += "[ui_test] failure reported by counts but no Error-status test found\n";
    }

    std::snprintf(line, sizeof(line), "[ui_test] Tests Result: %s (%d/%d passed)\n", (tested == passed) ? "OK" : "Errors",
                  passed, tested);
    out += line;

    return out;
}

} // namespace cmd
} // namespace smatchet
