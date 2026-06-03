#pragma once

// Pure, ImGui-free decision helpers extracted from RenderNewIssueDraftRow so the
// draft-field option-filter + catalog-dropdown predicate logic is bucket-A testable and the
// draw body stays under the function-size caps. No ImGui, no global state.

#include "TrackerFieldSchema.h"

#include <string>

namespace SmatchetNewIssueDraft {
namespace detail {

/** True iff a catalog option matches the (already lowercased) combo search filter. An empty
 *  filter matches everything; otherwise the value, secondary value, or option-id substring is
 *  tested case-insensitively. Mirrors the in-combo filter used by the draft dropdown. */
bool OptionMatchesComboFilter(const TrackerFieldOption& opt, const std::string& filterLower);

/** True iff a non-id, non-project field should render as a catalog-option dropdown (issuetype, or
 *  a single-valued select/structured/status/sprint/user field that carries allowed options). The
 *  field pointer may be null (unknown field) — only the "issuetype" name forces the dropdown then. */
bool FieldUsesOptionDropdown(const std::string& fieldId, const TrackerField* field);

} // namespace detail
} // namespace SmatchetNewIssueDraft
