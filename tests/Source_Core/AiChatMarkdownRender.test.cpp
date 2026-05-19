// Bucket-A pure-logic tests for `AiChatMarkdownRender::ParseBlocks` — the
// md4c-driven markdown block walker. The Render() function is ImGui-side and
// lives in a separate TU not linked into the test rig.

#include "AiChatMarkdownRender.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

TEST_CASE("AiChatMarkdownRender::ParseBlocks empty input") {
    const auto blocks = AiChatMarkdownRender::ParseBlocks("");
    CHECK(blocks.empty());
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks single heading preserves markers") {
    const auto blocks = AiChatMarkdownRender::ParseBlocks("## Hello world");
    REQUIRE(blocks.size() == 1u);
    CHECK(blocks[0].Kind == AiChatMarkdownRender::BlockKind::Heading);
    CHECK(blocks[0].HeadingLevel == 2);
    CHECK(blocks[0].Text == "## Hello world");
    CHECK_FALSE(blocks[0].UseMonospace);
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks single paragraph flattens inline markers") {
    const auto blocks = AiChatMarkdownRender::ParseBlocks("Some **bold** and *italic* and `code`.");
    REQUIRE(blocks.size() == 1u);
    CHECK(blocks[0].Kind == AiChatMarkdownRender::BlockKind::Paragraph);
    CHECK(blocks[0].Text.find("**bold**") != std::string::npos);
    CHECK(blocks[0].Text.find("*italic*") != std::string::npos);
    CHECK(blocks[0].Text.find("`code`") != std::string::npos);
    CHECK_FALSE(blocks[0].UseMonospace);
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks fenced code block monospace + lang") {
    const std::string md = "```cpp\nint main() { return 0; }\n```";
    const auto blocks = AiChatMarkdownRender::ParseBlocks(md);
    REQUIRE(blocks.size() == 1u);
    CHECK(blocks[0].Kind == AiChatMarkdownRender::BlockKind::CodeFence);
    CHECK(blocks[0].UseMonospace);
    CHECK(blocks[0].LangTag == "cpp");
    CHECK(blocks[0].Text.find("int main()") != std::string::npos);
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks bullet list yields one ListItem per entry") {
    const std::string md = "- alpha\n- beta\n- gamma";
    const auto blocks = AiChatMarkdownRender::ParseBlocks(md);
    REQUIRE(blocks.size() == 3u);
    for (const auto& b : blocks) {
        CHECK(b.Kind == AiChatMarkdownRender::BlockKind::ListItem);
        CHECK(b.Text.substr(0, 2) == "- ");
    }
    CHECK(blocks[0].Text == "- alpha");
    CHECK(blocks[1].Text == "- beta");
    CHECK(blocks[2].Text == "- gamma");
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks ordered list emits numeric markers") {
    const std::string md = "1. first\n2. second\n3. third";
    const auto blocks = AiChatMarkdownRender::ParseBlocks(md);
    REQUIRE(blocks.size() == 3u);
    CHECK(blocks[0].Text == "1. first");
    CHECK(blocks[1].Text == "2. second");
    CHECK(blocks[2].Text == "3. third");
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks blockquote collapses paragraphs") {
    const std::string md = "> quoted line one\n> quoted line two";
    const auto blocks = AiChatMarkdownRender::ParseBlocks(md);
    REQUIRE(blocks.size() == 1u);
    CHECK(blocks[0].Kind == AiChatMarkdownRender::BlockKind::Quote);
    CHECK(blocks[0].Text.find("> quoted line one") != std::string::npos);
    CHECK(blocks[0].Text.find("> quoted line two") != std::string::npos);
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks mixed heading + paragraph + code") {
    const std::string md =
        "# Title\n\nIntro paragraph with *emphasis*.\n\n```python\nprint('hi')\n```\n\nClosing thought.";
    const auto blocks = AiChatMarkdownRender::ParseBlocks(md);
    REQUIRE(blocks.size() == 4u);
    CHECK(blocks[0].Kind == AiChatMarkdownRender::BlockKind::Heading);
    CHECK(blocks[0].Text == "# Title");
    CHECK(blocks[1].Kind == AiChatMarkdownRender::BlockKind::Paragraph);
    CHECK(blocks[1].Text.find("*emphasis*") != std::string::npos);
    CHECK(blocks[2].Kind == AiChatMarkdownRender::BlockKind::CodeFence);
    CHECK(blocks[2].LangTag == "python");
    CHECK(blocks[2].Text == "print('hi')");
    CHECK(blocks[3].Kind == AiChatMarkdownRender::BlockKind::Paragraph);
    CHECK(blocks[3].Text == "Closing thought.");
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks streaming-partial unclosed fence") {
    // The assistant streams "```cpp\nint x = 1;\nint y =" and we render before
    // the closing fence arrives. md4c auto-closes at EOF, so the partial code
    // surfaces in the last block.
    const std::string md = "```cpp\nint x = 1;\nint y =";
    const auto blocks = AiChatMarkdownRender::ParseBlocks(md);
    REQUIRE(!blocks.empty());
    CHECK(blocks.back().Kind == AiChatMarkdownRender::BlockKind::CodeFence);
    CHECK(blocks.back().UseMonospace);
    CHECK(blocks.back().Text.find("int x = 1;") != std::string::npos);
    CHECK(blocks.back().Text.find("int y =") != std::string::npos);
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks links flatten to raw [text](href)") {
    const std::string md = "Visit [Anthropic](https://www.anthropic.com) for docs.";
    const auto blocks = AiChatMarkdownRender::ParseBlocks(md);
    REQUIRE(blocks.size() == 1u);
    CHECK(blocks[0].Kind == AiChatMarkdownRender::BlockKind::Paragraph);
    CHECK(blocks[0].Text.find("[Anthropic](https://www.anthropic.com)") != std::string::npos);
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks horizontal rule emits Other block") {
    const std::string md = "Before\n\n---\n\nAfter";
    const auto blocks = AiChatMarkdownRender::ParseBlocks(md);
    REQUIRE(blocks.size() == 3u);
    CHECK(blocks[0].Kind == AiChatMarkdownRender::BlockKind::Paragraph);
    CHECK(blocks[1].Kind == AiChatMarkdownRender::BlockKind::Other);
    CHECK(blocks[1].Text == "---");
    CHECK(blocks[2].Kind == AiChatMarkdownRender::BlockKind::Paragraph);
}

TEST_CASE("AiChatMarkdownRender::ParseBlocks UseMonospace flag set only on code + table") {
    const std::string md = "# heading\n\npara\n\n```\ncode\n```\n\n- item";
    const auto blocks = AiChatMarkdownRender::ParseBlocks(md);
    REQUIRE(blocks.size() == 4u);
    CHECK_FALSE(blocks[0].UseMonospace);
    CHECK_FALSE(blocks[1].UseMonospace);
    CHECK(blocks[2].UseMonospace);
    CHECK_FALSE(blocks[3].UseMonospace);
}
