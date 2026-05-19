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

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
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

#ifndef SMATCHET_STUB_GIT_DIR
#error "SMATCHET_STUB_GIT_DIR must be defined via target_compile_definitions"
#endif
#ifndef SMATCHET_STUB_GH_DIR
#error "SMATCHET_STUB_GH_DIR must be defined via target_compile_definitions"
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

std::string StubGitPath() {
    std::string p = SMATCHET_STUB_GIT_DIR;
    p.push_back('/');
    p.append("stub_git");
    p.append(kExeSuffix);
    return p;
}

std::string StubGhPath() {
    std::string p = SMATCHET_STUB_GH_DIR;
    p.push_back('/');
    p.append("stub_gh");
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

// Capture prior env value before overwrite. `outExists` is set to true when
// the variable had any value (including the empty string) prior to the call;
// otherwise false (meaning "unset"). The two states are distinguished so the
// dtor can restore "unset" rather than collapsing to "" — which mattered for
// the SMATCHET_SECRET assertion (a literal empty-string SMATCHET_SECRET= still
// substring-matches the snapshot check).
bool GetEnvPortable(const char* name, std::string& out) {
#if defined(_WIN32)
    char buf[32 * 1024];
    DWORD got = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (got == 0) {
        // ERROR_ENVVAR_NOT_FOUND when the variable doesn't exist; treat any
        // zero-return as "absent" — a truly-empty variable on Windows is
        // indistinguishable from absent via the Win32 API.
        out.clear();
        return false;
    }
    out.assign(buf, got);
    return true;
#else
    const char* v = std::getenv(name);
    if (!v) { out.clear(); return false; }
    out.assign(v);
    return true;
#endif
}

void UnsetEnvPortable(const char* name) {
#if defined(_WIN32)
    SetEnvironmentVariableA(name, nullptr);
    std::string kv = std::string(name) + "=";
    _putenv(kv.c_str());
#else
    unsetenv(name);
#endif
}

// RAII helper: set an env var for the duration of a scope, restore the prior
// value (or absence) on destruction. Replaces hand-rolled set/clear pairs in
// each TEST_CASE — DOCTEST's REQUIRE-abort skips the cleanup line, leaving
// the test process polluted for the next case otherwise.
class ScopedEnvOverride {
public:
    ScopedEnvOverride(const char* name, const char* value)
        : name_(name) {
        priorExisted_ = GetEnvPortable(name, priorValue_);
        SetEnvPortable(name, value ? value : "");
    }
    ~ScopedEnvOverride() {
        if (priorExisted_) {
            SetEnvPortable(name_.c_str(), priorValue_.c_str());
        } else {
            UnsetEnvPortable(name_.c_str());
        }
    }
    ScopedEnvOverride(const ScopedEnvOverride&) = delete;
    ScopedEnvOverride& operator=(const ScopedEnvOverride&) = delete;
private:
    std::string name_;
    std::string priorValue_;
    bool priorExisted_ = false;
};

// Thread-safe sink for state-callback strings. The runner invokes onState
// from its poll/runner threads, but the test thread reads the collected
// vector during REQUIRE/CHECK. Without synchronisation that read/write pair
// is a data race that a TSan build flags immediately.
class StateSink {
public:
    void Append(const std::string& s) {
        std::lock_guard<std::mutex> g(mu_);
        states_.push_back(s);
    }
    std::vector<std::string> Snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        return states_;
    }
private:
    mutable std::mutex mu_;
    std::vector<std::string> states_;
};

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

        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "happy");

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
        StateSink stateSink;
        auto onState = [&stateSink](const std::string& s) { stateSink.Append(s); };
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

        RmTreeBestEffort(dir);
    }

    TEST_CASE("Spawn error path surfaces ok=false") {
        const std::string dir = MakeTempDir("err");
        REQUIRE_FALSE(dir.empty());
        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "error");

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

        RmTreeBestEffort(dir);
    }

    TEST_CASE("Spawn clarification path fires AwaitingUser + Resume completes") {
        const std::string dir = MakeTempDir("clar");
        REQUIRE_FALSE(dir.empty());
        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "clarification");

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

        RmTreeBestEffort(dir);
    }

    TEST_CASE("Spawn cancel mid-run returns cancelled") {
        const std::string dir = MakeTempDir("cancel");
        REQUIRE_FALSE(dir.empty());
        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "sleep");

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

        RmTreeBestEffort(dir);
    }

    // ===== H6 PR-open fallback =====
    //
    // Sentinel-file path is already covered by "Spawn happy path" above —
    // stub_claude writes PR_URL.txt, runner picks it up, no fallback fires.
    // These cases drive the fallback branch by selecting STUB_CLAUDE_MODE=no-pr
    // (writes RUN_RESULT.json with ok=true but no PR_URL.txt), forcing the
    // runner to invoke `git push` + `gh pr create` itself.

    TEST_CASE("H6 fallback path: gh pr create succeeds + prUrl carries through") {
        const std::string dir = MakeTempDir("h6ok");
        REQUIRE_FALSE(dir.empty());
        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "no-pr");
        ScopedEnvOverride stubGit("STUB_GIT_MODE", "default");
        ScopedEnvOverride stubGh("STUB_GH_MODE", "default");
        ScopedEnvOverride stubGhUrl("STUB_GH_URL", "https://github.com/owner/repo/pull/123");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        opts.autoCreatePrIfMissing = true;
        opts.gitBinPath = StubGitPath();
        opts.ghBinPath = StubGhPath();
        opts.prBaseBranch = "develop";
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        StateSink stateSink;
        auto onState = [&stateSink](const std::string& s) { stateSink.Append(s); };
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, nullptr, onState, cancel, out, err);
        CHECK_MESSAGE(ok, err);
        CHECK(out.ok);
        CHECK(out.prUrl == "https://github.com/owner/repo/pull/123");

        // PR_URL.txt should have been written by the fallback path so future
        // restart-from-disk inspection sees the same URL.
        std::string urlOnDisk;
        REQUIRE(ReadFileText(dir + "/PR_URL.txt", urlOnDisk));
        CHECK(urlOnDisk.find("https://github.com/owner/repo/pull/123") != std::string::npos);

        // State callback should have emitted "PrOpen" before "Complete".
        bool sawPrOpen = false;
        bool sawCompleteAfterPrOpen = false;
        const std::vector<std::string> states = stateSink.Snapshot();
        for (const auto& s : states) {
            if (s == "PrOpen") sawPrOpen = true;
            if (s == "Complete" && sawPrOpen) sawCompleteAfterPrOpen = true;
        }
        CHECK(sawPrOpen);
        CHECK(sawCompleteAfterPrOpen);

        RmTreeBestEffort(dir);
    }

    TEST_CASE("H6 fallback path: gh pr create failure surfaces ok=false") {
        const std::string dir = MakeTempDir("h6gherr");
        REQUIRE_FALSE(dir.empty());
        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "no-pr");
        ScopedEnvOverride stubGit("STUB_GIT_MODE", "default");
        ScopedEnvOverride stubGh("STUB_GH_MODE", "create-fail");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        opts.autoCreatePrIfMissing = true;
        opts.gitBinPath = StubGitPath();
        opts.ghBinPath = StubGhPath();
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, nullptr, nullptr, cancel, out, err);
        CHECK_FALSE(ok);
        CHECK_FALSE(out.ok);
        CHECK(out.errorMessage.find("gh pr create") != std::string::npos);

        RmTreeBestEffort(dir);
    }

    TEST_CASE("H6 fallback path: git push failure surfaces ok=false") {
        const std::string dir = MakeTempDir("h6pusherr");
        REQUIRE_FALSE(dir.empty());
        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "no-pr");
        ScopedEnvOverride stubGit("STUB_GIT_MODE", "push-fail");
        ScopedEnvOverride stubGh("STUB_GH_MODE", "default");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        opts.autoCreatePrIfMissing = true;
        opts.gitBinPath = StubGitPath();
        opts.ghBinPath = StubGhPath();
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, nullptr, nullptr, cancel, out, err);
        CHECK_FALSE(ok);
        CHECK_FALSE(out.ok);
        CHECK(out.errorMessage.find("git push") != std::string::npos);

        RmTreeBestEffort(dir);
    }

    TEST_CASE("H6 fallback path: bad-URL output rejected") {
        const std::string dir = MakeTempDir("h6badurl");
        REQUIRE_FALSE(dir.empty());
        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "no-pr");
        ScopedEnvOverride stubGit("STUB_GIT_MODE", "default");
        ScopedEnvOverride stubGh("STUB_GH_MODE", "bad-url");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        opts.autoCreatePrIfMissing = true;
        opts.gitBinPath = StubGitPath();
        opts.ghBinPath = StubGhPath();
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, nullptr, nullptr, cancel, out, err);
        CHECK_FALSE(ok);
        CHECK_FALSE(out.ok);
        CHECK(out.errorMessage.find("non-PR URL") != std::string::npos);

        RmTreeBestEffort(dir);
    }

    TEST_CASE("H6 fallback disabled: harness without PR_URL.txt leaves prUrl empty") {
        // autoCreatePrIfMissing=false → runner must NOT invoke git/gh. The
        // harness succeeded so ok=true with empty prUrl is the documented
        // outcome (the operator opted out of implicit PR creation).
        const std::string dir = MakeTempDir("h6off");
        REQUIRE_FALSE(dir.empty());
        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "no-pr");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        opts.autoCreatePrIfMissing = false;
        // Intentionally do NOT set git/gh paths — if the fallback fired it
        // would shell out to PATH and probably succeed on dev machines,
        // which would mask the regression. Leaving them empty + autoCreate
        // off keeps the test hermetic.
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        const auto seed = MakeFixtureSeed(dir);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        CodingHarness::RunResult out;
        std::string err;
        const bool ok = r.Spawn(seed, dir, nullptr, nullptr, cancel, out, err);
        CHECK_MESSAGE(ok, err);
        CHECK(out.ok);
        CHECK(out.prUrl.empty());

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

        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "happy");
        ScopedEnvOverride secret("SMATCHET_SECRET", "leak-me-not");
        ScopedEnvOverride ghTok("GH_TOKEN", "test-gh-token-allowed");

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

        RmTreeBestEffort(dir);
    }

    // ===== Phase-7 SpawnAdHoc — ad-hoc worktree path =====
    //
    // The ad-hoc path mirrors Spawn() but creates the worktree under
    // `Options::adHocWorktreeRoot/coderabbit-pr<N>` instead of the
    // proposal-keyed `.claude/worktrees/agent-<id>` shape. We use
    // skipWorktreeCreate so the tests don't need a real git remote;
    // the runner sees the dir already exists at the computed path and
    // proceeds to the seed-file writes + spawn.

    TEST_CASE("Phase-7 SpawnAdHoc happy path writes SEED.json + SEED.md + spawns harness") {
        const std::string root = MakeTempDir("phase7-adhoc");
        REQUIRE_FALSE(root.empty());
        // Pre-create coderabbit-pr42 under root so skipWorktreeCreate=true
        // sees the dir already exists.
        const std::string expected = root + "/coderabbit-pr42";
#if defined(_WIN32)
        REQUIRE(CreateDirectoryA(expected.c_str(), nullptr) != 0);
#else
        REQUIRE(mkdir(expected.c_str(), 0755) == 0);
#endif

        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "happy");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        opts.adHocWorktreeRoot = root;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        CodingHarness::ClaudeCodeLocalRunner::AdHocSpawnRequest req;
        req.prUrl = "https://github.com/owner/repo/pull/42";
        req.headRefName = "feature/x";
        req.dispatchSource = "coderabbit_comment";
        req.commentBodyOrFailureBody = "🛠️ Add a missing const reference here.";
        req.targetAgent = "handoff-implementer";
        req.routedDelegate = "coderabbit-triage";
        req.iteration = 3;

        std::string err;
        const bool ok = r.SpawnAdHoc(req, err);
        CHECK_MESSAGE(ok, err);

        // SEED.json written at the expected path.
        std::string seedJson;
        REQUIRE(ReadFileText(expected + "/SEED.json", seedJson));
        CHECK(seedJson.find("\"dispatch_source\"") != std::string::npos);
        CHECK(seedJson.find("\"coderabbit_comment\"") != std::string::npos);
        CHECK(seedJson.find("\"routed_delegate\"") != std::string::npos);
        CHECK(seedJson.find("\"coderabbit-triage\"") != std::string::npos);
        // Branch encoded as coderabbit/pr<N>/iter<n>.
        CHECK(seedJson.find("coderabbit/pr42/iter3") != std::string::npos);

        // SEED.md opens with the routing headers.
        std::string seedMd;
        REQUIRE(ReadFileText(expected + "/SEED.md", seedMd));
        CHECK(seedMd.find("## First delegate: handoff-implementer") != std::string::npos);
        CHECK(seedMd.find("## Routed via: coderabbit-triage") != std::string::npos);
        CHECK(seedMd.find("dispatch_source:** coderabbit_comment") != std::string::npos);

        // CHECK_RUN.json is NOT written for coderabbit_comment dispatches.
