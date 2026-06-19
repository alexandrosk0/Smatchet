#ifndef SMATCHET_COMMANDS_SCENARIOS_UITESTOUTCOMEFORMAT_H
#define SMATCHET_COMMANDS_SCENARIOS_UITESTOUTCOMEFORMAT_H

// UiTestOutcomeFormat — PURE, engine-free formatter for the bucket-E ui_test.run
// outcome text the scenario emits to the --spawn child stdout (captured by the
// spawn-log) for diagnosability. This header is deliberately ImGui-FREE: it names no ImGui types (no
// imgui.h / imgui_te_engine.h pollution), so it is trivially linkable from the
// doctest rig (tests/Core/UiTestOutcomeFormat.test.cpp). The ImGui-dependent
// EXTRACTION (walking ImGuiTestEngine_GetTestList / Output.Status /
// ExtractLinesForVerboseLevels) stays in UiTestScenario.cpp, which builds a
// vector<UiTestOutcomeRow> and calls FormatUiTestOutcome(...) to produce the
// emitted string with a single CLI-stdout write.

#include <string>
#include <vector>

namespace smatchet {
namespace cmd {

// One failing ui_test, already extracted from the engine into POD form.
struct UiTestOutcomeRow {
    std::string category;
    std::string name;
    std::string errorLog;
};

// Build the multi-line outcome text emitted to the spawn-redirected child stdout:
//   [ui_test] run complete: filter='<filter>' tested=<n> passed=<n> failed=<n>[ (no tests queued/registered)]
//   [ui_test] KO: <cat>/<name>            (one per failure)
//   <errorLog excerpt>                    (when non-empty, per failure)
//   [ui_test] failure reported by counts but no Error-status test found
//                                          (defensive: tested>passed but no rows)
//   [ui_test] Tests Result: OK|Errors (<passed>/<tested> passed)
// `filter` empty -> rendered as '(all)'. `queued` false -> the run-complete line
// gets the " (no tests queued/registered)" suffix. The trailing newline is
// included so the caller emits the whole block with one write.
std::string FormatUiTestOutcome(int tested, int passed, const std::string& filter, bool queued,
                                const std::vector<UiTestOutcomeRow>& failures);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_UITESTOUTCOMEFORMAT_H
