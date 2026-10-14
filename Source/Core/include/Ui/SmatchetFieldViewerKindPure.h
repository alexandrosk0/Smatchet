#pragma once

#include "TicketFieldEditorDescriptionPure.h"

#include <string>

/// Which grid fields open a read-only viewer window when their cell is clicked, and which content
/// provider each one uses. Pure, so the dispatch is unit-tested without ImGui. Adding a viewer field
/// = one enumerator here, one content class, one case in `OpenFieldViewerForField`.
namespace SmatchetFieldViewer {

enum class FieldViewerKind : unsigned char { None, Comments, ActivityLog };

inline FieldViewerKind FieldViewerKindForFieldId(const std::string& fieldId) {
    if (fieldId == "comments") {
        return FieldViewerKind::Comments;
    }
    if (IsActivityLogFieldId(fieldId)) {
        return FieldViewerKind::ActivityLog;
    }
    return FieldViewerKind::None;
}

inline bool HasFieldViewer(const std::string& fieldId) {
    return FieldViewerKindForFieldId(fieldId) != FieldViewerKind::None;
}

} // namespace SmatchetFieldViewer
