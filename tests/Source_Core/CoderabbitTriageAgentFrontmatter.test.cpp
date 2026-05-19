// agents/coderabbit-triage.md frontmatter assertion. Locks the v2 shape
// landed in phase 8 of `coderabbit-react-loop` — spawned-harness mode +
// version bump. Mirrors HandoffAgentFrontmatter.test.cpp / PrIteratorAgentFrontmatter.test.cpp.

#include "agent_frontmatter_parse.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef SMATCHET_TESTS_REPO_ROOT
#error "SMATCHET_TESTS_REPO_ROOT must be defined to locate agents/<name>.md"
#endif

namespace {

const std::string kAgentPath = std::string(SMATCHET_TESTS_REPO_ROOT) + "/agents/coderabbit-triage.md";

bool Contains(const std::vector<std::string>& v, const std::string& needle) {
    return std::find(v.begin(), v.end(), needle) != v.end();
}

std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST_SUITE("CoderabbitTriageAgentFrontmatter") {
    TEST_CASE("parses cleanly") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE_MESSAGE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err),
                        "parse failed: " << err);
        CHECK(err.empty());
    }

    TEST_CASE("structured scalars match the v2 shape") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err));
        CHECK(fm.name == "coderabbit-triage");
        CHECK(fm.complexity == "medium");
        CHECK(fm.readOnly == true);
        // (coderabbit-react-loop phase 8) Bumped to 2 alongside the
        // `Spawned-harness mode` section. The version assertion is the
        // drift-guard that catches an accidental revert.
        CHECK(fm.version == 2);
    }

    TEST_CASE("triggers cover the locked set") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err));
        CHECK(Contains(fm.triggers, "coderabbit"));
        CHECK(Contains(fm.triggers, "PR bot"));
        CHECK(Contains(fm.triggers, "PR review comments"));
    }

    TEST_CASE("delegates-to includes implementer-class subagents the spawned-harness mode dispatches") {
        smatchet::test::frontmatter::ParsedFrontmatter fm;
        std::string err;
        REQUIRE(smatchet::test::frontmatter::ParseFrontmatter(kAgentPath, fm, err));
        // Phase-8 spawned-harness mode lets the agent dispatch these directly.
        CHECK(Contains(fm.delegatesTo, "tracker-backend"));
        CHECK(Contains(fm.delegatesTo, "grid-engine"));
        CHECK(Contains(fm.delegatesTo, "mechanic"));
        CHECK(Contains(fm.delegatesTo, "build-doctor"));
        CHECK(Contains(fm.delegatesTo, "test-author"));
    }

    TEST_CASE("body contains the Spawned-harness mode section") {
        const std::string body = ReadFile(kAgentPath);
        REQUIRE_FALSE(body.empty());
        // Phase-8 added this section; the assertion guards against an
        // accidental removal during a future refactor.
        CHECK(body.find("## Spawned-harness mode") != std::string::npos);
        CHECK(body.find("dispatch_source == \"coderabbit_comment\"") != std::string::npos);
    }
}
