#ifndef SMATCHET_TESTS_OFFLINE_QUEUE_TEST_ENV_H
#define SMATCHET_TESTS_OFFLINE_QUEUE_TEST_ENV_H

// OfflineQueueTestEnvGuard — shared per-test environment guard for TUs that drive
// OfflineQueueService (extracted verbatim from OfflineQueueServiceRuntime.test.cpp for
// multi-grid Slice 1c so the backend-key replay tests reuse it instead of cloning). Each
// instance:
//   * creates a unique temp dir,
//   * points `ConfigManager::SetUserDataDirectory()` at it (so audit + config land there),
//   * writes `smatchet_config.json` with `read_only_mode=false` (when ConfigManager loads
//     against a missing/empty config file it defaults `ReadOnlyMode=true`, blocking every
//     queue mutation under test).
// All state is process-wide; doctest's single-threaded runner prevents races.

#include "BackendAuditTrail.h"
#include "ConfigManager.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace smatchet_tests {

class OfflineQueueTestEnvGuard {
  public:
    OfflineQueueTestEnvGuard() {
        const char* envTmp = nullptr;
#if defined(_WIN32)
        envTmp = std::getenv("TEMP");
        if (!envTmp)
            envTmp = std::getenv("TMP");
        if (!envTmp)
            envTmp = "C:\\Windows\\Temp";
        const char sep = '\\';
        const auto unique =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        dir_ = std::string(envTmp) + sep + "smatchet_offqueue_test_" + std::to_string(unique);
        ::_mkdir(dir_.c_str());
#else
        envTmp = std::getenv("TMPDIR");
        if (!envTmp)
            envTmp = "/tmp";
        const char sep = '/';
        const auto unique =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        dir_ = std::string(envTmp) + sep + "smatchet_offqueue_test_" + std::to_string(unique);
        ::mkdir(dir_.c_str(), 0755);
#endif
        dirWithSep_ = dir_;
        if (dirWithSep_.back() != sep)
            dirWithSep_.push_back(sep);
        ConfigManager::SetUserDataDirectory(dirWithSep_);
        // Drop a minimal config so ConfigManager::Load() sees `read_only_mode=false`.
        const std::string cfgPath = dirWithSep_ + "smatchet_config.json";
        std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
        f << "{\"read_only_mode\":false}";
        f.close();
        ConfigManager::InvalidateCache();
    }
    ~OfflineQueueTestEnvGuard() {
        std::remove(BackendAuditTrail::GetAuditFilePath().c_str());
        std::remove((dirWithSep_ + "smatchet_config.json").c_str());
        ConfigManager::SetUserDataDirectory("");
        ConfigManager::InvalidateCache();
        std::remove(dir_.c_str());
    }
    OfflineQueueTestEnvGuard(const OfflineQueueTestEnvGuard&) = delete;
    OfflineQueueTestEnvGuard& operator=(const OfflineQueueTestEnvGuard&) = delete;

  private:
    std::string dir_;
    std::string dirWithSep_;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_OFFLINE_QUEUE_TEST_ENV_H
