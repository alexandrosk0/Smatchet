// Pure-helper tests for the system-prompt + model-resolution phases extracted
// from `AiAssistantController::RunRequest` (monoliths decomposition). The
// helpers live in the `smatchet::ai::pure` namespace and are header-defined
// (`inline`) so this rig links them without the controller TU's AppController /
// Ui-session dependencies.
//
// The whole TU self-gates on SMATCHET_WITH_AI to match the controller header,
// which collapses to nothing when AI is disabled.

#if defined(SMATCHET_WITH_AI)

#include "AiAssistantController.h"
#include "AiTypes.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

AiContextBlock MakeBlock(const std::string& name, const std::string& body) {
    AiContextBlock b;
    b.Name = name;
    b.Body = body;
    return b;
}

} // namespace

TEST_CASE("ComposeSystemPrompt: empty agents.md and no blocks yields empty prompt") {
    const std::string out = smatchet::ai::pure::ComposeSystemPrompt(std::string(), std::vector<AiContextBlock>());
    CHECK(out.empty());
}

TEST_CASE("ComposeSystemPrompt: agents.md only, missing trailing newline gets one + separator") {
    const std::string out = smatchet::ai::pure::ComposeSystemPrompt("HELLO", std::vector<AiContextBlock>());
    CHECK(out == "HELLO\n\n---\n\n");
}

TEST_CASE("ComposeSystemPrompt: agents.md already newline-terminated is not double-newlined") {
    const std::string out = smatchet::ai::pure::ComposeSystemPrompt("HELLO\n", std::vector<AiContextBlock>());
    CHECK(out == "HELLO\n\n---\n\n");
}

TEST_CASE("ComposeSystemPrompt: single context block wraps in smatchet_context tags with header") {
    std::vector<AiContextBlock> blocks;
    blocks.push_back(MakeBlock("selection", "row data"));
    const std::string out = smatchet::ai::pure::ComposeSystemPrompt(std::string(), blocks);
    CHECK(out == "## Current Smatchet context\n\n"
                 "<smatchet_context block=\"selection\">\n"
                 "row data\n"
                 "</smatchet_context>\n");
}

TEST_CASE("ComposeSystemPrompt: body already newline-terminated is not double-newlined") {
    std::vector<AiContextBlock> blocks;
    blocks.push_back(MakeBlock("selection", "row data\n"));
    const std::string out = smatchet::ai::pure::ComposeSystemPrompt(std::string(), blocks);
    CHECK(out == "## Current Smatchet context\n\n"
                 "<smatchet_context block=\"selection\">\n"
                 "row data\n"
                 "</smatchet_context>\n");
}

TEST_CASE("ComposeSystemPrompt: empty-body blocks are skipped") {
    std::vector<AiContextBlock> blocks;
    blocks.push_back(MakeBlock("empty", ""));
    blocks.push_back(MakeBlock("real", "body"));
    const std::string out = smatchet::ai::pure::ComposeSystemPrompt(std::string(), blocks);
    CHECK(out == "## Current Smatchet context\n\n"
                 "<smatchet_context block=\"real\">\n"
                 "body\n"
                 "</smatchet_context>\n");
}

TEST_CASE("ComposeSystemPrompt: multiple blocks joined by a single newline between tags") {
    std::vector<AiContextBlock> blocks;
    blocks.push_back(MakeBlock("a", "one"));
    blocks.push_back(MakeBlock("b", "two"));
    const std::string out = smatchet::ai::pure::ComposeSystemPrompt(std::string(), blocks);
    CHECK(out == "## Current Smatchet context\n\n"
                 "<smatchet_context block=\"a\">\n"
                 "one\n"
                 "</smatchet_context>\n"
                 "\n"
                 "<smatchet_context block=\"b\">\n"
                 "two\n"
                 "</smatchet_context>\n");
}

TEST_CASE("ComposeSystemPrompt: agents.md prefix and context section both present") {
    std::vector<AiContextBlock> blocks;
    blocks.push_back(MakeBlock("sel", "x"));
    const std::string out = smatchet::ai::pure::ComposeSystemPrompt("AGENTS", blocks);
    CHECK(out == "AGENTS\n\n---\n\n"
                 "## Current Smatchet context\n\n"
                 "<smatchet_context block=\"sel\">\n"
                 "x\n"
                 "</smatchet_context>\n");
}

TEST_CASE("ComposeSystemPrompt: all-empty blocks emit no context header") {
    std::vector<AiContextBlock> blocks;
    blocks.push_back(MakeBlock("a", ""));
    blocks.push_back(MakeBlock("b", ""));
    const std::string out = smatchet::ai::pure::ComposeSystemPrompt("AGENTS", blocks);
    CHECK(out == "AGENTS\n\n---\n\n");
}

TEST_CASE("ResolveChatModel: non-empty override always wins") {
    const std::string out = smatchet::ai::pure::ResolveChatModel(AiProvider::OpenAi, "override-model", "openai-cfg",
                                                                 "anthropic-cfg", "ollama-cfg", "deepseek-cfg");
    CHECK(out == "override-model");
}

TEST_CASE("ResolveChatModel: provider maps to its configured model when no override") {
    const std::string empty;
    CHECK(smatchet::ai::pure::ResolveChatModel(AiProvider::OpenAi, empty, "o", "a", "ol", "d") == "o");
    CHECK(smatchet::ai::pure::ResolveChatModel(AiProvider::Anthropic, empty, "o", "a", "ol", "d") == "a");
    CHECK(smatchet::ai::pure::ResolveChatModel(AiProvider::OllamaNative, empty, "o", "a", "ol", "d") == "ol");
    CHECK(smatchet::ai::pure::ResolveChatModel(AiProvider::OllamaOpenAiCompat, empty, "o", "a", "ol", "d") == "ol");
    CHECK(smatchet::ai::pure::ResolveChatModel(AiProvider::DeepSeek, empty, "o", "a", "ol", "d") == "d");
}

#endif // SMATCHET_WITH_AI
