// WhisperPrefsFields — Phase F doctest coverage for the four new
// ConfigManager fields (WhisperLanguage, WhisperTrim, WhisperMaxClipSec,
// WhisperAutoSendOnPunctuation). Mirrors the existing ConfigManager.test.cpp
// shape: drive ConfigManager::Save → InvalidateCache → Load against a
// redirected user-data directory and assert the values round-trip plus
// fresh-install defaults survive.
//
// Schema-bump discipline: all four fields are additive (no schema_version
// bump in Phase F). Tests therefore also check that a config file written
// without the new keys still loads with the documented defaults.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <string>

#if defined(SMATCHET_WITH_WHISPER)

TEST_CASE("WhisperPrefsFields fresh-install defaults match the documented values") {
    smatchet_tests::TestEnvGuard env;
    ConfigManager::InvalidateCache();
    const TrackerConfig cfg = ConfigManager::Load();
    CHECK(cfg.WhisperLanguage == "en");
    CHECK(cfg.WhisperTrim == true);
    CHECK(cfg.WhisperMaxClipSec == 60);
    CHECK(cfg.WhisperAutoSendOnPunctuation == false);
}

TEST_CASE("WhisperPrefsFields round-trip through Save -> Load") {
    smatchet_tests::TestEnvGuard env;
    ConfigManager::InvalidateCache();
    TrackerConfig cfg = ConfigManager::Load();
    // Mutate every Phase F field to a non-default value.
    cfg.WhisperLanguage = "fr";
    cfg.WhisperTrim = false;
    cfg.WhisperMaxClipSec = 120;
    cfg.WhisperAutoSendOnPunctuation = true;
    ConfigManager::Save(cfg);
    ConfigManager::InvalidateCache();
    const TrackerConfig loaded = ConfigManager::Load();
    CHECK(loaded.WhisperLanguage == "fr");
    CHECK(loaded.WhisperTrim == false);
    CHECK(loaded.WhisperMaxClipSec == 120);
    CHECK(loaded.WhisperAutoSendOnPunctuation == true);
}

TEST_CASE("WhisperMaxClipSec clamps negative and > 600 inputs on load") {
    smatchet_tests::TestEnvGuard env;
    const std::string cfgPath = ConfigManager::GetConfigPath();
    // Hand-write a config with out-of-range values; load should clamp them.
    {
        std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
        f << "{\"whisper_max_clip_sec\":-5}";
    }
    ConfigManager::InvalidateCache();
    const TrackerConfig clampedLow = ConfigManager::Load();
    CHECK(clampedLow.WhisperMaxClipSec == 0); // negative -> 0 (disabled cap).

    {
        std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
        f << "{\"whisper_max_clip_sec\":9999}";
    }
    ConfigManager::InvalidateCache();
    const TrackerConfig clampedHigh = ConfigManager::Load();
    CHECK(clampedHigh.WhisperMaxClipSec == 600); // > 600 -> 600 (10 min cap).

    std::remove(cfgPath.c_str());
    ConfigManager::InvalidateCache();
}

TEST_CASE("WhisperPrefsFields missing keys preserve struct defaults on load") {
    smatchet_tests::TestEnvGuard env;
    const std::string cfgPath = ConfigManager::GetConfigPath();
    // Write a minimal config that omits all Phase F keys.
    {
        std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
        f << "{\"read_only_mode\":false}";
    }
    ConfigManager::InvalidateCache();
    const TrackerConfig cfg = ConfigManager::Load();
    // All Phase F fields should come from the in-struct defaults.
    CHECK(cfg.WhisperLanguage == "en");
    CHECK(cfg.WhisperTrim == true);
    CHECK(cfg.WhisperMaxClipSec == 60);
    CHECK(cfg.WhisperAutoSendOnPunctuation == false);

    std::remove(cfgPath.c_str());
    ConfigManager::InvalidateCache();
}

#endif // SMATCHET_WITH_WHISPER
