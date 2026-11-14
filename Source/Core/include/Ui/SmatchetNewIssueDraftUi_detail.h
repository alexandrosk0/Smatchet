#pragma once

// Pure, ImGui-free decision helpers extracted from RenderNewIssueDraftRow so the
// catalog-dropdown predicate logic is bucket-A testable and the draw body stays under the
// function-size caps. No ImGui, no global state.

#include "TrackerFieldSchema.h"

#include <string>

namespace SmatchetNewIssueDraft {
namespace detail {

/** True iff a non-id, non-project field should render as a catalog-option dropdown (issuetype, or
 *  a single-valued select/structured/status/sprint/user field that carries allowed options). The
 *  field pointer may be null (unknown field) — only the "issuetype" name forces the dropdown then. */
bool FieldUsesOptionDropdown(const std::string& fieldId, const TrackerField* field);

} // namespace detail
} // namespace SmatchetNewIssueDraft
