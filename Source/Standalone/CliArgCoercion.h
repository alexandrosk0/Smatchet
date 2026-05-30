#ifndef SMATCHET_CLI_ARG_COERCION_H
#define SMATCHET_CLI_ARG_COERCION_H

// Pure helper for the unified CLI's `--key=value` parser.
//
// Without this, ParseArgs stored every value as a JSON string, which made
// `scenario.run --registerLuaProvider=true` / `--pixPerFrame=8` raise
// `json.exception.type_error.302` inside the handler because it asked for a
// bool / int via `args.value(...)`. Coercion happens once at parse time so
// handlers can stay strongly-typed.
//
// Pure C++14 + nlohmann::json — no I/O, no globals, deterministic. Unit-tested
// in tests/Core/CliArgCoercion.test.cpp.

#include <nlohmann/json.hpp>

#include <string>

namespace smatchet {
namespace cli {

/// Coerce a raw `--key=value` value string to its most natural JSON scalar.
///
/// Rules (first-match wins):
///   - `true` / `false` (case-sensitive)            → JSON boolean.
///   - `^-?\d+$` and fits in `long long`             → JSON integer.
///   - `^-?\d+\.\d+([eE][-+]?\d+)?$`                  → JSON number (double).
///   - else                                            → JSON string.
///
/// Values that look numeric but don't fit in `long long` fall back to string
/// rather than overflow. Literal text the user might want as-is (`nan`,
/// `null`, `[…]`, `{…}`) stays a string — wrap in an explicit JSON-envelope
/// syntax if/when that path lands.
nlohmann::json CoerceCliArgValue(const std::string& raw);

} // namespace cli
} // namespace smatchet

#endif // SMATCHET_CLI_ARG_COERCION_H
