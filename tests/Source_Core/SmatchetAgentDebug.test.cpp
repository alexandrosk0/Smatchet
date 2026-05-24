// Slice 7 of docs/design/autonomous-debugging-no-creds.md — doctest cover
// for the production-resident NDJSON helper at tests/_debug/SmatchetAgentDebug.h.
//
// Cases V7.1..V7.5 from the plan's verification table. V7.6 (hot-path budget
// probe) deferred to a follow-up perf-detective PR per the slice charter.

// Force SMATCHET_AGENT_DEBUG ON in this TU so the macro expands to a real
// Emit() call regardless of preset. The header's compile-out path is exercised
// implicitly elsewhere (any TU without the define gets `((void)0)`).
#ifndef SMATCHET_AGENT_DEBUG
#define SMATCHET_AGENT_DEBUG 1
#endif

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../_debug/SmatchetAgentDebug.h"

namespace {

#if defined(_WIN32)
const char kSep = '\\';
#else
const char kSep = '/';
#endif

std::string ScratchDir() {
    // SMATCHET_TESTS_REPO_ROOT is exported by tests/CMakeLists.txt.
    std::string root = SMATCHET_TESTS_REPO_ROOT;
    std::string dir = root + kSep + "tests" + kSep + "_debug" + kSep + "scratch";
#if defined(_WIN32)
    std::string mkcmd = "if not exist \"" + dir + "\" mkdir \"" + dir + "\"";
#else
    std::string mkcmd = "mkdir -p \"" + dir + "\"";
#endif
    (void)std::system(mkcmd.c_str());
    return dir;
}

std::string ScratchPath(const char* name) {
    return ScratchDir() + kSep + name + ".ndjson";
}

std::vector<std::string> ReadLines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path.c_str(), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
    }
    return out;
}

std::size_t FileBytes(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
    if (!f) {
        return 0;
    }
    return static_cast<std::size_t>(f.tellg());
}

}  // namespace

TEST_CASE("V7.1 — per-line schema validation") {
    const std::string path = ScratchPath("v71_schema");
    smatchet_agent_debug::ResetForTest();
    smatchet_agent_debug::SetTargetPath(path, /*fsync_enabled=*/false);

    nlohmann::json payload;
    payload["method"] = "GET";
    payload["url"] = "https://example.test/api/v1/foo";
    payload["status"] = 200;
    SMATCHET_AGENT_DEBUG_LOG("backend-call", payload);

    smatchet_agent_debug::ResetForTest();

    const std::vector<std::string> lines = ReadLines(path);
    REQUIRE(lines.size() == 1);

    nlohmann::json parsed = nlohmann::json::parse(lines[0]);
    CHECK(parsed.contains("ts"));
    CHECK(parsed["ts"].is_string());
    CHECK(parsed.contains("category"));
    CHECK(parsed["category"].get<std::string>() == "backend-call");
    CHECK(parsed.contains("pid"));
    CHECK(parsed["pid"].is_number_integer());
    CHECK(parsed.contains("tid"));
    CHECK(parsed["tid"].is_string());
    CHECK(parsed.contains("scope"));
    CHECK(parsed["scope"].is_string());
    CHECK(parsed.contains("payload"));
    CHECK(parsed["payload"]["method"].get<std::string>() == "GET");
    CHECK(parsed["payload"]["status"].get<int>() == 200);
}

TEST_CASE("V7.2 — closed-set category enum validation") {
    // In-set: succeeds.
    CHECK(smatchet_agent_debug::IsValidCategory("backend-call"));
    CHECK(smatchet_agent_debug::IsValidCategory("ui-event"));
    CHECK(smatchet_agent_debug::IsValidCategory("worker-handoff"));
    CHECK(smatchet_agent_debug::IsValidCategory("lock-claim"));
    CHECK(smatchet_agent_debug::IsValidCategory("scenario-phase"));
    CHECK(smatchet_agent_debug::IsValidCategory("cli-command"));
    CHECK(smatchet_agent_debug::IsValidCategory("temp-debug"));

    // Out-of-set: rejected (runtime check — compile-time enum-class would be
    // stricter but would force callers to qualify every literal, hurting
    // grep-ability of the category strings in production code).
    CHECK_FALSE(smatchet_agent_debug::IsValidCategory("not-a-category"));
    CHECK_FALSE(smatchet_agent_debug::IsValidCategory(""));
    CHECK_FALSE(smatchet_agent_debug::IsValidCategory(nullptr));

    // Out-of-set Emit() writes nothing to the file.
    const std::string path = ScratchPath("v72_enum");
    smatchet_agent_debug::ResetForTest();
    smatchet_agent_debug::SetTargetPath(path, /*fsync_enabled=*/false);
    nlohmann::json payload;
    payload["msg"] = "bad";
    smatchet_agent_debug::Emit("not-a-category", __FILE__, __LINE__, payload);
    smatchet_agent_debug::ResetForTest();
    CHECK(FileBytes(path) == 0u);
}

