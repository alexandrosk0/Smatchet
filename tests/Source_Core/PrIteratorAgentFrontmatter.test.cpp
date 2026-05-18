// agents/pr-iterator.md frontmatter assertion. Locks the agent file shape
// for the H7-introduced second-stage handoff delegate. Same parser shared
// with HandoffAgentFrontmatter.test.cpp (`tests/_helpers/agent_frontmatter_parse`).

#include "agent_frontmatter_parse.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#ifndef SMATCHET_TESTS_REPO_ROOT
#error "SMATCHET_TESTS_REPO_ROOT must be defined to locate agents/<name>.md"
#endif

namespace {

const std::string kAgentPath = std::string(SMATCHET_TESTS_REPO_ROOT) + "/agents/pr-iterator.md";

bool Contains(const std::vector<std::string>& v, const std::string& needle) {
    return std::find(v.begin(), v.end(), needle) != v.end();
}

} // namespace

TEST_SUITE("PrIteratorAgentFrontmatter") {
    TEST_CASE("parses cleanly") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE_MESSAGE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err),
                        "parse failed: " << err);
        CHECK(err.empty());
    }

    TEST_CASE("structured scalars match the locked H7 shape") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err));
        CHECK(fm.name == "pr-iterator");
        CHECK(fm.complexity == "medium");
        CHECK(fm.readOnly == false);
        CHECK(fm.version == 1);
        // `class` is not a structured-field today — assert via rawScalars.
        CHECK(fm.rawScalars.count("class") == 1);
        CHECK(fm.rawScalars.at("class") == "Maintenance");
    }

    TEST_CASE("triggers cover the locked H7 set") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err));
        CHECK(Contains(fm.triggers, "pr-iterator"));
        CHECK(Contains(fm.triggers, "iterate on pr"));
        CHECK(Contains(fm.triggers, "address review"));
    }

    TEST_CASE("delegates-to covers the locked H7 set") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err));
        CHECK(Contains(fm.delegatesTo, "claude"));
        CHECK(Contains(fm.delegatesTo, "build-doctor"));
        CHECK(Contains(fm.delegatesTo, "test-author"));
    }
}
