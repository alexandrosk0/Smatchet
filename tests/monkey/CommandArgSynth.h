#ifndef SMATCHET_TESTS_MONKEY_COMMAND_ARG_SYNTH_H
#define SMATCHET_TESTS_MONKEY_COMMAND_ARG_SYNTH_H

// CommandArgSynth — seeded argument fuzzers for the nightly command-registry monkey
// (tests/monkey/monkey_command_registry.cpp). See docs/plans/nightly-monkey-tester.md.
//
// DESIGN INVARIANT (the reason this monkey is false-positive-free):
// every probe this header builds is GUARANTEED to be rejected by
// CommandRegistry::Dispatch BEFORE the command's handler body runs — it is a
// missing-required, un-coercible-type, out-of-bounds/over-length, or
// destructive-unconfirmed input (or a garbage command name). So a non-Ok result is
// the ONLY correct outcome, and no app-mutating handler ever executes headless. This
// mirrors exactly the boundary tests/Commands/BuiltinCommandsDispatch.test.cpp draws
// ("No app-mutating handler body runs in this slice"), but with seeded random VALUES
// inside each probe shape so the coercion / bounds / fuzzy-match code paths get
// exercised with adversarial data instead of one fixed literal.
//
// Handler-body execution (running a command's real handler against fake backends) is
// deliberately NOT here — it is the allow-list-gated 1b follow-up.
//
// All randomness comes from a single caller-owned std::mt19937_64 via the raw helpers
// below (never std::uniform_int_distribution, whose algorithm is not standardized
// across libstdc++/libc++ — raw modulo keeps a fixed seed byte-for-byte reproducible).
// Fuzzed strings stay valid UTF-8 on purpose: raw invalid UTF-8 would trip nlohmann's
// own serializer policy (type_error.316) rather than Smatchet logic, adding noise, not
// signal. NUL, control chars, quotes, backslashes and multi-byte code points are all in
// scope and are all valid UTF-8.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Commands/Command.h"
#include "Commands/ParamValueSynthPure.h"

