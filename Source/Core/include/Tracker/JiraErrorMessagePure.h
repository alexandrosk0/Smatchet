#pragma once

#include <string>

// Pure helper (no HTTP, no ImGui) extracting a human-readable message from a Jira REST
// error response body — the Jira-side counterpart of ExtractLinearErrorMessage
// (LinearClientHelpers.h). Unit-tested in tests/Core/JiraErrorMessagePure.test.cpp.

namespace smatchet {
namespace jira {

/// Best-effort human-readable error message from a Jira REST error response body.
/// Jira reports failures as {"errorMessages":["..."],"errors":{"field":"..."}}; the
/// result joins every errorMessages[] string plus every errors{} value ("field: msg"),
/// length-capped, prefixed with "HTTP <status>: ". Falls back to plain "HTTP <status>"
/// when the body is empty, fails the bounded parse (depth bombs are discarded, never
/// parsed recursively — bare-json-parse-untrusted), or carries no usable error fields.
/// The RAW body is never returned — callers previously spliced up to 1200 chars of
/// arbitrary response text (HTML error pages, JSON dumps) into user-facing toasts.
std::string ExtractJiraErrorMessage(int httpStatus, const std::string& body);

} // namespace jira
} // namespace smatchet
