#ifndef SMATCHET_CLI_ARG_COERCION_H
#define SMATCHET_CLI_ARG_COERCION_H

// Pure helper for the unified CLI's `--key=value` parser.
// Without this, ParseArgs stored every value as a JSON string, which made
// `scenario.run --registerLuaProvider=true` / `--pixPerFrame=8` raise
// `json.exception.type_error.302` inside the handler because it asked for a
// bool / int via `args.value(...)`. Coercion happens once at parse time so
// handlers can stay strongly-typed.
// Pure C++14 + nlohmann::json — no I/O, no globals, deterministic. Unit-tested
// in tests/Core/CliArgCoercion.test.cpp.

#include <nlohmann/json.hpp>

#include <string>

namespace smatchet {
namespace cli {

/// Coerce a raw `--key=value` value string to its most natural JSON scalar.
/// Rules (first-match wins):
///   - `true` / `false` (case-sensitive)            → JSON boolean.
///   - `^-?\d+$` and fits in `long long`             → JSON integer.
///   - `^-?\d+\.\d+([eE][-+]?\d+)?$`                  → JSON number (double).
///   - else                                            → JSON string.
/// Values that look numeric but don't fit in `long long` fall back to string
/// rather than overflow. Literal text the user might want as-is (`nan`,
/// `null`, `[…]`, `{…}`) stays a string — wrap in an explicit JSON-envelope
/// syntax if/when that path lands.
nlohmann::json CoerceCliArgValue(const std::string& raw);

/// True when `port` is a usable TCP port (1..65535). The CLI rejects an
/// out-of-range --mcp-port up front instead of handing httplib a port it can
/// never reach.
bool IsValidMcpPort(long long port);

/// Clamp a scenario frame count to a non-negative, overflow-safe range: negative
/// becomes 0, and a huge count is capped so the scenario driver's frame-count to
/// wait-ms arithmetic cannot overflow int.
int ClampScenarioFrames(long long frames);

/// Read a scenario `frames` argument from its coerced JSON value, clamped via
/// ClampScenarioFrames. Non-numeric or unparseable values fall back to
/// `defaultFrames`. Shared by the --spawn and in-process scenario drivers.
int ScenarioFramesFromJson(const nlohmann::json& framesValue, int defaultFrames);

/// Wall-clock wait budget (ms) for a scenario run: an explicit `--timeout` wins;
/// otherwise the budget is derived from the frame count (frames at 60 fps plus a
/// 30 s startup/teardown allowance). Shared by the --spawn and in-process
/// scenario drivers so the two paths cannot diverge — the --spawn path once
/// inlined only the frames-derived half and silently dropped `--timeout` while
/// its own timeout hint advertised the flag (issue #1943).
int ScenarioWaitMs(int timeoutMs, int frames);

} // namespace cli
} // namespace smatchet

#endif // SMATCHET_CLI_ARG_COERCION_H
