#ifndef SMATCHET_COMMANDS_PARAM_BOUNDS_PURE_H
#define SMATCHET_COMMANDS_PARAM_BOUNDS_PURE_H

// Pure, header-only numeric-range + max-length check for a coerced command
// argument. Extracted so it is bucket-A testable without linking the registry
// (mirrors the CommandSourceTrust pure-predicate pattern). CommandRegistry's
// ValidateAndResolveArgs calls this after type coercion; an out-of-range or
// over-length value yields ValidationError. Bounds are opt-in per ParamSpec
// (null MinInt/MaxInt + MaxLen==0 == unbounded), so an un-backfilled command is
// unaffected. Inspecting the coerced value needs the full nlohmann type (not
// Command.h's json_fwd), so this lives in its own header.

#include <string>

#include <nlohmann/json.hpp>

#include "Commands/Command.h"

namespace smatchet {
namespace cmd {

/// True when `coerced` satisfies `p`'s optional bounds. On failure returns false,
/// fills `whyNot` with a human-readable reason, and sets one structured key on
/// `detail` ({min|max|maxLen: limit}) for the error envelope. `detail` must be a
/// JSON object on entry.
inline bool ParamValueWithinBounds(const ParamSpec& p, const nlohmann::json& coerced, std::string& whyNot,
                                   nlohmann::json& detail) {
    if (p.Type == ParamType::Int && coerced.is_number_integer()) {
        const long long n = coerced.get<long long>();
        if (p.MinInt && n < *p.MinInt) {
            whyNot = "is below the minimum of " + std::to_string(*p.MinInt);
            detail["min"] = *p.MinInt;
            return false;
        }
        if (p.MaxInt && n > *p.MaxInt) {
            whyNot = "exceeds the maximum of " + std::to_string(*p.MaxInt);
            detail["max"] = *p.MaxInt;
            return false;
        }
    } else if (p.Type == ParamType::Number && coerced.is_number()) {
        const double n = coerced.get<double>();
        if (p.MinInt && n < static_cast<double>(*p.MinInt)) {
            whyNot = "is below the minimum of " + std::to_string(*p.MinInt);
            detail["min"] = *p.MinInt;
            return false;
        }
        if (p.MaxInt && n > static_cast<double>(*p.MaxInt)) {
            whyNot = "exceeds the maximum of " + std::to_string(*p.MaxInt);
            detail["max"] = *p.MaxInt;
            return false;
        }
    }
    if (p.MaxLen > 0 && coerced.is_string() && coerced.get<std::string>().size() > p.MaxLen) {
        whyNot = "exceeds the maximum length of " + std::to_string(p.MaxLen) + " bytes";
        detail["maxLen"] = static_cast<long long>(p.MaxLen);
        return false;
    }
    return true;
}

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_PARAM_BOUNDS_PURE_H
