// FileIo::ScopedFileLock — the cross-process advisory lock hoisted out of
// ConfigManager_Internal.h into the layer-0 FileIo module (BACKLOG_CODE_REVIEW.md C5), plus the
// one new consumer that motivated the hoist: the BackendAuditTrail append path.
//
// Every case drives the PRODUCTION symbol (smatchet::fileio::ScopedFileLock from
// Source/Core/src/FileIo.cpp, and BackendAuditTrail::AppendEvent) — there is no local
// re-implementation of flock/LockFileEx here.
//
// Platform note: the blocking-ordering assertions are POSIX-only. The Win32 LockFileEx branch is
// structurally parallel but is not exercised by the Linux lanes these tests run on.

#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "FileIo.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using smatchet::fileio::ScopedFileLock;

namespace {

bool PathExists(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return f.is_open();
}

std::string UniqueSuffix() {
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return std::to_string(ns);
}

// A path in the test's working directory (ctest runs in a writable build dir), matching the
// convention already used by Core/ConfigAtomicWriteSecurity.test.cpp.
std::string TempTarget(const char* tag) { return std::string("./fileiolock_") + tag + "_" + UniqueSuffix() + ".dat"; }

bool MakeDir(const std::string& path) {
#if defined(_WIN32)
    return ::_mkdir(path.c_str()) == 0;
#else
    return ::mkdir(path.c_str(), 0755) == 0;
#endif
}

} // namespace

TEST_CASE("ScopedFileLock: locks a '.lock' sidecar rather than the target file itself") {
    const std::string target = TempTarget("sidecar");
    const std::string sidecar = target + ".lock";
    std::remove(sidecar.c_str());

    {
        ScopedFileLock lock(target);
        CHECK(lock.LockPath() == sidecar);
        // The sidecar is what gets created; the target is untouched, which is exactly what lets
        // the lock survive an atomic temp-then-rename replace of the target.
        CHECK(PathExists(sidecar));
        CHECK_FALSE(PathExists(target));
#if !defined(_WIN32)
        CHECK(lock.Held());
#endif
    }

    std::remove(sidecar.c_str());
}

#if !defined(_WIN32)
TEST_CASE("ScopedFileLock: a second holder blocks until the first is destroyed") {
    const std::string target = TempTarget("serialise");
    const std::string sidecar = target + ".lock";
    std::remove(sidecar.c_str());

    // 0 while the first lock is held, 1 once it has been released. The second holder samples it
    // at the instant it acquires, so a sample of 1 proves it waited rather than acquiring in
    // parallel. flock(2) is per-open-file-description, so two ScopedFileLock objects contend even
    // inside a single process.
    std::atomic<int> phase{0};
    std::atomic<int> observedPhase{-1};

    std::future<void> second;
    {
        ScopedFileLock first(target);
        REQUIRE(first.Held());

        second = std::async(std::launch::async, [&] {
            ScopedFileLock lock(target);
            observedPhase.store(phase.load());
        });

        // Give the async holder time to reach the blocking flock() call, then release.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(observedPhase.load() == -1); // still blocked while the first holder lives.
        phase.store(1);
    } // `first` released here.

    REQUIRE(second.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    second.get();
    CHECK(observedPhase.load() == 1);

    std::remove(sidecar.c_str());
}

TEST_CASE("ScopedFileLock: releases on destruction so the next holder acquires immediately") {
    const std::string target = TempTarget("release");
    const std::string sidecar = target + ".lock";
    std::remove(sidecar.c_str());

    {
        ScopedFileLock first(target);
        REQUIRE(first.Held());
    }

    // With the first holder gone this must not block. Run it on a worker with a timeout so a
    // regression that leaks the descriptor fails the test instead of hanging the suite.
    std::future<bool> reacquired = std::async(std::launch::async, [&] {
        ScopedFileLock lock(target);
        return lock.Held();
    });
    REQUIRE(reacquired.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(reacquired.get());

    std::remove(sidecar.c_str());
}
#endif // !_WIN32

TEST_CASE("BackendAuditTrail: the append path takes the FileIo sidecar lock") {
    // The C5 payoff: Persistence/ now guards its append with the same primitive the config
    // writers use. Observable proof is the '<audit file>.lock' sidecar appearing next to the
    // audit file once the async writer has flushed a line.
#if defined(_WIN32)
    const char* envTmp = std::getenv("TEMP");
    if (!envTmp)
        envTmp = "C:\\Windows\\Temp";
    const char sep = '\\';
#else
    const char* envTmp = std::getenv("TMPDIR");
    if (!envTmp)
        envTmp = "/tmp";
    const char sep = '/';
#endif
    const std::string dir = std::string(envTmp) + sep + "smatchet_fileiolock_" + UniqueSuffix();
    REQUIRE(MakeDir(dir));
    ConfigManager::SetUserDataDirectory(dir + sep);

    const std::string auditPath = BackendAuditTrail::GetAuditFilePath();
    const std::string sidecar = auditPath + ".lock";
    const std::string operationId = BackendAuditTrail::MakeOperationId("fileiolock");

    BackendAuditTrail::AppendResult("update", "test", "SMAT-1", operationId, true, "");

    // The writer is an async process-wide singleton; poll for the flush like the sibling
    // BackendAuditTrail tests do.
    bool sawLine = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream f(auditPath.c_str(), std::ios::binary);
        std::string l;
        while (f.is_open() && std::getline(f, l)) {
            if (l.find(operationId) != std::string::npos)
                sawLine = true;
        }
        if (sawLine)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CHECK(sawLine);
    CHECK(PathExists(sidecar));

    ConfigManager::SetUserDataDirectory("");
    std::remove(sidecar.c_str());
    std::remove(auditPath.c_str());
    std::remove(dir.c_str());
}
