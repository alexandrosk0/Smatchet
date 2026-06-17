// TestEnvGuard — RAII helper that gives each doctest TEST_CASE a private user-data
// directory and a minimal `smatchet_config.json` (with `read_only_mode=false`).
//
// Why: `ConfigManager` is a process-wide singleton; runtime callers (audit writer,
// offline queue, sync service, annotate analysis, etc.) all read paths through it.
// Without redirection, parallel test runs would share the user's real config /
// audit files. Doctest is single-threaded by default so per-TEST_CASE state still
// works, but we must isolate dirs so adjacent cases never poison each other.
//
// Also: when no on-disk `smatchet_config.json` exists, `ConfigManager::Load()`
// defaults `ReadOnlyMode=true` to protect first-launch users from accidental
// writes. Tests that exercise queue mutations or sync writes need that flipped —
// the guard writes a minimal config with `read_only_mode=false` so callers see
// the same "configured user" state they would in production.
//
// Per-case usage:
//
//     TEST_CASE("something") {
//         smatchet_tests::TestEnvGuard env;     // dir created + ConfigManager redirected
//         // ... exercise code that calls ConfigManager::GetConfigPath() etc.
//     }                                          // dir + config removed on ~TestEnvGuard
//
// Header-only so the test target picks it up via `target_include_directories(...
// tests/support)`. C++14-compliant — no external deps beyond stdlib + ConfigManager.h.

#ifndef SMATCHET_TESTS_SUPPORT_TEST_ENV_GUARD_H
#define SMATCHET_TESTS_SUPPORT_TEST_ENV_GUARD_H

#include "ConfigManager.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h> // ::mkdir
#include <unistd.h>   // ::rmdir
#endif

namespace smatchet_tests {

class TestEnvGuard {
  public:
    TestEnvGuard() {
        const char* envTmp = nullptr;
#if defined(_WIN32)
        envTmp = std::getenv("TEMP");
        if (!envTmp)
            envTmp = std::getenv("TMP");
        if (!envTmp)
            envTmp = "C:\\Windows\\Temp";
        const char sep = '\\';
#else
        envTmp = std::getenv("TMPDIR");
        if (!envTmp)
            envTmp = "/tmp";
        const char sep = '/';
#endif
        const auto unique =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        dir_ = std::string(envTmp) + sep + "smatchet_env_test_" + std::to_string(unique);
#if defined(_WIN32)
        ::_mkdir(dir_.c_str());
#else
        ::mkdir(dir_.c_str(), 0755);
#endif
        dirWithSep_ = dir_;
        if (dirWithSep_.back() != sep)
            dirWithSep_.push_back(sep);

        ConfigManager::SetUserDataDirectory(dirWithSep_);
        // Minimal config so ConfigManager::Load() sees `read_only_mode=false`. The default
        // when no config file exists is `true` (first-launch protection), which blocks
        // queue mutations and other state changes that most tests want to exercise.
        const std::string cfgPath = dirWithSep_ + "smatchet_config.json";
        std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
        f << "{\"read_only_mode\":false}";
        f.close();
        ConfigManager::InvalidateCache();
    }

    ~TestEnvGuard() {
        // Best-effort cleanup. Order matters: clear the cache first so any subsequent
        // ConfigManager::Load() in the next case re-reads from disk rather than returning
        // our values.
        std::remove((dirWithSep_ + "smatchet_config.json").c_str());
        ConfigManager::SetUserDataDirectory("");
        ConfigManager::InvalidateCache();
#if defined(_WIN32)
        ::_rmdir(dir_.c_str());
#else
        ::rmdir(dir_.c_str());
#endif
    }

    TestEnvGuard(const TestEnvGuard&) = delete;
    TestEnvGuard& operator=(const TestEnvGuard&) = delete;

    const std::string& Dir() const { return dir_; }
    const std::string& DirWithSep() const { return dirWithSep_; }

  private:
    std::string dir_;
    std::string dirWithSep_;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_SUPPORT_TEST_ENV_GUARD_H