#if defined(_WIN32)
        const DWORD attr = GetFileAttributesA((expected + "/CHECK_RUN.json").c_str());
        CHECK(attr == INVALID_FILE_ATTRIBUTES);
#else
        struct stat st;
        CHECK(stat((expected + "/CHECK_RUN.json").c_str(), &st) != 0);
#endif

        RmTreeBestEffort(root);
    }

    TEST_CASE("Phase-7 SpawnAdHoc CI dispatch writes CHECK_RUN.json") {
        const std::string root = MakeTempDir("phase7-ci");
        REQUIRE_FALSE(root.empty());
        const std::string expected = root + "/coderabbit-pr77";
#if defined(_WIN32)
        REQUIRE(CreateDirectoryA(expected.c_str(), nullptr) != 0);
#else
        REQUIRE(mkdir(expected.c_str(), 0755) == 0);
#endif

        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "happy");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        opts.adHocWorktreeRoot = root;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        CodingHarness::ClaudeCodeLocalRunner::AdHocSpawnRequest req;
        req.prUrl = "https://github.com/owner/repo/pull/77";
        req.headRefName = "agent/77/h7";
        req.dispatchSource = "ci_build_failure";
        req.commentBodyOrFailureBody = "CI check-run 'build' concluded 'failure'";
        req.targetAgent = "handoff-implementer";
        req.routedDelegate = "build-doctor";
        req.iteration = 1;
        req.checkRunPayload = nlohmann::json::object();
        req.checkRunPayload["check_run_id"] = 12345;
        req.checkRunPayload["check_run_name"] = "build";
        req.checkRunPayload["conclusion"] = "failure";

        std::string err;
        const bool ok = r.SpawnAdHoc(req, err);
        CHECK_MESSAGE(ok, err);

        std::string checkRun;
        REQUIRE(ReadFileText(expected + "/CHECK_RUN.json", checkRun));
        CHECK(checkRun.find("\"check_run_id\"") != std::string::npos);
        CHECK(checkRun.find("12345") != std::string::npos);
        CHECK(checkRun.find("\"check_run_name\"") != std::string::npos);
        CHECK(checkRun.find("\"build\"") != std::string::npos);

        std::string seedMd;
        REQUIRE(ReadFileText(expected + "/SEED.md", seedMd));
        CHECK(seedMd.find("## Routed via: build-doctor") != std::string::npos);

        RmTreeBestEffort(root);
    }

    TEST_CASE("Phase-7 SpawnAdHoc rejects empty prUrl") {
        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        CodingHarness::ClaudeCodeLocalRunner::AdHocSpawnRequest req;
        req.headRefName = "feature/x";
        req.dispatchSource = "coderabbit_comment";
        req.targetAgent = "handoff-implementer";
        req.routedDelegate = "coderabbit-triage";

        std::string err;
        CHECK_FALSE(r.SpawnAdHoc(req, err));
        CHECK(err.find("prUrl is empty") != std::string::npos);
    }

    TEST_CASE("Phase-7 SpawnAdHoc rejects malformed prUrl") {
        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        CodingHarness::ClaudeCodeLocalRunner::AdHocSpawnRequest req;
        req.prUrl = "not-a-url";
        req.headRefName = "feature/x";
        req.dispatchSource = "coderabbit_comment";
        req.targetAgent = "handoff-implementer";
        req.routedDelegate = "coderabbit-triage";

        std::string err;
        CHECK_FALSE(r.SpawnAdHoc(req, err));
        CHECK_FALSE(err.empty());
    }

    TEST_CASE("Phase-7 SpawnAdHoc rejects wrong targetAgent") {
        // Per the 2026-05-18 locked decision, targetAgent MUST be
        // "handoff-implementer". Any other value is a coding error.
        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        CodingHarness::ClaudeCodeLocalRunner::AdHocSpawnRequest req;
        req.prUrl = "https://github.com/owner/repo/pull/42";
        req.headRefName = "feature/x";
        req.dispatchSource = "coderabbit_comment";
        req.targetAgent = "coderabbit-triage"; // wrong — should be handoff-implementer
        req.routedDelegate = "coderabbit-triage";

        std::string err;
        CHECK_FALSE(r.SpawnAdHoc(req, err));
        CHECK(err.find("handoff-implementer") != std::string::npos);
    }

    TEST_CASE("Phase-7 SpawnAdHoc rejects empty dispatchSource") {
        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        CodingHarness::ClaudeCodeLocalRunner::AdHocSpawnRequest req;
        req.prUrl = "https://github.com/owner/repo/pull/42";
        req.headRefName = "feature/x";
        // dispatchSource intentionally empty
        req.targetAgent = "handoff-implementer";
        req.routedDelegate = "coderabbit-triage";

        std::string err;
        CHECK_FALSE(r.SpawnAdHoc(req, err));
        CHECK(err.find("dispatchSource") != std::string::npos);
    }

    TEST_CASE("Phase-7 SpawnAdHoc rejects empty headRefName") {
        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        CodingHarness::ClaudeCodeLocalRunner::AdHocSpawnRequest req;
        req.prUrl = "https://github.com/owner/repo/pull/42";
        req.dispatchSource = "coderabbit_comment";
        req.targetAgent = "handoff-implementer";
        req.routedDelegate = "coderabbit-triage";

        std::string err;
        CHECK_FALSE(r.SpawnAdHoc(req, err));
        CHECK(err.find("headRefName") != std::string::npos);
    }

    TEST_CASE("Phase-7 SpawnAdHoc rejects empty routedDelegate") {
        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        CodingHarness::ClaudeCodeLocalRunner::AdHocSpawnRequest req;
        req.prUrl = "https://github.com/owner/repo/pull/42";
        req.headRefName = "feature/x";
        req.dispatchSource = "coderabbit_comment";
        req.targetAgent = "handoff-implementer";

        std::string err;
        CHECK_FALSE(r.SpawnAdHoc(req, err));
        CHECK(err.find("routedDelegate") != std::string::npos);
    }

    TEST_CASE("Phase-7 SpawnAdHoc uses default iteration of 1 when unset") {
        const std::string root = MakeTempDir("phase7-iter1");
        REQUIRE_FALSE(root.empty());
        const std::string expected = root + "/coderabbit-pr5";
#if defined(_WIN32)
        REQUIRE(CreateDirectoryA(expected.c_str(), nullptr) != 0);
#else
        REQUIRE(mkdir(expected.c_str(), 0755) == 0);
#endif

        ScopedEnvOverride stubMode("STUB_CLAUDE_MODE", "happy");

        CodingHarness::ClaudeCodeLocalRunner::Options opts;
        opts.binPath = StubClaudePath();
        opts.skipWorktreeCreate = true;
        opts.timeoutMs = 15000;
        opts.adHocWorktreeRoot = root;
        CodingHarness::ClaudeCodeLocalRunner r(opts);

        CodingHarness::ClaudeCodeLocalRunner::AdHocSpawnRequest req;
        req.prUrl = "https://github.com/owner/repo/pull/5";
        req.headRefName = "feature/x";
        req.dispatchSource = "coderabbit_comment";
        req.targetAgent = "handoff-implementer";
        req.routedDelegate = "coderabbit-triage";
        // iteration intentionally unset → defaults to 1

        std::string err;
        const bool ok = r.SpawnAdHoc(req, err);
        CHECK_MESSAGE(ok, err);

        std::string seedJson;
        REQUIRE(ReadFileText(expected + "/SEED.json", seedJson));
        CHECK(seedJson.find("coderabbit/pr5/iter1") != std::string::npos);

        RmTreeBestEffort(root);
    }
}

#endif // SMATCHET_WITH_AGENTIC
