// MCP response/log envelope pure-logic tests — Base64, truncation, run_lua + generic
// tool-call summary builders, and JSON-RPC error-message extraction.

#include "McpJsonRpcPure.h"
#include "StringUtil.h" // Base64Encode now lives in Core (single-sourced)

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

using smatchet::mcp::pure::BuildRunLuaSummary;
using smatchet::mcp::pure::BuildToolCallSummary;
using smatchet::mcp::pure::ExtractJsonRpcErrorMessage;
using smatchet::mcp::pure::TruncateOneLine;

TEST_CASE("Base64Encode handles empty, sub-3-byte tails, aligned, and binary inputs") {
    CHECK(Base64Encode("") == "");

    // Single byte → 2 chars + "==".
    CHECK(Base64Encode("f") == "Zg==");

    // Two bytes → 3 chars + "=".
    CHECK(Base64Encode("fo") == "Zm8=");

    // Aligned 3-byte multiple.
    CHECK(Base64Encode("foo") == "Zm9v");
    CHECK(Base64Encode("foobar") == "Zm9vYmFy");

    // Canonical RFC 4648 fixture.
    CHECK(Base64Encode("Aladdin:open sesame") == "QWxhZGRpbjpvcGVuIHNlc2FtZQ==");

    // Non-ASCII bytes round-trip through the table cleanly.
    std::string highBytes;
    highBytes.push_back(static_cast<char>(0xFF));
    highBytes.push_back(static_cast<char>(0xFE));
    highBytes.push_back(static_cast<char>(0xFD));
    CHECK(Base64Encode(highBytes) == "//79");

    // Length growth law: ceil(n/3) * 4. 1024 random-ish bytes → 1368 chars.
    std::string oneKb(1024, 'A');
    CHECK(Base64Encode(oneKb).size() == ((1024 + 2) / 3) * 4);
}

TEST_CASE("TruncateOneLine respects cap and appends ellipsis on overflow") {
    CHECK(TruncateOneLine("", 10) == "");
    CHECK(TruncateOneLine("short", 10) == "short");
    CHECK(TruncateOneLine("exactly10!", 10) == "exactly10!");     // exactly-at-cap unchanged.
    CHECK(TruncateOneLine("exactly11!.", 10) == "exactly11!..."); // one-over → first 10 bytes + "...".

    // Zero cap → empty input passes through; non-empty triggers truncation to 0 + "...".
    CHECK(TruncateOneLine("", 0) == "");
    CHECK(TruncateOneLine("x", 0) == "...");

    // maxChars equal to size is the inclusive boundary (size() <= maxChars).
    CHECK(TruncateOneLine("abc", 3) == "abc");
    CHECK(TruncateOneLine("abc", 2) == "ab...");

    // Multibyte: TruncateOneLine slices by bytes, not codepoints. Doc the actual behaviour.
    // "héllo" in UTF-8 = h\xC3\xA9llo (6 bytes). Truncating to 2 bytes yields "h\xC3"
    // — a partial codepoint. Test only the byte-length law.
    const std::string utf8 = "h\xC3\xA9llo";
    CHECK(TruncateOneLine(utf8, 2).size() == 2 + 3); // 2 raw bytes + "..."
    CHECK(TruncateOneLine(utf8, 100) == utf8);
}

TEST_CASE("BuildRunLuaSummary formats snippet/script modes with allowlisted args") {
    // Snippet mode emits "mode=snippet code_len=N".
    nlohmann::json snippet;
    snippet["mode"] = "snippet";
    snippet["code"] = "abc";
    CHECK(BuildRunLuaSummary(snippet) == "mode=snippet code_len=3");

    // Script mode emits basename of the script path, truncated to 48 chars.
    nlohmann::json script;
    script["mode"] = "script";
    script["script"] = "/long/path/to/foo.lua";
    CHECK(BuildRunLuaSummary(script) == "mode=script script=foo.lua");

    // Backslash path separator (Windows) — same basename extraction.
    nlohmann::json scriptWin;
    scriptWin["mode"] = "script";
    scriptWin["script"] = "C:\\Scripts\\bar.lua";
    CHECK(BuildRunLuaSummary(scriptWin) == "mode=script script=bar.lua");

    // Allowlisted nested args trailer.
    nlohmann::json withArgs;
    withArgs["mode"] = "snippet";
    withArgs["code"] = "x";
    nlohmann::json inner;
    inner["issue_key"] = "ABC-1";
    withArgs["args"] = inner;
    CHECK(BuildRunLuaSummary(withArgs) == "mode=snippet code_len=1 issue_key=ABC-1");

    // Non-object input → empty string per is_object() guard.
    CHECK(BuildRunLuaSummary(nlohmann::json("not an object")) == "");
    CHECK(BuildRunLuaSummary(nlohmann::json::array()) == "");

    // Empty object → "mode=" with no value, no allowlisted tail.
    CHECK(BuildRunLuaSummary(nlohmann::json::object()) == "mode=");
}

