// agents/handoff-implementer.md frontmatter assertion. Locks the agent file
// shape that ClaudeCodeLocalRunner + AgenticHandoffController (H3/H4) depend
// on so the doctest rig fails fast if the agent file drifts during a refactor.

#include "agent_frontmatter_parse.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#ifndef SMATCHET_TESTS_REPO_ROOT
#error "SMATCHET_TESTS_REPO_ROOT must be defined to locate agents/<name>.md"
#endif

namespace {

const std::string kAgentPath = std::string(SMATCHET_TESTS_REPO_ROOT) + "/agents/handoff-implementer.md";

bool Contains(const std::vector<std::string>& v, const std::string& needle) {
    return std::find(v.begin(), v.end(), needle) != v.end();
}

} // namespace

TEST_SUITE("HandoffAgentFrontmatter") {
    TEST_CASE("parses cleanly") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE_MESSAGE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err),
                        "parse failed: " << err);
        CHECK(err.empty());
    }

    TEST_CASE("structured scalars match the locked H2 shape") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err));
        CHECK(fm.name == "handoff-implementer");
        CHECK(fm.complexity == "medium");
        CHECK(fm.readOnly == false);
        CHECK(fm.version == 1);
        // `class` is not a structured-field today — assert via rawScalars so
        // we still pin the Implementer class without bloating the parser API.
        CHECK(fm.rawScalars.count("class") == 1);
        CHECK(fm.rawScalars.at("class") == "Implementer");
    }

    TEST_CASE("triggers cover the locked H2 set") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err));
        CHECK(Contains(fm.triggers, "handoff-implementer"));
        CHECK(Contains(fm.triggers, "implement issue"));
        CHECK(Contains(fm.triggers, "agent handoff"));
    }

    TEST_CASE("delegates-to covers the locked H2 set") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err));
        CHECK(Contains(fm.delegatesTo, "claude"));
        CHECK(Contains(fm.delegatesTo, "build-doctor"));
        CHECK(Contains(fm.delegatesTo, "test-author"));
    }
}
