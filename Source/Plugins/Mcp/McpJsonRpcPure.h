#ifndef SMATCHET_MCP_MCPJSONRPCPURE_H
#define SMATCHET_MCP_MCPJSONRPCPURE_H

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

// Pure JSON-RPC / MCP-protocol helpers — no <httplib.h>, <cpr/*>, or <winsock2.h>.
// Keeps the wire-format surface link-clean for the test rig and for any future
// non-HTTP consumer. Behaviour matches the original anonymous-namespace helpers
// in McpPlugin.cpp byte-for-byte.
namespace smatchet {
namespace mcp {
namespace pure {

std::string Base64Encode(const std::string& in);

bool LooksLikeHttpUrl(const std::string& url);

std::string NormalizeDomain(const std::string& rawDomain);

std::string ExtractHostFromUrl(const std::string& url);

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
