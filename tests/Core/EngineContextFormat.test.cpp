// EngineContextFormat tests — the pure snapshot→markdown formatter behind the
// quick-create issue description prefill (quick-create-issue-unreal-context plan):
// per-toggle filtering, missing-key skips, actor cap, log-tail truncation, and the
// empty/malformed-snapshot → empty-string contract.

#include "Diagnostics/EngineContextFormat.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

namespace {

using smatchet::hostctx::BuildEngineContextMarkdown;
using smatchet::hostctx::EngineContextToggles;
using smatchet::hostctx::TogglesFromConfig;

nlohmann::json FullSnapshot() {
    nlohmann::json j = nlohmann::json::object();
    j["source"] = "unreal";
    j["engineVersion"] = "Unreal Engine 5.6.1-12345+++UE5+Release-5.6";
    j["projectName"] = "MyGame";
    j["projectDir"] = "D:/Proj/MyGame/";
    j["platform"] = "Windows";
    j["osVersion"] = "Windows 11 (23H2)";
    j["buildConfig"] = "Development";
    j["mapName"] = "TestArena";
    j["pie"] = {{"active", true}, {"simulating", false}};
    j["selectedActors"] = nlohmann::json::array({{{"label", "BP_Door_2"}, {"class", "BP_Door_C"}}});
    j["logTail"] = nlohmann::json::array({"[Log] LogTemp: line one", "[Warning] LogNet: line two"});
    return j;
}

} // namespace

TEST_CASE("BuildEngineContextMarkdown emits every enabled+present section") {
    const std::string md = BuildEngineContextMarkdown(FullSnapshot(), EngineContextToggles{});
    CHECK(md.find("### Engine context") == 0);
    CHECK(md.find("- Engine: Unreal Engine 5.6.1") != std::string::npos);
    CHECK(md.find("- Project: MyGame (`D:/Proj/MyGame/`)") != std::string::npos);
    CHECK(md.find("- Platform: Windows (Windows 11 (23H2)) — Development build") != std::string::npos);
    CHECK(md.find("- Map: TestArena") != std::string::npos);
    CHECK(md.find("- PIE: active") != std::string::npos);
    CHECK(md.find("- Selected actors (1):") != std::string::npos);
    CHECK(md.find("  - BP_Door_2 (BP_Door_C)") != std::string::npos);
    CHECK(md.find("#### Output Log (last 2 lines)") != std::string::npos);
    CHECK(md.find("[Warning] LogNet: line two") != std::string::npos);
}

TEST_CASE("BuildEngineContextMarkdown filters by toggles") {
    EngineContextToggles t;
    t.EngineVersion = false;
    t.Project = false;
    t.Platform = false;
    t.PieState = false;
    t.SelectedActors = false;
    t.LogTail = false;
    // Only Level stays on.
    const std::string md = BuildEngineContextMarkdown(FullSnapshot(), t);
    CHECK(md.find("- Map: TestArena") != std::string::npos);
    CHECK(md.find("- Engine:") == std::string::npos);
    CHECK(md.find("- Project:") == std::string::npos);
    CHECK(md.find("- Platform:") == std::string::npos);
    CHECK(md.find("- PIE:") == std::string::npos);
    CHECK(md.find("Selected actors") == std::string::npos);
    CHECK(md.find("Output Log") == std::string::npos);
}

TEST_CASE("BuildEngineContextMarkdown returns empty when nothing applies") {
    SUBCASE("non-object snapshot") {
        CHECK(BuildEngineContextMarkdown(nlohmann::json(), EngineContextToggles{}).empty());
        CHECK(BuildEngineContextMarkdown(nlohmann::json::array(), EngineContextToggles{}).empty());
        CHECK(BuildEngineContextMarkdown(nlohmann::json("plain string"), EngineContextToggles{}).empty());
    }
    SUBCASE("empty object") {
        CHECK(BuildEngineContextMarkdown(nlohmann::json::object(), EngineContextToggles{}).empty());
    }
    SUBCASE("all toggles off") {
        EngineContextToggles t;
        t.EngineVersion = t.Project = t.Platform = t.Level = t.PieState = t.SelectedActors = t.LogTail = false;
        CHECK(BuildEngineContextMarkdown(FullSnapshot(), t).empty());
    }
    SUBCASE("enabled fields absent from the snapshot") {
        nlohmann::json j = nlohmann::json::object();
        j["source"] = "unreal"; // present but not a rendered field
        CHECK(BuildEngineContextMarkdown(j, EngineContextToggles{}).empty());
    }
}

