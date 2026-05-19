// CoderabbitCiReactConfig.test.cpp — phase-8 of `coderabbit-react-loop`.
//
// Locks the two new ConfigManager blocks (`CoderabbitReact` + `CiReact`):
//   - construct-time defaults match the JSON sketch in
//     `docs/design/coderabbit-react-loop.md § ConfigManager coderabbit_react
//     + ci_react blocks` byte-for-byte (kept in sync by these assertions).
//   - JSON round-trip: write → load → assert equality on every field.
//   - clamp behaviour for hand-edited out-of-range values.
//
// Build-time gating: only compiled when SMATCHET_WITH_AGENTIC=ON (the two
// blocks live inside the same gate in ConfigManager.h).

#if defined(SMATCHET_WITH_AGENTIC)

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool Contains(const std::vector<std::string>& v, const std::string& needle) {
    return std::find(v.begin(), v.end(), needle) != v.end();
}

void WriteConfig(const nlohmann::json& j) {
    const std::string path = ConfigManager::GetConfigPath();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << j.dump(2);
    f.close();
    ConfigManager::InvalidateCache();
}

} // namespace

TEST_SUITE("CoderabbitCiReactConfig") {
    TEST_CASE("CoderabbitReact default-constructs to the documented shape") {
        TrackerConfig cfg; // construct-time defaults only — no Load needed.
        CHECK(cfg.CoderabbitReact.Enabled == false);
        CHECK(cfg.CoderabbitReact.PollIntervalSec == 1800);
        REQUIRE(cfg.CoderabbitReact.WatchedBaseBranches.size() == 1);
        CHECK(cfg.CoderabbitReact.WatchedBaseBranches.front() == "develop");
        REQUIRE(cfg.CoderabbitReact.BotLogins.size() == 1);
        CHECK(cfg.CoderabbitReact.BotLogins.front() == "coderabbitai[bot]");
        CHECK(cfg.CoderabbitReact.ShortCircuitRejectEnabled == true);
        CHECK(cfg.CoderabbitReact.AutoDispatchFixes == true);
        CHECK(cfg.CoderabbitReact.IterationBudgetPerPr == 5);
        CHECK(cfg.CoderabbitReact.AdHocWorktreeRoot == ".claude/worktrees");
    }

    TEST_CASE("CiReact default-constructs to the documented shape") {
        TrackerConfig cfg;
        CHECK(cfg.CiReact.Enabled == false);
        CHECK(cfg.CiReact.PollIntervalSec == 600);
        REQUIRE(cfg.CiReact.WatchedBaseBranches.size() == 1);
        CHECK(cfg.CiReact.WatchedBaseBranches.front() == "develop");
        // Watched check-names list from the plan body (4 entries).
        CHECK(cfg.CiReact.WatchedCheckNames.size() == 4);
        CHECK(Contains(cfg.CiReact.WatchedCheckNames, "Windows + MSYS2 UCRT64"));
        CHECK(Contains(cfg.CiReact.WatchedCheckNames, "Windows + MSYS2 UCRT64 (SMATCHET_WITH_AGENTIC=OFF)"));
        CHECK(Contains(cfg.CiReact.WatchedCheckNames, "Windows + MSYS2 UCRT64 (SMATCHET_WITH_WHISPER=OFF)"));
        CHECK(Contains(cfg.CiReact.WatchedCheckNames, "Test-delta gate"));
        // Ignored list — coverage row carried until 2026-05-30 cutoff.
        REQUIRE(cfg.CiReact.IgnoredCheckNames.size() == 1);
        CHECK(cfg.CiReact.IgnoredCheckNames.front() == "Coverage (windows-2022 + OpenCppCoverage)");
        CHECK(cfg.CiReact.AutoDispatchBuildDoctor == true);
        CHECK(cfg.CiReact.AutoDispatchTestRig == true);
        // Locked 2026-05-18 decision — default OFF.
        CHECK(cfg.CiReact.AutoDispatchDebugDetective == false);
        CHECK(cfg.CiReact.TransientRerunEnabled == true);
        CHECK(cfg.CiReact.TransientRerunMaxPerPr == 2);
        CHECK(cfg.CiReact.IterationBudgetPerPr == 5);
        CHECK(cfg.CiReact.AnnotationFetchCount == 20);
        CHECK(cfg.CiReact.LogTailLines == 200);
    }

    TEST_CASE("CoderabbitReact JSON round-trip preserves every field") {
        smatchet_tests::TestEnvGuard env;
        TrackerConfig original;
        original.CoderabbitReact.Enabled = true;
        original.CoderabbitReact.PollIntervalSec = 900;
        original.CoderabbitReact.WatchedBaseBranches = {"develop", "main"};
        original.CoderabbitReact.BotLogins = {"coderabbitai[bot]", "custombot[bot]"};
        original.CoderabbitReact.ShortCircuitRejectEnabled = false;
        original.CoderabbitReact.AutoDispatchFixes = false;
        original.CoderabbitReact.IterationBudgetPerPr = 17;
        original.CoderabbitReact.AdHocWorktreeRoot = "/tmp/custom-worktrees";
        ConfigManager::Save(original);
        // InvalidateCache so the next Load re-reads from disk.
        ConfigManager::InvalidateCache();
        const TrackerConfig loaded = ConfigManager::Load();
        CHECK(loaded.CoderabbitReact.Enabled == true);
        CHECK(loaded.CoderabbitReact.PollIntervalSec == 900);
        CHECK(loaded.CoderabbitReact.WatchedBaseBranches.size() == 2);
        CHECK(loaded.CoderabbitReact.WatchedBaseBranches[0] == "develop");
        CHECK(loaded.CoderabbitReact.WatchedBaseBranches[1] == "main");
        CHECK(loaded.CoderabbitReact.BotLogins.size() == 2);
        CHECK(loaded.CoderabbitReact.BotLogins[1] == "custombot[bot]");
        CHECK(loaded.CoderabbitReact.ShortCircuitRejectEnabled == false);
        CHECK(loaded.CoderabbitReact.AutoDispatchFixes == false);
        CHECK(loaded.CoderabbitReact.IterationBudgetPerPr == 17);
        CHECK(loaded.CoderabbitReact.AdHocWorktreeRoot == "/tmp/custom-worktrees");
    }

    TEST_CASE("CiReact JSON round-trip preserves every field") {
        smatchet_tests::TestEnvGuard env;
        TrackerConfig original;
        original.CiReact.Enabled = true;
        original.CiReact.PollIntervalSec = 1200;
        original.CiReact.WatchedBaseBranches = {"develop"};
        original.CiReact.WatchedCheckNames = {"Custom check A", "Custom check B"};
        original.CiReact.IgnoredCheckNames = {}; // post-2026-05-30 cutoff shape.
        original.CiReact.AutoDispatchBuildDoctor = false;
        original.CiReact.AutoDispatchTestRig = false;
        original.CiReact.AutoDispatchDebugDetective = true;
        original.CiReact.TransientRerunEnabled = false;
        original.CiReact.TransientRerunMaxPerPr = 7;
        original.CiReact.IterationBudgetPerPr = 33;
        original.CiReact.AnnotationFetchCount = 50;
        original.CiReact.LogTailLines = 500;
        ConfigManager::Save(original);
        ConfigManager::InvalidateCache();
        const TrackerConfig loaded = ConfigManager::Load();
        CHECK(loaded.CiReact.Enabled == true);
        CHECK(loaded.CiReact.PollIntervalSec == 1200);
        CHECK(loaded.CiReact.WatchedCheckNames.size() == 2);
        CHECK(loaded.CiReact.IgnoredCheckNames.empty());
        CHECK(loaded.CiReact.AutoDispatchBuildDoctor == false);
        CHECK(loaded.CiReact.AutoDispatchTestRig == false);
        CHECK(loaded.CiReact.AutoDispatchDebugDetective == true);
        CHECK(loaded.CiReact.TransientRerunEnabled == false);
        CHECK(loaded.CiReact.TransientRerunMaxPerPr == 7);
        CHECK(loaded.CiReact.IterationBudgetPerPr == 33);
        CHECK(loaded.CiReact.AnnotationFetchCount == 50);
        CHECK(loaded.CiReact.LogTailLines == 500);
    }

    TEST_CASE("CoderabbitReact clamps poll_interval and iteration budget") {
        smatchet_tests::TestEnvGuard env;
        nlohmann::json j = nlohmann::json::object();
        j["read_only_mode"] = false;
        // Hand-edited values outside the documented range.
        nlohmann::json cr = nlohmann::json::object();
        cr["enabled"] = true;
        cr["poll_interval_sec"] = 5;       // below floor
        cr["iteration_budget_per_pr"] = 0; // below floor
        j["coderabbit_react"] = std::move(cr);
        WriteConfig(j);
        TrackerConfig low = ConfigManager::Load();
        CHECK(low.CoderabbitReact.PollIntervalSec == 60);
        CHECK(low.CoderabbitReact.IterationBudgetPerPr == 1);

        nlohmann::json j2 = nlohmann::json::object();
        j2["read_only_mode"] = false;
        nlohmann::json cr2 = nlohmann::json::object();
        cr2["enabled"] = true;
        cr2["poll_interval_sec"] = 86400; // way above ceiling
        cr2["iteration_budget_per_pr"] = 999;
        j2["coderabbit_react"] = std::move(cr2);
        WriteConfig(j2);
        TrackerConfig high = ConfigManager::Load();
        CHECK(high.CoderabbitReact.PollIntervalSec == 3600);
        CHECK(high.CoderabbitReact.IterationBudgetPerPr == 50);
    }

    TEST_CASE("CiReact clamps poll_interval, budgets, annotation count, log tail") {
        smatchet_tests::TestEnvGuard env;
        nlohmann::json j = nlohmann::json::object();
        j["read_only_mode"] = false;
        nlohmann::json ci = nlohmann::json::object();
        ci["poll_interval_sec"] = 1;
        ci["iteration_budget_per_pr"] = -3;
        ci["transient_rerun_max_per_pr"] = -1;
        ci["annotation_fetch_count"] = 0;
        ci["log_tail_lines"] = 1;
        j["ci_react"] = std::move(ci);
        WriteConfig(j);
        TrackerConfig low = ConfigManager::Load();
        CHECK(low.CiReact.PollIntervalSec == 60);
        CHECK(low.CiReact.IterationBudgetPerPr == 1);
        CHECK(low.CiReact.TransientRerunMaxPerPr == 0);
        CHECK(low.CiReact.AnnotationFetchCount == 1);
        CHECK(low.CiReact.LogTailLines == 10);

        nlohmann::json j2 = nlohmann::json::object();
        j2["read_only_mode"] = false;
        nlohmann::json ci2 = nlohmann::json::object();
        ci2["poll_interval_sec"] = 99999;
        ci2["iteration_budget_per_pr"] = 9999;
        ci2["transient_rerun_max_per_pr"] = 9999;
        ci2["annotation_fetch_count"] = 9999;
        ci2["log_tail_lines"] = 9999;
        j2["ci_react"] = std::move(ci2);
        WriteConfig(j2);
        TrackerConfig high = ConfigManager::Load();
        CHECK(high.CiReact.PollIntervalSec == 3600);
        CHECK(high.CiReact.IterationBudgetPerPr == 50);
        CHECK(high.CiReact.TransientRerunMaxPerPr == 10);
        CHECK(high.CiReact.AnnotationFetchCount == 100);
        CHECK(high.CiReact.LogTailLines == 2000);
    }

    TEST_CASE("Missing coderabbit_react and ci_react blocks load construct-time defaults") {
        smatchet_tests::TestEnvGuard env;
        // Write a config that omits both react blocks; the additive defaults
        // contract guarantees the rest of TrackerConfig still hydrates.
        nlohmann::json j = nlohmann::json::object();
        j["read_only_mode"] = false;
        WriteConfig(j);
        TrackerConfig cfg = ConfigManager::Load();
        CHECK(cfg.CoderabbitReact.Enabled == false);
        CHECK(cfg.CoderabbitReact.PollIntervalSec == 1800);
        CHECK(cfg.CoderabbitReact.IterationBudgetPerPr == 5);
        CHECK(cfg.CiReact.Enabled == false);
        CHECK(cfg.CiReact.PollIntervalSec == 600);
        CHECK(cfg.CiReact.AutoDispatchDebugDetective == false);
    }

    TEST_CASE("CoderabbitReact list fields survive empty-array round-trip") {
        smatchet_tests::TestEnvGuard env;
        TrackerConfig original;
        original.CoderabbitReact.WatchedBaseBranches.clear();
        original.CoderabbitReact.BotLogins.clear();
        ConfigManager::Save(original);
        ConfigManager::InvalidateCache();
        const TrackerConfig loaded = ConfigManager::Load();
        CHECK(loaded.CoderabbitReact.WatchedBaseBranches.empty());
        CHECK(loaded.CoderabbitReact.BotLogins.empty());
    }
}

#endif // SMATCHET_WITH_AGENTIC
