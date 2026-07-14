#ifndef SMATCHET_UI_SMATCHET_BACKEND_DISPLAY_H
#define SMATCHET_UI_SMATCHET_BACKEND_DISPLAY_H

// Shared backend-key -> product-name mapping for UI strings (P2-M18). The config /
// pane keys are lowercase type strings ("jira", "github"); user-facing surfaces must
// show the product name ("Jira", "GitHub") instead. One inline seam so the status
// bar, pane picker, comment toasts, and future surfaces can't drift apart again.

#include <cctype>
#include <cstddef>
#include <string>

/// "jira" -> "Jira", "github" -> "GitHub", "plane" -> "Plane", "linear" -> "Linear";
/// unknown keys get a first-letter capitalization; empty stays empty ("tracker" is the
/// caller's fallback wording decision).
inline std::string BackendDisplayName(const std::string& rawType) {
    std::string t = rawType.size() > 64u ? rawType.substr(0u, 64u) : rawType;
    std::string lower = t;
    for (std::size_t i = 0; i < lower.size(); ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[i])));
    }
    if (lower == "jira")
        return "Jira";
    if (lower == "plane")
        return "Plane";
    if (lower == "github")
        return "GitHub";
    if (lower == "linear")
        return "Linear";
    if (!t.empty()) {
        t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    }
    return t;
}

#endif // SMATCHET_UI_SMATCHET_BACKEND_DISPLAY_H
