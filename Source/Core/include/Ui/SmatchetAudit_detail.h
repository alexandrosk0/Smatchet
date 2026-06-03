#pragma once

// Pure, ImGui-free decision helpers extracted from SmatchetUI::drawAuditWindow so the
// filter / pagination logic is bucket-A testable and the draw body stays under the
// function-size branch cap. No ImGui, no session state — referentially transparent.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace SmatchetAudit {
namespace detail {

/** Stringify a JSON field by key: strings verbatim, bools as "true"/"false", integers via
 *  to_string, anything else via dump(). Missing key or non-object yields empty. */
std::string JsonStringValue(const nlohmann::json& j, const char* key);

/** Case-insensitive substring test on already-lowered inputs. Empty needle always matches. */
bool ContainsNoCasePreLower(const std::string& haystackLower, const std::string& needleLower);

/** True iff an action string passes the Action dropdown filter (0=All, 1=Creates, 2=Updates/
 *  Transitions/FieldEdits, 3=Comments, 4=Attachments, 5=Offline). */
bool ActionMatchesFilter(const std::string& action, int filter);

/** True iff a success flag passes the Result dropdown filter (0=All, 1=Success, 2=Failure). */
bool ResultMatchesFilter(bool success, int filter);

/** Clamp the rows-per-page input into the supported [25, 1000] range. */
int ClampRowsPerPage(int value);

/** Filter pass: walk events in display order (newestFirst reverses) and keep indices whose
 *  action+result pass the dropdown filters AND whose parallel pre-lowered search text contains
 *  the lowered query. Indices past searchLower's end are skipped (cache mid-rebuild). */
std::vector<std::size_t> CollectFilteredAuditIndices(const std::vector<nlohmann::json>& events,
                                                     const std::vector<std::string>& searchLower, int actionFilter,
                                                     int resultFilter, const std::string& queryLower, bool newestFirst);

/** Ceil-divide the filtered count by rows-per-page, clamped to at least one page. */
int ComputeAuditPageCount(std::size_t filteredCount, int rowsPerPage);

} // namespace detail
} // namespace SmatchetAudit