namespace smatchet {
namespace monkey {

// Command-system types live in smatchet::cmd; surface the ones used below (type names in
// signatures don't resolve via ADL the way the synth free-functions do).
using cmd::Command;
using cmd::ParamSpec;
using cmd::ParamType;
using cmd::FirstRequiredParam;
using cmd::FirstScalarParam;
using cmd::SynthValidRequiredArgs;

// --- raw seeded primitives (portable, distribution-free) -------------------------

inline std::uint64_t NextU64(std::mt19937_64& rng) { return rng(); }

/// Uniform-ish index in [0, n). Returns 0 when n == 0. Modulo bias is irrelevant for a
/// fuzzer and keeps the sequence reproducible across standard libraries.
inline std::size_t PickIndex(std::mt19937_64& rng, std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(rng() % n); }

inline bool PickBool(std::mt19937_64& rng) { return (rng() & 1ULL) != 0; }

/// A long long in [lo, hi] (inclusive). If lo > hi, returns lo.
inline long long PickIntInRange(std::mt19937_64& rng, long long lo, long long hi) {
    if (hi <= lo) {
        return lo;
    }
    const unsigned long long span = static_cast<unsigned long long>(hi - lo) + 1ULL;
    return lo + static_cast<long long>(rng() % span);
}

/// A valid-UTF-8 string of exactly `len` bytes drawn from an adversarial-but-safe
/// charset (ASCII incl. NUL/controls/quotes/backslash, plus occasional 2-byte code
/// points). Every byte sequence produced is valid UTF-8 so nlohmann::dump never throws.
inline std::string RandUtf8SafeString(std::mt19937_64& rng, std::size_t len) {
    static const char kAscii[] = "abcXYZ012 \t\n\"\\/{}[]:,'`~!@#$%^&*()_+-=.?";
    static const std::size_t kAsciiN = sizeof(kAscii) - 1;
    std::string s;
    s.reserve(len);
    while (s.size() < len) {
        const std::uint64_t r = rng();
        const int kind = static_cast<int>(r % 16);
        if (kind == 0) {
            s.push_back('\0'); // embedded NUL — valid UTF-8 (U+0000), a classic fuzz vector
        } else if (kind == 1 && s.size() + 2 <= len) {
            // A 2-byte UTF-8 code point in [U+0080, U+07FF]: 110xxxxx 10xxxxxx.
            const unsigned cp = 0x80u + static_cast<unsigned>((r >> 8) % (0x7FFu - 0x80u + 1u));
            s.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            s.push_back(kAscii[(r >> 8) % kAsciiN]);
        }
    }
    return s;
}

// --- probe taxonomy --------------------------------------------------------------

// Every kind is guaranteed to be rejected before the handler (see the file header).
enum class ProbeKind {
    MissingRequired,  // omit a required param -> missing-required-arg
    WrongType,        // JSON array where a scalar is expected -> validation-error
    OutOfBounds,      // Int/Number outside MinInt/MaxInt -> validation-error
    OverLength,       // String longer than MaxLen -> validation-error
    DestructiveGate,  // destructive + valid args + no confirm -> confirm-required
    UnknownName       // a name that cannot exist -> unknown-command (fuzzes FuzzyMatch)
};

inline const char* ProbeKindName(ProbeKind k) {
    switch (k) {
    case ProbeKind::MissingRequired: return "missing-required";
    case ProbeKind::WrongType:       return "wrong-type";
    case ProbeKind::OutOfBounds:     return "out-of-bounds";
    case ProbeKind::OverLength:      return "over-length";
    case ProbeKind::DestructiveGate: return "destructive-gate";
    case ProbeKind::UnknownName:     return "unknown-name";
    }
    return "?";
}

/// Which probe kinds are applicable to command `c` (i.e. guaranteed to stop pre-handler).
/// A zero-param non-destructive command yields an EMPTY list — it has no probeable
/// surface (any dispatch would run its handler), so the driver skips it, exactly like
/// the existing harness's `skippedNoProbe` bucket.
inline std::vector<ProbeKind> ApplicableProbes(const Command& c) {
    std::vector<ProbeKind> out;
    if (FirstRequiredParam(c) != nullptr) {
        out.push_back(ProbeKind::MissingRequired);
    }
    if (FirstScalarParam(c) != nullptr) {
        out.push_back(ProbeKind::WrongType);
    }
    for (const ParamSpec& p : c.Params) {
        if ((p.Type == ParamType::Int || p.Type == ParamType::Number) && (p.MinInt || p.MaxInt)) {
            out.push_back(ProbeKind::OutOfBounds);
            break;
        }
    }
    for (const ParamSpec& p : c.Params) {
        if (p.Type == ParamType::String && p.MaxLen > 0) {
            out.push_back(ProbeKind::OverLength);
            break;
        }
    }
    if (c.Destructive) {
        out.push_back(ProbeKind::DestructiveGate);
    }
    return out;
}

// First param matching a predicate-ish selector, or nullptr.
inline const ParamSpec* FirstBoundedNumeric(const Command& c) {
    for (const ParamSpec& p : c.Params) {
        if ((p.Type == ParamType::Int || p.Type == ParamType::Number) && (p.MinInt || p.MaxInt)) {
            return &p;
        }
    }
    return nullptr;
}

inline const ParamSpec* FirstBoundedString(const Command& c) {
    for (const ParamSpec& p : c.Params) {
        if (p.Type == ParamType::String && p.MaxLen > 0) {
            return &p;
        }
    }
    return nullptr;
}

// --- probe builders --------------------------------------------------------------

// The concrete fuzz input plus enough metadata for the driver's oracle + replay log.
struct FuzzInput {
    ProbeKind kind;
    nlohmann::json args; // object (or, for UnknownName, unused)
    std::string overrideName; // non-empty only for UnknownName
};

/// Build a randomized, guaranteed-pre-handler-reject input of the given kind for `c`.
/// `kind` must be one returned by ApplicableProbes(c) (UnknownName is handled by the
/// driver directly and does not need a command).
inline FuzzInput BuildProbe(const Command& c, ProbeKind kind, std::mt19937_64& rng) {
    FuzzInput in;
    in.kind = kind;

    switch (kind) {
    case ProbeKind::MissingRequired: {
        // Start from all-required-filled, then DELETE one required param so the
        // required-check is the guaranteed stopper. Randomly also drop others / inject a
        // junk key so the args object shape varies run to run.
        nlohmann::json args = SynthValidRequiredArgs(c);
        std::vector<std::string> required;
        for (const ParamSpec& p : c.Params) {
            if (p.Required) {
                required.push_back(p.Name);
            }
        }
        if (!required.empty()) {
            args.erase(required[PickIndex(rng, required.size())]);
        }
        if (PickBool(rng)) {
            args["__monkey_junk"] = RandUtf8SafeString(rng, PickIndex(rng, 8));
        }
        in.args = std::move(args);
        break;
    }
    case ProbeKind::WrongType: {
        // Valid required args, then clobber a scalar param with a JSON array — never
        // coercible to String/Int/Number/Bool, so validation rejects deterministically.
        nlohmann::json args = SynthValidRequiredArgs(c);
        const ParamSpec* scalar = FirstScalarParam(c);
        if (scalar != nullptr) {
            // Random wrong shape: array of random junk, or a nested object.
            if (PickBool(rng)) {
                nlohmann::json arr = nlohmann::json::array();
                const std::size_t n = PickIndex(rng, 4);
                for (std::size_t i = 0; i < n; ++i) {
                    arr.push_back(RandUtf8SafeString(rng, PickIndex(rng, 6)));
                }
                args[scalar->Name] = std::move(arr);
            } else {
                args[scalar->Name] = {{"unexpected", "object"}};
            }
        }
        in.args = std::move(args);
        break;
    }
    case ProbeKind::OutOfBounds: {
        nlohmann::json args = SynthValidRequiredArgs(c);
        const ParamSpec* np = FirstBoundedNumeric(c);
        if (np != nullptr) {
            long long bad = 0;
            const bool underflow = np->MinInt && (!np->MaxInt || PickBool(rng));
            if (underflow) {
                bad = *np->MinInt - PickIntInRange(rng, 1, 1000);
            } else if (np->MaxInt) {
                bad = *np->MaxInt + PickIntInRange(rng, 1, 1000);
            } else {
                bad = PickIntInRange(rng, -1000, 1000);
            }
            args[np->Name] = bad;
        }
        in.args = std::move(args);
        break;
    }
    case ProbeKind::OverLength: {
        nlohmann::json args = SynthValidRequiredArgs(c);
        const ParamSpec* sp = FirstBoundedString(c);
        if (sp != nullptr) {
            const std::size_t over = sp->MaxLen + 1 + PickIndex(rng, 32);
            args[sp->Name] = RandUtf8SafeString(rng, over);
        }
        in.args = std::move(args);
        break;
    }
    case ProbeKind::DestructiveGate: {
        // Valid required args so validation passes and the CONFIRM GATE is the stopper.
        // Deliberately never set ConfirmedDestructive on the ctx (the driver enforces
        // that for this probe): an Ok result here would mean a destructive handler ran
        // unconfirmed — the exact security failure the gate exists to prevent.
        in.args = SynthValidRequiredArgs(c);
        break;
    }
    case ProbeKind::UnknownName: {
        // A name no command can have. Randomize so FuzzyMatch sees varied input.
        in.overrideName = "monkey.no_such_" + RandUtf8SafeString(rng, 4 + PickIndex(rng, 8));
        in.args = nlohmann::json::object();
        break;
    }
    }
    return in;
}

} // namespace monkey
} // namespace smatchet

#endif // SMATCHET_TESTS_MONKEY_COMMAND_ARG_SYNTH_H
