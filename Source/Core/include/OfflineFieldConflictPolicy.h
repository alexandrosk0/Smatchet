#pragma once

// Scalar offline-replay conflict policy. Lives in its own header with zero banned
// includes (no SQLite, no cpr, no ImGui, no nlohmann) so the doctest rig under
// tests/ can exercise the decision without dragging the full LocalCacheManager /
// SQLite / cpr cascade. Sibling of OfflineQueueReplayPolicy.h.
//
// Scope (ADR-0016): scalar conflict detection is 2-way on DISPLAY values — `base`
// (the persisted pre-edit display string) vs `theirs` (the re-fetched display
// string). The queued payload ("mine") is in backend format (account / transition
// ids, ADF) and plays no part in the detection decision; it is shown in the resolve
// modal for the user's benefit only.

#include <cstddef>
#include <string>

namespace OfflineFieldConflictPolicy {

/// Index of the first non-whitespace char (or size()). ASCII whitespace only —
/// matches the trim semantics the rest of the offline queue uses.
inline std::size_t FirstNonWhitespaceIndex(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
    return i;
}

/// Index one past the last non-whitespace char (or 0 for an all-whitespace string).
inline std::size_t LastNonWhitespaceEnd(const std::string& s) {
    std::size_t end = s.size();
    while (end > 0) {
        const char c = s[end - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            --end;
        } else {
            break;
        }
    }
    return end;
}

/// Whitespace-trimmed copy (leading + trailing ASCII whitespace stripped).
inline std::string TrimWhitespace(const std::string& s) {
    const std::size_t b = FirstNonWhitespaceIndex(s);
    const std::size_t e = LastNonWhitespaceEnd(s);
    return (b < e) ? s.substr(b, e - b) : std::string();
}

/// True when the server's current display value (`theirs`) differs from the
/// pre-edit display value the user looked at (`base`), after a whitespace trim on
/// both sides — i.e. the server moved since the user made the edit, so an offline
/// replay would silently overwrite a concurrent change and we must ask first.
///
/// An empty `base` (no reference captured — legacy rows, fields introduced before
/// capture) returns false: we can't ask about a divergence we have no reference
/// for. Documented residue (ADR-0016), not a silent overwrite of a *detected*
/// change.
inline bool ServerMovedFromBase(const std::string& base, const std::string& theirs) {
    const std::string baseTrimmed = TrimWhitespace(base);
    if (baseTrimmed.empty()) {
        return false;
    }
    return baseTrimmed != TrimWhitespace(theirs);
}

} // namespace OfflineFieldConflictPolicy