TEST_CASE("BuildToolCallSummary emits tool + sorted key list and allowlisted values") {
    // Non-object args path — "tool=NAME" with no arg_keys suffix.
    CHECK(BuildToolCallSummary("my_tool", nlohmann::json("scalar")) == "tool=my_tool");
    CHECK(BuildToolCallSummary("my_tool", nlohmann::json::array()) == "tool=my_tool");

    // Object args — nlohmann::json iterates keys in sorted order for object{} (default).
    nlohmann::json args;
    args["a"] = 1;
    args["b"] = "x";
    const std::string out = BuildToolCallSummary("my_tool", args);
    CHECK(out.substr(0, std::string("tool=my_tool arg_keys=[a,b]").size()) == "tool=my_tool arg_keys=[a,b]");

    // Allowlisted key in top-level args (BuildToolCallSummary appends KVs from `arguments`
    // directly, not from a nested "args" — distinct from BuildRunLuaSummary).
    nlohmann::json keyed;
    keyed["issue_key"] = "PROJ-42";
    keyed["other"] = "z";
    const std::string keyedOut = BuildToolCallSummary("create_issue", keyed);
    // Sorted keys: issue_key, other. Allowlisted tail appends ` issue_key=PROJ-42`.
    CHECK(keyedOut == "tool=create_issue arg_keys=[issue_key,other] issue_key=PROJ-42");

    // Empty object → empty key list, no allowlisted tail.
    CHECK(BuildToolCallSummary("noop", nlohmann::json::object()) == "tool=noop arg_keys=[]");
}

TEST_CASE("BuildToolCallSummary truncates oversize allowlisted string values to 80 chars") {
    nlohmann::json keyed;
    keyed["id"] = std::string(120, 'x'); // 120 'x' chars, well over the 80-char cap.
    const std::string out = BuildToolCallSummary("t", keyed);
    // Tail is " id=" + truncate-80("xxxx…") → 80 'x' + "..."
    const std::string expected = "tool=t arg_keys=[id] id=" + std::string(80, 'x') + "...";
    CHECK(out == expected);
}

TEST_CASE("ExtractJsonRpcErrorMessage prefers .message string, falls back to dump") {
    // Missing error key → empty.
    CHECK(ExtractJsonRpcErrorMessage(nlohmann::json::object(), 80) == "");

    // Object with .message string → truncated.
    nlohmann::json withMsg;
    nlohmann::json err;
    err["code"] = -32601;
    err["message"] = "Method not found";
    withMsg["error"] = err;
    CHECK(ExtractJsonRpcErrorMessage(withMsg, 80) == "Method not found");

    // .message truncated when over cap.
    nlohmann::json longMsg;
    nlohmann::json longErr;
    longErr["message"] = std::string(100, 'z');
    longMsg["error"] = longErr;
    CHECK(ExtractJsonRpcErrorMessage(longMsg, 10) == std::string(10, 'z') + "...");

    // Object without .message → dump of the error object, truncated.
    nlohmann::json noMsg;
    nlohmann::json noMsgErr;
    noMsgErr["code"] = -1;
    noMsg["error"] = noMsgErr;
    const std::string dumped = ExtractJsonRpcErrorMessage(noMsg, 200);
    CHECK(dumped == noMsgErr.dump());

    // Non-object error (scalar) → dumped + truncated.
    nlohmann::json scalarErr;
    scalarErr["error"] = "plain string";
    CHECK(ExtractJsonRpcErrorMessage(scalarErr, 100) == nlohmann::json("plain string").dump());

    // Numeric error scalar.
    nlohmann::json numErr;
    numErr["error"] = 42;
    CHECK(ExtractJsonRpcErrorMessage(numErr, 10) == "42");
}
