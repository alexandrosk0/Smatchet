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

}  // namespace pure
}  // namespace mcp
}  // namespace smatchet

#endif  // SMATCHET_MCP_MCPJSONRPCPURE_H
