// CodingHarnessSeedBuilder pure-logic tests. The SEED.json shape produced here
// is the wire contract between the runner (Smatchet side) and the spawned
// harness's first delegate (`handoff-implementer`); these tests pin the shape
// against silent drift.

#include "CodingHarnessSeedBuilder.h"
#include "CodingHarnessTypes.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

namespace {

CodingHarness::Seed MakeFixtureSeed() {
    CodingHarness::Seed s;
    s.proposalId        = 42;
    s.sourceTracker     = "github";
    s.issueKey          = "smatchet/example#7";
    s.issueTitle        = "Fix flaky offline-queue replay test";
    s.issueBodyMarkdown = "The test occasionally fails on Windows CI.\n\nReproduction: run `ctest -R OfflineQueueServiceRuntime` five times.\n";
    s.approachOutline   = "1. Identify race in `OfflineQueueService::ReplayOnce`.\n2. Add explicit ordering via the new dispatcher.\n3. Update fake deps to drive the new ordering.";
    s.complexityHint    = "medium";
    s.targetBranch      = "agent/42/fix-flaky-offline-queue-replay-test";
    s.workingDirectory  = "C:/Development/Smatchet/.claude/worktrees/agent-42";
    s.timestampUnixSec  = 1747526400; // 2025-05-18T00:00:00Z
    return s;
}

CodingHarness::Seed MakeMinimalSeed() {
    CodingHarness::Seed s;
    s.proposalId       = 1;
    s.sourceTracker    = "github";
    s.issueKey         = "o/r#1";
    s.targetBranch     = "agent/1/x";
    s.workingDirectory = "/tmp/wt";
    s.timestampUnixSec = 1000;
    return s;
}

} // namespace

TEST_SUITE("CodingHarnessSeedBuilder.FormatSeedMarkdown") {
    TEST_CASE("deterministic on identical input") {
        const auto seed = MakeFixtureSeed();
        const std::string a = CodingHarness::SeedBuilder::FormatSeedMarkdown(seed);
        const std::string b = CodingHarness::SeedBuilder::FormatSeedMarkdown(seed);
        CHECK(a == b);
        CHECK_FALSE(a.empty());
    }

    TEST_CASE("includes every typed field in stable order") {
        const auto seed = MakeFixtureSeed();
        const std::string md = CodingHarness::SeedBuilder::FormatSeedMarkdown(seed);
        // The Markdown shape is part of the wire spec — handoff-implementer
        // weaves text from this file into commit messages. Pin the headings
        // and label literals so a stylistic rewrite cannot silently break
        // downstream readers.
        CHECK(md.find("# Smatchet handoff seed") != std::string::npos);
        CHECK(md.find("**proposalId:** 42") != std::string::npos);
        CHECK(md.find("**sourceTracker:** github") != std::string::npos);
        CHECK(md.find("**issueKey:** smatchet/example#7") != std::string::npos);
        CHECK(md.find("**issueTitle:** Fix flaky offline-queue replay test") != std::string::npos);
        CHECK(md.find("**complexityHint:** medium") != std::string::npos);
        CHECK(md.find("**targetBranch:** agent/42/fix-flaky-offline-queue-replay-test") != std::string::npos);
        CHECK(md.find("## Issue body") != std::string::npos);
        CHECK(md.find("## Approach outline") != std::string::npos);
    }

    TEST_CASE("empty body renders the empty marker (not a degenerate fence)") {
        CodingHarness::Seed s = MakeMinimalSeed();
        s.issueBodyMarkdown.clear();
        s.approachOutline.clear();
        const std::string md = CodingHarness::SeedBuilder::FormatSeedMarkdown(s);
        CHECK(md.find("## Issue body\n\n_(empty)_") != std::string::npos);
        CHECK(md.find("## Approach outline\n\n_(empty)_") != std::string::npos);
    }
}