TEST_CASE("BuildEngineContextMarkdown skips mistyped keys instead of throwing") {
    nlohmann::json j = FullSnapshot();
    j["engineVersion"] = 42;                                // wrong type — skipped
    j["pie"] = "yes";                                       // wrong type — skipped
    j["selectedActors"] = {{{"unexpected", true}}};         // entry without label/class — skipped
    j["logTail"] = nlohmann::json::array({7, "real line"}); // non-string entries render blank
    const std::string md = BuildEngineContextMarkdown(j, EngineContextToggles{});
    CHECK(md.find("- Engine:") == std::string::npos);
    CHECK(md.find("- PIE:") == std::string::npos);
    CHECK(md.find("- Map: TestArena") != std::string::npos);
    CHECK(md.find("real line") != std::string::npos);
}

TEST_CASE("BuildEngineContextMarkdown caps the actor list at 25") {
    nlohmann::json j = FullSnapshot();
    nlohmann::json actors = nlohmann::json::array();
    for (int i = 0; i < 30; ++i) {
        actors.push_back({{"label", "Actor_" + std::to_string(i)}, {"class", "AStaticMeshActor"}});
    }
    j["selectedActors"] = actors;
    const std::string md = BuildEngineContextMarkdown(j, EngineContextToggles{});
    CHECK(md.find("- Selected actors (30):") != std::string::npos);
    CHECK(md.find("Actor_24") != std::string::npos);
    CHECK(md.find("Actor_25 ") == std::string::npos); // 26th entry not listed
    CHECK(md.find("…and 5 more") != std::string::npos);
}

TEST_CASE("BuildEngineContextMarkdown truncates the log tail to the last N lines") {
    nlohmann::json j = nlohmann::json::object();
    nlohmann::json lines = nlohmann::json::array();
    for (int i = 0; i < 10; ++i) {
        lines.push_back("line " + std::to_string(i));
    }
    j["logTail"] = lines;

    EngineContextToggles t;
    t.LogTailLines = 3;
    const std::string md = BuildEngineContextMarkdown(j, t);
    CHECK(md.find("#### Output Log (last 3 lines)") != std::string::npos);
    CHECK(md.find("line 6") == std::string::npos);
    CHECK(md.find("line 7") != std::string::npos);
    CHECK(md.find("line 9") != std::string::npos);

    SUBCASE("out-of-band line counts clamp defensively") {
        t.LogTailLines = -10;
        const std::string clampedLow = BuildEngineContextMarkdown(j, t);
        CHECK(clampedLow.find("#### Output Log (last 1 lines)") != std::string::npos);

        t.LogTailLines = 100000;
        const std::string clampedHigh = BuildEngineContextMarkdown(j, t);
        CHECK(clampedHigh.find("#### Output Log (last 10 lines)") != std::string::npos);
    }
}

TEST_CASE("TogglesFromConfig mirrors the TrackerConfig fields") {
    TrackerConfig cfg;
    cfg.QuickCreateCtxEngineVersion = false;
    cfg.QuickCreateCtxProject = true;
    cfg.QuickCreateCtxPlatform = false;
    cfg.QuickCreateCtxLevel = true;
    cfg.QuickCreateCtxPieState = false;
    cfg.QuickCreateCtxSelectedActors = true;
    cfg.QuickCreateCtxLogTail = false;
    cfg.QuickCreateCtxLogLines = 77;

    const EngineContextToggles t = TogglesFromConfig(cfg);
    CHECK(t.EngineVersion == false);
    CHECK(t.Project == true);
    CHECK(t.Platform == false);
    CHECK(t.Level == true);
    CHECK(t.PieState == false);
    CHECK(t.SelectedActors == true);
    CHECK(t.LogTail == false);
    CHECK(t.LogTailLines == 77);
}
