#ifndef SMATCHET_MCP_MCPJSONRPCPURE_H
#define SMATCHET_MCP_MCPJSONRPCPURE_H

#include <cstddef>
#include <iosfwd>
#include <string>

#include <nlohmann/json.hpp>

// Pure JSON-RPC / MCP-protocol helpers — no <httplib.h>, <cpr/*>, or <winsock2.h>.
// Keeps the wire-format surface link-clean for the test rig and for any future
// non-HTTP consumer. Behaviour matches the original anonymous-namespace helpers
// in McpPlugin.cpp byte-for-byte.
namespace smatchet {
namespace mcp {
namespace pure {

// Internal log-summary / string helpers, promoted from an anonymous namespace so
// the dispatch test rig can exercise them directly (they back BuildRunLuaSummary /
// BuildToolCallSummary). Behaviour is unchanged — same byte-for-byte logic as the
// former file-local statics.
namespace detail {

/// Lowercase an ASCII string (per-byte std::tolower). Non-ASCII bytes pass through.
std::string ToLowerAscii(std::string value);

/// Trim leading + trailing ASCII whitespace (std::isspace).
std::string TrimAsciiWhitespace(const std::string& value);

/// Final path component after the last '/' or '\\' (empty in -> empty out).
std::string BasenameForDisplay(const std::string& path);

/// Append ` key=value` for each allow-listed identifier key present in `obj`
/// (issue_key / key / id / priority); string values are TruncateOneLine'd to 80.
void AppendAllowlistedArgKvs(std::ostringstream& oss, const nlohmann::json& obj);

} // namespace detail

// Base64Encode moved to Core's StringUtil.h (was byte-identical to the tracker's encoder).

bool LooksLikeHttpUrl(const std::string& url);

std::string NormalizeDomain(const std::string& rawDomain);

std::string ExtractHostFromUrl(const std::string& url);

// True iff the authority (between `://` and the first `/?#`) carries userinfo,
// i.e. contains an '@'. curl/cpr dial the host AFTER the last '@', so a naive
// allow-list keyed on the userinfo-looking prefix would pass while the fetch --
// with the Basic-auth tracker credential -- lands on the attacker host. The
// attachment proxy rejects any such URL outright before host validation.
bool UrlHasUserinfo(const std::string& url);

bool IsLoopbackAddress(const std::string& remoteAddr);

// DNS-rebinding defence (synthesis #4). A rebound browser connects to the
// loopback port but carries the attacker's hostname in `Host:`, never the
// literal 127.0.0.1 -- so accepting only loopback-literal Host values closes the
// attack without breaking legitimate local MCP clients (which send
// `Host: 127.0.0.1:<port>` or `localhost`). Case-folds, strips an optional
// `:port`, handles bracketed IPv6 (`[::1]:port`), and rejects a trailing dot
// (`127.0.0.1.` / `localhost.`) which resolves to loopback but is not a literal
// a legit client emits. Empty Host is rejected (fail closed).
bool IsLoopbackHostHeader(const std::string& hostHeader);

// Origin policy for the MCP loopback server. A legitimate local MCP client / the
// same-origin tooling sends NO Origin (empty -> accept) or, at most, a loopback
// Origin; a malicious cross-origin browser page carries the attacker's Origin.
// Accepts: empty, the literal "null" (sandboxed/file origins send this), or an
// http/https Origin whose host is a loopback literal. Rejects everything else.
bool IsAllowedMcpOrigin(const std::string& originHeader);

// Combined fail-closed Host/Origin decision for the Authorize path. Returns true
// iff the Host header is loopback-literal AND the Origin (when present) is
// allowed. `reason` is set to a short stable token ("bad_host" / "bad_origin")
// for LOG_WARN when the decision is reject; left untouched on accept.
bool IsMcpHostOriginAllowed(const std::string& hostHeader, const std::string& originHeader, std::string& reason);

// Constant-time string compare: return true iff a == b. Always reads max(|a|,|b|) bytes.
bool ConstantTimeStringEquals(const std::string& a, const std::string& b);

bool IsAllowedAttachmentHost(const std::string& host, const std::string& trackerDomain);

// Max concurrent SSE streams the MCP server accepts. Each SSE stream parks a worker in the
// heartbeat wait-loop for its whole lifetime; the httplib pool is 8 (StartServerThread), so
// an unbounded number of SSE connections would exhaust the pool and silently hang every
// later connection in httplib's job deque (security synthesis #20). 4 leaves >= 4 workers
// for keep-alive POST channels + tool calls while bounding the parking surface.
constexpr int kMaxConcurrentSseConnections = 4;

// Connection-bound decision for a new SSE stream. `currentActive` is the count of SSE
// streams already parked (BEFORE admitting this one). Returns true iff admitting one more
// would stay within `kMaxConcurrentSseConnections` (i.e. currentActive < cap). Pure — the
// caller owns the atomic counter + the 503 response. Negative input fails closed (false).
bool CanAcceptSseConnection(int currentActive);

// Access-Control-Allow-Origin value for the SSE route, or "" for "emit no CORS header".
// Never a wildcard: a native MCP client sends no Origin and needs no CORS grant, and a
// browser client only ever passes Authorize on the loopback bind (where the Host/Origin
// gate above has already vetted its Origin as loopback) — echo exactly that vetted Origin,
// re-vetted here via IsAllowedMcpOrigin for defence in depth. On the 0.0.0.0 bind the
// Host/Origin gate is intentionally skipped, so any grant would let an arbitrary web origin
// read the token-authenticated stream: emit nothing.
std::string ResolveSseCorsOrigin(const std::string& requestOrigin, bool loopbackBind);

/// Cap a string to `maxChars` characters and append "..." when truncated. Pure.
std::string TruncateOneLine(const std::string& s, std::size_t maxChars);

/// Single canonical `run_lua` tool-list entry — eliminates the REST / JSON-RPC duplication (§5.1).
nlohmann::json BuildRunLuaToolEntry();

/** Summarize `run_lua` arguments (mode, code/script size or basename, allowlisted nested `args`). */
std::string BuildRunLuaSummary(const nlohmann::json& arguments);

/** Tool name plus sorted argument keys and allowlisted identifier values. */
std::string BuildToolCallSummary(const std::string& toolName, const nlohmann::json& arguments);

std::string ExtractJsonRpcErrorMessage(const nlohmann::json& jres, std::size_t maxLen);

} // namespace pure
} // namespace mcp
} // namespace smatchet

#endif // SMATCHET_MCP_MCPJSONRPCPURE_H