TEST_SUITE("CodingHarnessSeedBuilder.FormatSeedJson") {
    TEST_CASE("emits every known field at top level under canonical names") {
        const auto seed = MakeFixtureSeed();
        const nlohmann::json j = CodingHarness::SeedBuilder::FormatSeedJson(seed);
        REQUIRE(j.is_object());
        CHECK(j.at("proposalId").get<int64_t>() == 42);
        CHECK(j.at("sourceTracker").get<std::string>() == "github");
        CHECK(j.at("issueKey").get<std::string>() == "smatchet/example#7");
        CHECK(j.at("issueTitle").get<std::string>() == "Fix flaky offline-queue replay test");
        CHECK(j.at("issueBodyMarkdown").get<std::string>() == seed.issueBodyMarkdown);
        CHECK(j.at("approachOutline").get<std::string>() == seed.approachOutline);
        CHECK(j.at("complexityHint").get<std::string>() == "medium");
        CHECK(j.at("targetBranch").get<std::string>() == seed.targetBranch);
        CHECK(j.at("workingDirectory").get<std::string>() == seed.workingDirectory);
        CHECK(j.at("timestampUnixSec").get<int64_t>() == 1747526400);
    }

    TEST_CASE("payloadExtra keys merge in at top level after known fields") {
        CodingHarness::Seed s = MakeMinimalSeed();
        s.payloadExtra = nlohmann::json::object();
        s.payloadExtra["futureField"] = "v1";
        s.payloadExtra["nested"] = {{"a", 1}, {"b", 2}};
        const nlohmann::json j = CodingHarness::SeedBuilder::FormatSeedJson(s);
        CHECK(j.at("futureField").get<std::string>() == "v1");
        CHECK(j.at("nested").at("a").get<int>() == 1);
        CHECK(j.at("nested").at("b").get<int>() == 2);
    }

    TEST_CASE("payloadExtra collision with known key — typed field wins") {
        CodingHarness::Seed s = MakeMinimalSeed();
        s.payloadExtra = nlohmann::json::object();
        s.payloadExtra["proposalId"] = 999;          // collision
        s.payloadExtra["customKey"]  = "kept";
        const nlohmann::json j = CodingHarness::SeedBuilder::FormatSeedJson(s);
        CHECK(j.at("proposalId").get<int64_t>() == 1);          // typed field wins
        CHECK(j.at("customKey").get<std::string>() == "kept");
    }
}

TEST_SUITE("CodingHarnessSeedBuilder.ParseSeedJson") {
    TEST_CASE("round-trips a fully populated Seed back to identical fields") {
        const auto orig = MakeFixtureSeed();
        const nlohmann::json j = CodingHarness::SeedBuilder::FormatSeedJson(orig);
        CodingHarness::Seed parsed;
        std::string err;
        REQUIRE(CodingHarness::SeedBuilder::ParseSeedJson(j, parsed, err));
        CHECK(err.empty());
        CHECK(parsed.proposalId == orig.proposalId);
        CHECK(parsed.sourceTracker == orig.sourceTracker);
        CHECK(parsed.issueKey == orig.issueKey);
        CHECK(parsed.issueTitle == orig.issueTitle);
        CHECK(parsed.issueBodyMarkdown == orig.issueBodyMarkdown);
        CHECK(parsed.approachOutline == orig.approachOutline);
        CHECK(parsed.complexityHint == orig.complexityHint);
        CHECK(parsed.targetBranch == orig.targetBranch);
        CHECK(parsed.workingDirectory == orig.workingDirectory);
        CHECK(parsed.timestampUnixSec == orig.timestampUnixSec);
    }

    TEST_CASE("payloadExtra captures unknown top-level keys losslessly") {
        CodingHarness::Seed orig = MakeMinimalSeed();
        orig.payloadExtra = nlohmann::json::object();
        orig.payloadExtra["futureFieldA"] = "alpha";
        orig.payloadExtra["futureFieldB"] = 7;
        const nlohmann::json j = CodingHarness::SeedBuilder::FormatSeedJson(orig);
        CodingHarness::Seed parsed;
        std::string err;
        REQUIRE(CodingHarness::SeedBuilder::ParseSeedJson(j, parsed, err));
        REQUIRE(parsed.payloadExtra.is_object());
        CHECK(parsed.payloadExtra.at("futureFieldA").get<std::string>() == "alpha");
        CHECK(parsed.payloadExtra.at("futureFieldB").get<int>() == 7);
    }

    TEST_CASE("missing required string field is a clear error") {
        nlohmann::json j = CodingHarness::SeedBuilder::FormatSeedJson(MakeFixtureSeed());
        j.erase("issueKey");
        CodingHarness::Seed parsed;
        std::string err;
        CHECK_FALSE(CodingHarness::SeedBuilder::ParseSeedJson(j, parsed, err));
        CHECK(err.find("issueKey") != std::string::npos);
    }

    TEST_CASE("missing required integer field is a clear error") {
        nlohmann::json j = CodingHarness::SeedBuilder::FormatSeedJson(MakeFixtureSeed());
        j.erase("timestampUnixSec");
        CodingHarness::Seed parsed;
        std::string err;
        CHECK_FALSE(CodingHarness::SeedBuilder::ParseSeedJson(j, parsed, err));
        CHECK(err.find("timestampUnixSec") != std::string::npos);
    }

    TEST_CASE("string-typed field with non-string value rejected") {
        nlohmann::json j = CodingHarness::SeedBuilder::FormatSeedJson(MakeFixtureSeed());
        j["sourceTracker"] = 42;
        CodingHarness::Seed parsed;
        std::string err;
        CHECK_FALSE(CodingHarness::SeedBuilder::ParseSeedJson(j, parsed, err));
        CHECK(err.find("sourceTracker") != std::string::npos);
    }

    TEST_CASE("non-object root rejected") {
        nlohmann::json j = nlohmann::json::array();
        CodingHarness::Seed parsed;
        std::string err;
        CHECK_FALSE(CodingHarness::SeedBuilder::ParseSeedJson(j, parsed, err));
        CHECK_FALSE(err.empty());
    }
}
