// CrashSink doctest — marker round-trip + consume semantics.
// docs/plans/shipped/log-a-bug-github.md § Phase 2.

#include "CrashSink.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <cstdint>
#include <ctime>
#include <fstream>
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

TEST_CASE("CrashSink — log-tail: stashes the crashed session's log path across launches") {
    const std::string dir = MakeTempDir();
    // Write a fake "crashed session" log next to the crash dir.
    const std::string logPath = (fs::path(dir) / "session.log").string();
    {
        std::ofstream f(logPath);
        for (int i = 0; i < 5; ++i) {
            f << "[INFO] line " << i << " token=supersecretvalue123longenoughtotriprule\n";
        }
    }
    // Session A: records its log path, then "crashes" (writes a marker).
    CrashSinkInit(dir, logPath);
    CrashSinkWriteMarkerAsyncSafe("SIGSEGV (segfault)");

    // Session B (next launch): Init stashes the previous log path BEFORE overwriting,
    // because a marker is pending. Consume returns a redacted tail of the crashed log.
    CrashSinkInit(dir, (fs::path(dir) / "session_B.log").string());
    const CrashInfo info = CrashSinkConsume();
    REQUIRE(info.Pending);
    CHECK(info.LogTail.find("line 4") != std::string::npos);              // crashed log content present
    CHECK(info.LogTail.find("supersecretvalue123") == std::string::npos); // redacted
}

TEST_CASE("CrashSink — append-breadcrumb-line keeps the activity breadcrumb and adds the terminate reason") {
    const std::string dir = MakeTempDir();
    CrashSinkInit(dir);
    CrashSinkBreadcrumb("editing grid");

    // #987: terminate handler appends the active exception's what() — the
    // normal activity breadcrumb must survive above it, newline-separated.
    CrashSinkAppendBreadcrumbLine("terminate: ", "system_error: thread ctor failed");
    CrashSinkWriteMarkerAsyncSafe("std::terminate (unhandled C++ exception)");

    const CrashInfo info = CrashSinkConsume();
    REQUIRE(info.Pending);
    CHECK(info.Breadcrumb == "editing grid\nterminate: system_error: thread ctor failed");
}

TEST_CASE("CrashSink — append-breadcrumb-line tolerates null prefix/text") {
    const std::string dir = MakeTempDir();
    CrashSinkInit(dir);
    CrashSinkBreadcrumb("idle");
    CrashSinkAppendBreadcrumbLine(nullptr, "only text");
    CrashSinkAppendBreadcrumbLine("only prefix: ", nullptr);
    CrashSinkWriteMarkerAsyncSafe("std::terminate (unhandled C++ exception)");
    const CrashInfo info = CrashSinkConsume();
    REQUIRE(info.Pending);
    CHECK(info.Breadcrumb == "idle\nonly text\nonly prefix: ");
}

TEST_CASE("CrashSink — pending dump path is non-empty after Init") {
    const std::string dir = MakeTempDir();
    CrashSinkInit(dir);
    const char* p = CrashSinkPendingDumpPath();
    REQUIRE(p != nullptr);
    CHECK(std::string(p).find("pending_crash.dmp") != std::string::npos);
}
