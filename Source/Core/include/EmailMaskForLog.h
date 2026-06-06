#ifndef SMATCHET_EMAIL_MASK_FOR_LOG_H
#define SMATCHET_EMAIL_MASK_FOR_LOG_H

#include <string>

namespace smatchet {
namespace logging {
namespace pure {

/// Masks a user email for logging (PII): keeps only the first local-part char
/// plus the domain (which is enough for diagnosis), e.g. "alice@acme.com" ->
/// "a***@acme.com". An empty or malformed (no usable '@') value is fully
/// redacted so no personal data reaches the log. Issue #820.
///
/// Extracted verbatim from the anonymous namespace in SmatchetPreferencesUi.cpp
/// into this standalone pure header so the doctest rig can exercise it without
/// that Ui TU's ImGui / cpr / AppController dependency chain. `inline` so callers
/// include the header without a paired .cpp.
inline std::string MaskEmailForLog(const std::string& email) {
    if (email.empty()) {
        return "(none)";
    }
    const size_t at = email.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= email.size()) {
        return "(redacted)";
    }
    std::string out;
    out.push_back(email[0]);
    out += "***";
    out += email.substr(at); // "@domain"
    return out;
}

} // namespace pure
} // namespace logging
} // namespace smatchet

#endif // SMATCHET_EMAIL_MASK_FOR_LOG_H
