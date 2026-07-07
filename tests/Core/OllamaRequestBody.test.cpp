#include <doctest/doctest.h>

#include "AiTypes.h"

#include <nlohmann/json.hpp>
#include <string>

using nlohmann::json;

// Defined in OllamaClient.cpp. Declared here (rather than pulling OllamaClient.h)
// so this test does not tie the sibling-provider-client headers into a diff — the
// DRY scanner surfaces their pre-existing structural clones whenever one is touched.
std::string OllamaBuildRequestBodyJson(const AiChatRequest& req);

// DR26: Ollama's /api/chat ignores a top-level `system` field (that is an
// /api/generate parameter), so the system prompt must be emitted as a leading
// {role:"system"} message or it is silently dropped.
TEST_CASE("OllamaBuildRequestBodyJson: system prompt is a leading role=system message") {
    AiChatRequest req;
    req.Model = "llama3";
    req.SystemPrompt = "You are a helpful agent.";
    AiMessage user;
    user.Role = "user";
    user.Content = "hello";
    req.History.push_back(user);

    const json body = json::parse(OllamaBuildRequestBodyJson(req));

    // No top-level `system` field (it would be ignored by /api/chat).
    CHECK_FALSE(body.contains("system"));

    REQUIRE(body.contains("messages"));
    REQUIRE(body["messages"].is_array());
    REQUIRE(body["messages"].size() == 2);
    CHECK(body["messages"][0]["role"] == "system");
    CHECK(body["messages"][0]["content"] == "You are a helpful agent.");
    CHECK(body["messages"][1]["role"] == "user");
    CHECK(body["messages"][1]["content"] == "hello");
}

TEST_CASE("OllamaBuildRequestBodyJson: no system message when the prompt is empty") {
    AiChatRequest req;
    req.Model = "llama3";
    AiMessage user;
    user.Role = "user";
    user.Content = "hi";
    req.History.push_back(user);

    const json body = json::parse(OllamaBuildRequestBodyJson(req));
    CHECK_FALSE(body.contains("system"));
    REQUIRE(body["messages"].size() == 1);
    CHECK(body["messages"][0]["role"] == "user");
}
