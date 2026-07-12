#include "SemanticVersionPure.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstddef>

namespace smatchet {
namespace version {

SemanticVersion ParseSemanticVersion(const std::string& raw) {
    SemanticVersion out;
    std::string s = raw;
    if (!s.empty() && s.front() == 'v') {
        s.erase(s.begin());
    }
    const std::size_t dash = s.find('-');
    if (dash != std::string::npos) {
        s.resize(dash);
    }

    std::array<int, 3> parts{{0, 0, 0}};
    std::size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        const std::size_t dot = s.find('.', start);
        const std::string token = (dot == std::string::npos) ? s.substr(start) : s.substr(start, dot - start);
        if (token.empty() ||
            !std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return out;
        }
        // CPP_CODE_AUDIT.md #17: std::atoi on an unbounded digit run is UB past INT_MAX
        // (e.g. a GitHub release tag like "v99999999999.0.0"); std::stoll + range-check
        // mirrors CallstackParser::ParseLineNumberInRange's guard for the same class of
        // untrusted-digit-run input.
        try {
            const long long value = std::stoll(token);
            if (value < 0 || value > static_cast<long long>(INT_MAX)) {
                return out;
            }
            parts[static_cast<std::size_t>(i)] = static_cast<int>(value);
        } catch (...) { // catch-all-ok: stoll on untrusted release-tag digits (e.g. > LLONG_MAX)
            return out;
        }
        if (dot == std::string::npos) {
            if (i != 2) {
                return out;
            }
            start = s.size();
        } else {
            start = dot + 1;
        }
    }
    if (start < s.size()) {
        return out;
    }

    out.Major = parts[0];
    out.Minor = parts[1];
    out.Patch = parts[2];
    out.Valid = true;
    return out;
}

int CompareSemanticVersion(const SemanticVersion& a, const SemanticVersion& b) {
    if (a.Major != b.Major) {
        return a.Major < b.Major ? -1 : 1;
    }
    if (a.Minor != b.Minor) {
        return a.Minor < b.Minor ? -1 : 1;
    }
    if (a.Patch != b.Patch) {
        return a.Patch < b.Patch ? -1 : 1;
    }
    return 0;
}

} // namespace version
} // namespace smatchet
