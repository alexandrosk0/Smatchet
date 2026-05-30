// CrashSink doctest — marker round-trip + consume semantics.
// docs/plans/active/log-a-bug-github.md § Phase 2.

#include "CrashSink.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <cstdint>
#include <ctime>
#include <string>

namespace fs = ghc::filesystem;
using namespace smatchet::diagnostics;

namespace {
std::string MakeTempDir() {
    const fs::path dir = fs::temp_directory_path() / ("smatchet_crashsink_" + std::to_string(::time(nullptr)) + "_" +
                                                      std::to_string(reinterpret_cast<std::uintptr_t>(&dir)));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}
} // namespace

TEST_CASE("CrashSink — no marker means nothing pending") {
    const std::string dir = MakeTempDir();
    CrashSinkInit(dir);
    CHECK_FALSE(CrashSinkHasPending());
    const CrashInfo none = CrashSinkConsume();
    CHECK_FALSE(none.Pending);
}

TEST_CASE("CrashSink — write marker -> pending -> consume -> cleared") {
    const std::string dir = MakeTempDir();
    CrashSinkInit(dir);
    CrashSinkBreadcrumb("editing grid");

    CrashSinkWriteMarkerAsyncSafe("SIGSEGV (segfault)");
    CHECK(CrashSinkHasPending());

    const CrashInfo info = CrashSinkConsume();
    CHECK(info.Pending);
    CHECK(info.Reason == "SIGSEGV (segfault)");
    CHECK(info.Breadcrumb == "editing grid");

    // Consuming deletes the marker — never loop on the same crash.
    CHECK_FALSE(CrashSinkHasPending());
    CHECK_FALSE(CrashSinkConsume().Pending);
}

TEST_CASE("CrashSink — pending dump path is non-empty after Init") {
    const std::string dir = MakeTempDir();
    CrashSinkInit(dir);
    const char* p = CrashSinkPendingDumpPath();
    REQUIRE(p != nullptr);
    CHECK(std::string(p).find("pending_crash.dmp") != std::string::npos);
}
