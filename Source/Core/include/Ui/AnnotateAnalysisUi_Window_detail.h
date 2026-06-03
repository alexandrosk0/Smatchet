#pragma once

#include <string>

// Pure (ImGui-free, AppController-free) display-classification helpers extracted from
// AnnotateAnalysisUi::DrawContent so the per-row annotate cells are unit-testable
// (bucket-A: tests/Core/AnnotateRowDisplayPure.test.cpp).
namespace AnnotateUiPure {

// Row user is "actionable" (assign / Jira lookup) only when loaded and a real user.
bool RowUserIsActionable(bool pending, const std::string& user);

// User cell renders dimmed while loading or with no real user ("" or "-").
bool RowUserCellDisabled(bool pending, const std::string& user);

// CL cell renders dimmed while loading or with an empty changelist.
bool RowClCellDisabled(bool pending, const std::string& changelist);

// An annotated line offers "Assign..." only for a real user (not "", "-", "...").
bool LineCanAssign(const std::string& user);

// Cell text: "..." while loading, "-" when empty, else the value verbatim.
std::string PendingOrDash(bool pending, const std::string& value);

} // namespace AnnotateUiPure
