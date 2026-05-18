// ClaudeCodeLocalRunner — drives the real runner against the stub_claude
// helper exe. Covers:
//   - Probe — happy path against stub_claude --version.
//   - Spawn happy path — runner returns true, RUN_RESULT.json parsed, prUrl
//     non-empty, onDelta callback fires at least once.
//   - Spawn error path — stub mode=error, runner returns false + ok=false.
//   - Spawn clarification path — stub mode=clarification, onStateChange
//     fires "AwaitingUser", Resume() drops USER_RESPONSE.json, run completes.
//   - Spawn cancel path — stub mode=sleep, cancel token flips mid-run, runner
//     returns false + result.errorMessage=="cancelled".
//   - Env allow-list — runner-side env block excludes SMATCHET_SECRET; the
//     stub's env_snapshot.txt confirms only the allow-listed keys arrived.

#include "ClaudeCodeLocalRunner.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "CodingHarnessTypes.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef SMATCHET_STUB_CLAUDE_DIR
#error "SMATCHET_STUB_CLAUDE_DIR must be defined via target_compile_definitions"
#endif

namespace {

#ifdef _WIN32
const char* kExeSuffix = ".exe";
#else
const char* kExeSuffix = "";
#endif

std::string StubClaudePath() {
    std::string p = SMATCHET_STUB_CLAUDE_DIR;
    p.push_back('/');
    p.append("stub_claude");
    p.append(kExeSuffix);
    return p;
}

// Unique-ish temp dir under the system tmp area. The runner skips the
// `git worktree add` when Options::skipWorktreeCreate is set, so we just
// need a real directory the stub can read/write.
std::string MakeTempDir(const std::string& tag) {
#if defined(_WIN32)
    char tempBuf[MAX_PATH];
    DWORD got = GetTempPathA(MAX_PATH, tempBuf);
    if (got == 0 || got >= MAX_PATH) {
        return std::string();
    }
    std::string base = std::string(tempBuf) + "smatchet-h3-" + tag + "-";
    base += std::to_string(static_cast<unsigned long long>(GetTickCount64()));
    if (!CreateDirectoryA(base.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return std::string();
    }
    return base;
#else
    std::string base = "/tmp/smatchet-h3-" + tag + "-XXXXXX";
    std::vector<char> tmpl(base.begin(), base.end());
    tmpl.push_back('\0');
    char* p = mkdtemp(tmpl.data());
    if (!p) {
        return std::string();
    }
    return std::string(p);
#endif
}

void RmTreeBestEffort(const std::string& dir) {
    if (dir.empty()) return;
#if defined(_WIN32)
    // Best-effort: not critical for the test exit code.
    std::string cmd = "cmd /c rmdir /S /Q \"" + dir + "\"";
    std::system(cmd.c_str());
#else
    std::string cmd = "rm -rf \"" + dir + "\"";
    std::system(cmd.c_str());
#endif
}

bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return false;
    }
    std::ostringstream os;
    os << f.rdbuf();
    out = os.str();
    return true;
}

void SetEnvPortable(const char* name, const char* value) {
#if defined(_WIN32)
    SetEnvironmentVariableA(name, value);
    // Also push into the CRT env so std::getenv sees it.
    std::string kv = std::string(name) + "=" + (value ? value : "");
    _putenv(kv.c_str());
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

CodingHarness::Seed MakeFixtureSeed(const std::string& worktreeDir) {
    CodingHarness::Seed s;
    s.proposalId = 42;
    s.sourceTracker = "github";
    s.issueKey = "owner/repo#42";
    s.issueTitle = "test issue";
    s.issueBodyMarkdown = "## body\n\ntext\n";
    s.approachOutline = "do the thing";
    s.complexityHint = "low";
    s.targetBranch = "agent/42/test";
    s.workingDirectory = worktreeDir;
    s.timestampUnixSec = 1700000000;
    return s;
}

} // namespace

