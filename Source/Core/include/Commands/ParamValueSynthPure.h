#ifndef SMATCHET_COMMANDS_PARAM_VALUE_SYNTH_PURE_H
#define SMATCHET_COMMANDS_PARAM_VALUE_SYNTH_PURE_H

// Pure, header-only synthesis of a *minimal valid* argument value for one
// ParamSpec (and helpers over a Command's Params). "Valid" means: survives
// CommandRegistry::ValidateAndResolveArgs — prefer the declared default, then the
// first enum value, then a type-appropriate literal inside the declared bounds.
//
// Extracted from CommandContractSweepScenario so the contract-sweep scenario AND
// the nightly command-registry monkey (tests/monkey/) share one synthesis rule
// instead of each carrying a private copy. Inspecting/producing the value needs
// the full nlohmann type (not Command.h's json_fwd), so this lives in its own
// header alongside ParamBoundsPure.h and follows the same pure-predicate pattern.
//
// Behaviour is byte-for-byte identical to the former anonymous-namespace copies in
// CommandContractSweepScenario.cpp — those TUs now include this header. Functions
// are `inline` so multiple TUs may include the header without an ODR clash.

#include <string>

#include <nlohmann/json.hpp>

#include "Commands/Command.h"

namespace smatchet {
namespace cmd {

/// Minimal value that survives ValidateAndResolveArgs for one ParamSpec: prefer the
/// declared default, then the first enum value, then a type-appropriate literal inside
/// the declared bounds. Used to carry a command PAST validation so a downstream guard
/// (the confirm gate) or the handler body is the thing under test.
inline nlohmann::json SynthValidValue(const ParamSpec& p) {
    if (p.Default && !p.Default->is_null()) {
        return *p.Default;
    }
    if (!p.Enum.empty()) {
        return p.Enum.front();
    }
    switch (p.Type) {
    case ParamType::Int: {
        long long v = 1;
        if (p.MinInt) {
            v = *p.MinInt;
        } else if (p.MaxInt && *p.MaxInt < v) {
            v = *p.MaxInt;
        }
        return v;
    }
    case ParamType::Number: {
        // Bounds-aware, mirroring the Int branch — a required Number param with
        // MinInt > 1 (or MaxInt < 1) must still get an in-bounds filler so it stays
        // valid and the probe rejection remains attributable to the field under test.
        long long v = 1;
        if (p.MinInt) {
            v = *p.MinInt;
        } else if (p.MaxInt && *p.MaxInt < v) {
            v = *p.MaxInt;
        }
        return v;
    }
    case ParamType::Bool:
        return false;
    case ParamType::Json:
        return nlohmann::json::object();
    case ParamType::String:
    default:
        return std::string("x");
    }
}

/// First Required param in declaration order — the one ValidateAndResolveArgs names when
/// the args object is empty (Required is checked before any Default is applied). Returns
/// nullptr when the command has no required param.
inline const ParamSpec* FirstRequiredParam(const Command& c) {
    for (const ParamSpec& p : c.Params) {
        if (p.Required) {
            return &p;
        }
    }
    return nullptr;
}

/// Args object with every required param filled by SynthValidValue — the shared way to
/// build an args object that clears the required-arg check.
inline nlohmann::json SynthValidRequiredArgs(const Command& c) {
    nlohmann::json args = nlohmann::json::object();
    for (const ParamSpec& p : c.Params) {
        if (p.Required) {
            args[p.Name] = SynthValidValue(p);
        }
    }
    return args;
}

/// First non-Json param in declaration order (a scalar: String/Int/Number/Bool). A JSON
/// array can never coerce to a scalar, so this is the param a wrong-type probe targets.
/// Returns nullptr when every param is Json (or there are none).
inline const ParamSpec* FirstScalarParam(const Command& c) {
    for (const ParamSpec& p : c.Params) {
        if (p.Type != ParamType::Json) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_PARAM_VALUE_SYNTH_PURE_H
