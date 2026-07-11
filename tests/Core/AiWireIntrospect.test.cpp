// AiWireIntrospect — the ai.dump-request wire builders are now thin wrappers over
// the SAME per-client BuildChatBody / URL resolver the live dispatch path uses
// (no drift-prone debug mirror). These cases lock the wire shape per provider and,
// crucially, prove the dump reflects the FULL request (system + multi-turn history)
// — the retired mirrors only ever emitted a single user turn (backlog security B4).

#include "AiTypes.h"
#include "AiWireIntrospect.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

using smatchet::ai::AnthropicBuildChatBodyJson;
using smatchet::ai::AnthropicResolveChatUrl;
using smatchet::ai::OllamaNativeBuildChatBodyJson;
using smatchet::ai::OllamaNativeResolveChatUrl;
using smatchet::ai::OpenAiBuildChatBodyJson;
using smatchet::ai::OpenAiResolveChatUrl;

namespace {

// A request with a system prompt AND a two-turn history — the mirror could never
// represent the history, so any case asserting the assistant turn proves fidelity.
AiChatRequest MakeReq() {
    AiChatRequest r;
    r.Model = "model-x";
    r.SystemPrompt = "be terse";
    r.Temperature = -1.0f; // unset sentinel
    r.MaxTokens = 0;       // unset sentinel
    AiMessage u;
    u.Role = "user";
    u.Content = "hi";
    r.History.push_back(u);
    AiMessage a;
    a.Role = "assistant";
    a.Content = "hello";
    r.History.push_back(a);
    return r;
}

AiClientConfig MakeCfg(const std::string& base) {
    AiClientConfig c;
    c.BaseUrl = base;
    return c;
}

} // namespace

TEST_CASE("OpenAi wire body: full system+history, default max_tokens, temperature omitted when unset") {
    const nlohmann::json b = OpenAiBuildChatBodyJson(MakeReq());
    CHECK(b["model"] == "model-x");
    CHECK(b["stream"] == true);
    CHECK(b["max_tokens"] == 4096); // unset -> client default cap
    CHECK(b.find("temperature") == b.end());
    REQUIRE(b["messages"].is_array());
    REQUIRE(b["messages"].size() == 3); // system + user + assistant
    CHECK(b["messages"][0]["role"] == "system");
    CHECK(b["messages"][1]["role"] == "user");
    CHECK(b["messages"][2]["role"] == "assistant"); // fidelity: the mirror never carried history
}

TEST_CASE("OpenAi emits temperature only when set") {
    AiChatRequest r = MakeReq();
    r.Temperature = 0.7f;
    const nlohmann::json b = OpenAiBuildChatBodyJson(r);
    REQUIRE(b.find("temperature") != b.end());
    CHECK(b["temperature"].get<float>() == doctest::Approx(0.7f));
}

TEST_CASE("OpenAi chat URL strips a trailing /v1 (single-source with the client)") {
    CHECK(OpenAiResolveChatUrl(MakeCfg("http://host:1234/v1")) == "http://host:1234/v1/chat/completions");
    CHECK(OpenAiResolveChatUrl(MakeCfg("http://host:1234/v1/")) == "http://host:1234/v1/chat/completions");
    CHECK(OpenAiResolveChatUrl(MakeCfg("http://host:1234")) == "http://host:1234/v1/chat/completions");
}

TEST_CASE("Anthropic wire body: system at top level, messages carry history only") {
    const nlohmann::json b = AnthropicBuildChatBodyJson(MakeReq());
    CHECK(b["model"] == "model-x");
    CHECK(b["stream"] == true);
    CHECK(b["max_tokens"] == 4096);
    CHECK(b["system"] == "be terse"); // Anthropic: system is top-level, not a message
    REQUIRE(b["messages"].size() == 2);
    CHECK(b["messages"][0]["role"] == "user");
    CHECK(b["messages"][1]["role"] == "assistant");
    CHECK(AnthropicResolveChatUrl(MakeCfg("https://api.anthropic.com")) == "https://api.anthropic.com/v1/messages");
}

TEST_CASE("Ollama native wire body: system as leading message, /api/chat url") {
    const nlohmann::json b = OllamaNativeBuildChatBodyJson(MakeReq());
    CHECK(b["model"] == "model-x");
    CHECK(b["stream"] == true);
    REQUIRE(b["messages"].size() == 3); // system + user + assistant (system is a message here)
    CHECK(b["messages"][0]["role"] == "system");
    CHECK(b["messages"][2]["role"] == "assistant");
    CHECK(OllamaNativeResolveChatUrl(MakeCfg("http://localhost:11434")) == "http://localhost:11434/api/chat");
}
