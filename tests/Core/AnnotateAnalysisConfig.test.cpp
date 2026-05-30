// AnnotateAnalysisConfig tests — Phase 2 (Annotate UI preferences cleanup).
//
// Cover the behavioural changes in ConfigManager::Load/SaveAnnotateAnalysis that
// the prefs cleanup introduced:
//   - cl_cache_max clamp on load into [16, 8192] (value-range guard, item 6).
//   - show_raw_callstack round-trip (raw/table toggle persistence, item 3).
//   - multi-rule path_remaps round-trip, in order (item 4).
//
// These exercise the on-disk persistence seam only (no UI), driven through
// TestEnvGuard which redirects ConfigManager at a private temp config dir.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <fstream>
#include <string>

namespace {

// Drop `contents` at <guarded dir>/smatchet_config.json and force the next load
// to re-read from disk. Mirrors the helper in ConfigMigration.test.cpp.
void WriteConfigRaw(const smatchet_tests::TestEnvGuard& env, const std::string& contents) {
    const std::string path = env.DirWithSep() + "smatchet_config.json";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!contents.empty()) {
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    f.close();
    ConfigManager::InvalidateCache();
}

} // namespace

TEST_CASE("LoadAnnotateAnalysis clamps cl_cache_max into [16,8192]") {
    smatchet_tests::TestEnvGuard env;

    SUBCASE("negative clamps up to 16") {
        WriteConfigRaw(env, R"({"annotate_analysis":{"cl_cache_max":-5}})");
        CHECK(ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries == 16);
    }
    SUBCASE("zero clamps up to 16") {
        WriteConfigRaw(env, R"({"annotate_analysis":{"cl_cache_max":0}})");
        CHECK(ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries == 16);
    }
    SUBCASE("over-max clamps down to 8192") {
        WriteConfigRaw(env, R"({"annotate_analysis":{"cl_cache_max":100000}})");
        CHECK(ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries == 8192);
    }
    SUBCASE("in-range value preserved") {
        WriteConfigRaw(env, R"({"annotate_analysis":{"cl_cache_max":512}})");
        CHECK(ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries == 512);
    }
}

TEST_CASE("AnnotateAnalysis show_raw_callstack round-trips") {
    smatchet_tests::TestEnvGuard env;

    WriteConfigRaw(env, R"({"annotate_analysis":{"show_raw_callstack":true}})");
    CHECK(ConfigManager::LoadAnnotateAnalysis().ShowRawCallstack == true);

    // Absent key falls back to the default (false).
    WriteConfigRaw(env, R"({"annotate_analysis":{}})");
    CHECK(ConfigManager::LoadAnnotateAnalysis().ShowRawCallstack == false);

    // Save → Load preserves the flag.
    AnnotateAnalysisConfig cfg = ConfigManager::LoadAnnotateAnalysis();
    cfg.ShowRawCallstack = true;
    ConfigManager::SaveAnnotateAnalysis(cfg);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::LoadAnnotateAnalysis().ShowRawCallstack == true);
}

TEST_CASE("AnnotateAnalysis multi-rule path_remaps round-trip in order") {
    smatchet_tests::TestEnvGuard env;

    WriteConfigRaw(env, R"({"annotate_analysis":{"path_remaps":[
        {"from":"//depot/a","to":"C:/a"},
        {"from":"//depot/longer/b","to":"C:/b"}
    ]}})");
    const AnnotateAnalysisConfig cfg = ConfigManager::LoadAnnotateAnalysis();
    REQUIRE(cfg.PathRemaps.size() == 2);
    CHECK(cfg.PathRemaps[0].FromPrefix == "//depot/a");
    CHECK(cfg.PathRemaps[0].ToPrefix == "C:/a");
    CHECK(cfg.PathRemaps[1].FromPrefix == "//depot/longer/b");
    CHECK(cfg.PathRemaps[1].ToPrefix == "C:/b");

    // A rule with an empty "from" prefix is dropped on load (matches loader guard).
    WriteConfigRaw(env, R"({"annotate_analysis":{"path_remaps":[{"from":"","to":"C:/x"}]}})");
    CHECK(ConfigManager::LoadAnnotateAnalysis().PathRemaps.empty());

    // Save → Load preserves a multi-rule vector and its order.
    AnnotateAnalysisConfig out;
    out.PathRemaps.push_back(PathRemapRule{"//depot/a", "C:/a"});
    out.PathRemaps.push_back(PathRemapRule{"//depot/b", "C:/b"});
    out.PathRemaps.push_back(PathRemapRule{"//depot/c", "C:/c"});
    ConfigManager::SaveAnnotateAnalysis(out);
    ConfigManager::InvalidateCache();
    const AnnotateAnalysisConfig reread = ConfigManager::LoadAnnotateAnalysis();
    REQUIRE(reread.PathRemaps.size() == 3);
    CHECK(reread.PathRemaps[0].FromPrefix == "//depot/a");
    CHECK(reread.PathRemaps[2].FromPrefix == "//depot/c");
    CHECK(reread.PathRemaps[2].ToPrefix == "C:/c");
}
