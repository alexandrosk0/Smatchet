#include <doctest/doctest.h>

#include "AiWirePure.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet::ai::pure::BuildChatMessages;
using smatchet::ai::pure::BuildHistoryMessages;
using smatchet::ai::pure::JoinUrl;
using smatchet::ai::pure::ResolveBaseUrlOr;

namespace {

AiMessage MakeMsg(const char* role, const char* content) {
    AiMessage m;
    m.Role = role;
    m.Content = content;
    m.CreatedAtUnixMs = 1700000000000;
    m.Pinned = true;
    return m;
}

} // namespace

TEST_CASE("JoinUrl puts exactly one slash between base and path") {
    SUBCASE("base without a trailing slash") { CHECK(JoinUrl("http://h:1", "/api/chat") == "http://h:1/api/chat"); }
    SUBCASE("base with one trailing slash") { CHECK(JoinUrl("http://h:1/", "/api/chat") == "http://h:1/api/chat"); }
    SUBCASE("base with many trailing slashes") {
        CHECK(JoinUrl("http://h:1//", "/api/chat") == "http://h:1/api/chat");
        CHECK(JoinUrl("http://h:1/////", "/api/chat") == "http://h:1/api/chat");
    }
    SUBCASE("path without a leading slash") {
        CHECK(JoinUrl("http://h:1", "api/chat") == "http://h:1/api/chat");
        CHECK(JoinUrl("http://h:1/", "api/chat") == "http://h:1/api/chat");
    }
    SUBCASE("path with many leading slashes") { CHECK(JoinUrl("http://h:1/", "//api/chat") == "http://h:1/api/chat"); }
    SUBCASE("base path segments are kept") {
        CHECK(JoinUrl("http://proxy/prefix", "/v1/messages") == "http://proxy/prefix/v1/messages");
        CHECK(JoinUrl("http://proxy/prefix//", "/v1/messages") == "http://proxy/prefix/v1/messages");
    }
    SUBCASE("slashes inside the path are untouched") {
        CHECK(JoinUrl("http://h:1", "/v1//models/") == "http://h:1/v1//models/");
    }
}

TEST_CASE("JoinUrl edge inputs") {
    SUBCASE("empty path yields the trimmed base plus one slash") {
        CHECK(JoinUrl("http://h:1", "") == "http://h:1/");
        CHECK(JoinUrl("http://h:1///", "") == "http://h:1/");
        CHECK(JoinUrl("http://h:1", nullptr) == "http://h:1/");
    }
    SUBCASE("empty or all-slash base yields a rooted path") {
        CHECK(JoinUrl("", "/api/chat") == "/api/chat");
        CHECK(JoinUrl("", "api/chat") == "/api/chat");
        CHECK(JoinUrl("//", "/api/chat") == "/api/chat");
        CHECK(JoinUrl("", "") == "/");
    }
}

TEST_CASE("ResolveBaseUrlOr prefers the configured base URL") {
    CHECK(ResolveBaseUrlOr("http://custom:8080", "https://api.default") == "http://custom:8080");
    CHECK(ResolveBaseUrlOr("", "https://api.default") == "https://api.default");
    CHECK(ResolveBaseUrlOr("", nullptr).empty());
}

TEST_CASE("ResolveBaseUrlOr drops every trailing slash") {
    CHECK(ResolveBaseUrlOr("http://custom:8080/", "https://api.default") == "http://custom:8080");
    CHECK(ResolveBaseUrlOr("http://custom:8080/v1//", "https://api.default") == "http://custom:8080/v1");
    CHECK(ResolveBaseUrlOr("", "https://api.default///") == "https://api.default");
    // A set but malformed base stays the user's choice; it never becomes the default host.
    CHECK(ResolveBaseUrlOr("///", "https://api.default").empty());
}

TEST_CASE("AppendHistoryMessages appends role + content only, in order") {
    nlohmann::json messages = nlohmann::json::array();
    messages.push_back("already-there");
    const std::vector<AiMessage> history{MakeMsg("user", "hi"), MakeMsg("assistant", "hello")};
    smatchet::ai::pure::detail::AppendHistoryMessages(messages, history);
    REQUIRE(messages.size() == 3);
    CHECK(messages[0] == "already-there");
    CHECK(messages[1] == nlohmann::json({{"role", "user"}, {"content", "hi"}}));
    CHECK(messages[2] == nlohmann::json({{"role", "assistant"}, {"content", "hello"}}));
    // Local-only AiMessage members (timestamp, pin) never reach the wire.
    CHECK(messages[1].size() == 2);
}

TEST_CASE("BuildHistoryMessages carries no system entry") {
    const std::vector<AiMessage> history{MakeMsg("user", "q"), MakeMsg("assistant", "a")};
    const nlohmann::json messages = BuildHistoryMessages(history);
    REQUIRE(messages.is_array());
    REQUIRE(messages.size() == 2);
    CHECK(messages[0] == nlohmann::json({{"role", "user"}, {"content", "q"}}));
    CHECK(messages[1] == nlohmann::json({{"role", "assistant"}, {"content", "a"}}));
    CHECK(BuildHistoryMessages(std::vector<AiMessage>()) == nlohmann::json::array());
}

TEST_CASE("BuildChatMessages leads with a system entry only when a system prompt is set") {
    const std::vector<AiMessage> history{MakeMsg("user", "q")};
    SUBCASE("with a system prompt") {
        const nlohmann::json messages = BuildChatMessages("be terse", history);
        REQUIRE(messages.size() == 2);
        CHECK(messages[0] == nlohmann::json({{"role", "system"}, {"content", "be terse"}}));
        CHECK(messages[1] == nlohmann::json({{"role", "user"}, {"content", "q"}}));
    }
    SUBCASE("without a system prompt") {
        const nlohmann::json messages = BuildChatMessages("", history);
        REQUIRE(messages.size() == 1);
        CHECK(messages[0]["role"] == "user");
    }
    SUBCASE("empty history and no system prompt is an empty array, not null") {
        const nlohmann::json messages = BuildChatMessages("", std::vector<AiMessage>());
        CHECK(messages.is_array());
        CHECK(messages.empty());
        CHECK(messages.dump() == "[]");
    }
}