TEST_CASE("V7.3 — empty-file semantic (zero calls = empty file, not 0-byte JSON object)") {
    const std::string path = ScratchPath("v73_empty");
    smatchet_agent_debug::ResetForTest();
    smatchet_agent_debug::SetTargetPath(path, /*fsync_enabled=*/false);
    // Zero SMATCHET_AGENT_DEBUG_LOG calls.
    smatchet_agent_debug::ResetForTest();

    CHECK(FileBytes(path) == 0u);
    const std::vector<std::string> lines = ReadLines(path);
    CHECK(lines.empty());
}

TEST_CASE("V7.4 — concurrency under 2 threads (1000 interleaved calls, no torn JSON)") {
    const std::string path = ScratchPath("v74_concurrency");
    smatchet_agent_debug::ResetForTest();
    smatchet_agent_debug::SetTargetPath(path, /*fsync_enabled=*/false);

    const int kPerThread = 500;
    auto worker = [](int tag) {
        for (int i = 0; i < kPerThread; ++i) {
            nlohmann::json payload;
            payload["tag"] = tag;
            payload["i"] = i;
            SMATCHET_AGENT_DEBUG_LOG("temp-debug", payload);
        }
    };
    std::thread t1(worker, 1);
    std::thread t2(worker, 2);
    t1.join();
    t2.join();

    smatchet_agent_debug::ResetForTest();

    const std::vector<std::string> lines = ReadLines(path);
    REQUIRE(lines.size() == static_cast<std::size_t>(kPerThread * 2));
    int seen1 = 0;
    int seen2 = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        nlohmann::json parsed = nlohmann::json::parse(lines[i]);
        REQUIRE(parsed.contains("payload"));
        const int tag = parsed["payload"]["tag"].get<int>();
        if (tag == 1) {
            ++seen1;
        } else if (tag == 2) {
            ++seen2;
        }
    }
    CHECK(seen1 == kPerThread);
    CHECK(seen2 == kPerThread);
}

TEST_CASE("V7.5 — SMATCHET_AGENT_DEBUG_FSYNC env knob") {
    // OFF: fsync counter stays at 0.
    {
        const std::string path = ScratchPath("v75_fsync_off");
        smatchet_agent_debug::ResetForTest();
        smatchet_agent_debug::SetTargetPath(path, /*fsync_enabled=*/false);
        nlohmann::json payload;
        payload["i"] = 1;
        SMATCHET_AGENT_DEBUG_LOG("temp-debug", payload);
        SMATCHET_AGENT_DEBUG_LOG("temp-debug", payload);
        const std::uint64_t count = smatchet_agent_debug::FsyncCountForTest();
        smatchet_agent_debug::ResetForTest();
        CHECK(count == 0u);
    }
    // ON: fsync counter increments per write.
    {
        const std::string path = ScratchPath("v75_fsync_on");
        smatchet_agent_debug::ResetForTest();
        smatchet_agent_debug::SetTargetPath(path, /*fsync_enabled=*/true);
        nlohmann::json payload;
        payload["i"] = 1;
        SMATCHET_AGENT_DEBUG_LOG("temp-debug", payload);
        SMATCHET_AGENT_DEBUG_LOG("temp-debug", payload);
        SMATCHET_AGENT_DEBUG_LOG("temp-debug", payload);
        const std::uint64_t count = smatchet_agent_debug::FsyncCountForTest();
        smatchet_agent_debug::ResetForTest();
        CHECK(count == 3u);
    }
}