TEST_SUITE("ClaudeCodeLocalRunner") {

    TEST_CASE("Name returns claude-code-local") {
        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        CodingHarness::ClaudeCodeLocalRunner r(opts);
        CHECK(r.Name() == "claude-code-local");
    }

    TEST_CASE("Probe succeeds against stub_claude --version") {
        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        CodingHarness::ClaudeCodeLocalRunner r(opts);
        std::string err;
        const bool ok = r.Probe(err);
        CHECK_MESSAGE(ok, err);
    }

    TEST_CASE("Probe fails on bogus binary") {
        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = "/definitely/not/a/real/binary/claude_nope";
        CodingHarness::ClaudeCodeLocalRunner r(opts);
        std::string err;
        CHECK_FALSE(r.Probe(err));
        CHECK_FALSE(err.empty());
    }

    TEST_CASE("Spawn happy path returns ok + parses RUN_RESULT.json") {
        const std::string dir = MakeTempDir("happy");
        REQUIRE_FALSE(dir.empty());

        SetEnvPortable("STUB_CLAUDE_MODE", "happy");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        std::atomic<int> deltaCount(0);
        auto onDelta = [&deltaCount](const CodingHarness::StreamEvent&) {
            deltaCount.fetch_add(1);
        };
        std::vector<std::string> states;
        auto onState = [&states](const std::string& s) { states.push_back(s); };
        auto cancel = std::make_shared<std::atomic<bool>>(false);

        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, onDelta, onState, cancel, out, err);
        CHECK_MESSAGE(ok, err);
        CHECK(out.ok);
        CHECK_FALSE(out.prUrl.empty());
        CHECK(out.filesChanged == 3);
        CHECK(deltaCount.load() >= 1);

        // Seeds were written.
        std::string seedJsonBody;
        REQUIRE(ReadFileText(dir + "/SEED.json", seedJsonBody));
        CHECK(seedJsonBody.find("\"proposalId\"") != std::string::npos);

        SetEnvPortable("STUB_CLAUDE_MODE", "");
        RmTreeBestEffort(dir);
    }

    TEST_CASE("Spawn error path surfaces ok=false") {
        const std::string dir = MakeTempDir("err");
        REQUIRE_FALSE(dir.empty());
        SetEnvPortable("STUB_CLAUDE_MODE", "error");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, nullptr, nullptr, cancel, out, err);
        CHECK_FALSE(ok);
        CHECK_FALSE(out.ok);

        SetEnvPortable("STUB_CLAUDE_MODE", "");
        RmTreeBestEffort(dir);
    }

    TEST_CASE("Spawn clarification path fires AwaitingUser + Resume completes") {
        const std::string dir = MakeTempDir("clar");
        REQUIRE_FALSE(dir.empty());
        SetEnvPortable("STUB_CLAUDE_MODE", "clarification");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 20000;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        std::atomic<bool> sawAwaiting(false);
        auto onState = [&r, &sawAwaiting](const std::string& s) {
            if (s == "AwaitingUser" && !sawAwaiting.exchange(true)) {
                // Drop USER_RESPONSE.json so the stub completes.
                CodingHarness::ClarificationResponse resp;
                resp.answer = "go ahead";
                resp.timestampUnixSec = 1700000001;
                std::string e;
                r.Resume(resp, nullptr, e);
            }
        };

        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, nullptr, onState, cancel, out, err);
        CHECK_MESSAGE(ok, err);
        CHECK(sawAwaiting.load());
        CHECK(out.ok);

        SetEnvPortable("STUB_CLAUDE_MODE", "");
        RmTreeBestEffort(dir);
    }

    TEST_CASE("Spawn cancel mid-run returns cancelled") {
        const std::string dir = MakeTempDir("cancel");
        REQUIRE_FALSE(dir.empty());
        SetEnvPortable("STUB_CLAUDE_MODE", "sleep");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 60000;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        std::thread flipper([cancel]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            cancel->store(true);
        });

        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, nullptr, nullptr, cancel, out, err);
        flipper.join();
        CHECK_FALSE(ok);
        CHECK_FALSE(out.ok);
        CHECK(out.errorMessage.find("cancel") != std::string::npos);

        SetEnvPortable("STUB_CLAUDE_MODE", "");
        RmTreeBestEffort(dir);
    }

    // ===== Env allow-list — the security boundary =====
    //
    // Set both SMATCHET_SECRET (forbidden — must be dropped) and GH_TOKEN
    // (allow-listed — must pass through) in the test process. After Spawn,
    // read env_snapshot.txt from the worktree and assert exactly that.
    TEST_CASE("env allow-list drops SMATCHET_SECRET and admits GH_TOKEN") {
        const std::string dir = MakeTempDir("envchk");
        REQUIRE_FALSE(dir.empty());

        SetEnvPortable("STUB_CLAUDE_MODE", "happy");
        SetEnvPortable("SMATCHET_SECRET", "leak-me-not");
        SetEnvPortable("GH_TOKEN", "test-gh-token-allowed");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, nullptr, nullptr, cancel, out, err);
        CHECK_MESSAGE(ok, err);

        std::string snap;
        REQUIRE(ReadFileText(dir + "/env_snapshot.txt", snap));

        // SMATCHET_SECRET must NOT appear in the child's env block.
        CHECK_MESSAGE(snap.find("SMATCHET_SECRET=") == std::string::npos,
                      "leaked SMATCHET_SECRET into child env");

        // GH_TOKEN must be present and carry the expected value.
        CHECK(snap.find("GH_TOKEN=test-gh-token-allowed") != std::string::npos);

        // STUB_CLAUDE_MODE survives as the documented narrow exception
        // (so tests can select stub modes) — sanity-check it arrived.
        CHECK(snap.find("STUB_CLAUDE_MODE=happy") != std::string::npos);

        // Cleanup
        SetEnvPortable("STUB_CLAUDE_MODE", "");
        SetEnvPortable("SMATCHET_SECRET", "");
        SetEnvPortable("GH_TOKEN", "");
        RmTreeBestEffort(dir);
    }
}

#endif // SMATCHET_WITH_AGENTIC
