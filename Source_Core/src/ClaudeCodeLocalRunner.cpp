// ClaudeCodeLocalRunner — first concrete ICodingHarnessRunner. See header
// for the security contract: the env allow-list is the real boundary;
// `bypassPermissions` is just "don't prompt on each tool call".

#include "ClaudeCodeLocalRunner.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "CodingHarnessSeedBuilder.h"
#include "Logger.h"
#include "SubprocessCapture.h"

#include <nlohmann/json.hpp>

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
#else
#include <sys/stat.h>
#endif

namespace CodingHarness {

namespace {

// Env-allow-list (decision #7). Every other variable inherited from the
// parent — including all SMATCHET_* config carriers — is dropped before the
// child process sees its env block. This list is the security boundary.
// Additions require a deliberate edit + the env-allow-list doctest update.
const char* const kAllowedEnvKeys[] = {
    "PATH",
    "HOME",
    "USER",
    "USERPROFILE",
    "TEMP",
    "TMP",
    "SYSTEMROOT",
    "GH_TOKEN",
    "GITHUB_TOKEN",
    "ANTHROPIC_API_KEY",
    nullptr,
};

// Filenames the runner writes/reads inside the worktree. The contract is
// shared with the handoff-implementer agent docs; do not rename casually.
const char* const kSeedJsonName       = "SEED.json";
const char* const kSeedMdName         = "SEED.md";
const char* const kRunResultName      = "RUN_RESULT.json";
const char* const kPrUrlName          = "PR_URL.txt";
const char* const kClarificationName  = "CLARIFICATION_NEEDED.json";
const char* const kUserResponseName   = "USER_RESPONSE.json";

bool FileExists(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool DirExists(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string JoinPath(const std::string& dir, const char* name) {
    if (dir.empty()) {
        return std::string(name);
    }
    std::string out = dir;
    if (out.back() != '/' && out.back() != '\\') {
        out.push_back('/');
    }
    out.append(name);
    return out;
}

bool WriteFileText(const std::string& path, const std::string& content, std::string& outError) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        outError = "failed to open for write: " + path;
        return false;
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f.good()) {
        outError = "failed to write: " + path;
        return false;
    }
    return true;
}

std::string ReadFileText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return std::string();
    }
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

// Per-platform helper: pick up `name` from the parent env (if set) and
// return a (name, value) pair. Empty value = not set; caller decides whether
// to include unset keys in the child env (we do not — drop unset).
bool TryReadParentEnv(const char* name, std::string& outValue) {
#if defined(_WIN32)
    // GetEnvironmentVariableA returns 0 on missing; otherwise the buffer
    // length needed (excluding NUL) if buffer too small, else the count
    // copied. Use the two-call dance to size correctly.
    const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return false;
    }
    std::string buf(static_cast<size_t>(needed), '\0');
    const DWORD got = GetEnvironmentVariableA(name, &buf[0], needed);
    if (got == 0 || got >= needed) {
        // Race or other failure — treat as unset.
        return false;
    }
    buf.resize(got);
    outValue = std::move(buf);
    return true;
#else
    const char* v = std::getenv(name);
    if (!v) {
        return false;
    }
    outValue = std::string(v);
    return true;
#endif
}

// Build the env allow-list block for SubprocessCapture. Only the keys in
// kAllowedEnvKeys[] that are present in the parent env are passed through.
// Any key in `forced` overrides; allow-list assertion still applies to its
// name. Forced keys are how tests inject STUB_CLAUDE_MODE without poisoning
// the parent env permanently.
std::vector<std::pair<std::string, std::string>> BuildAllowListEnv(
    const std::vector<std::pair<std::string, std::string>>& forced) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(16);
    for (size_t i = 0; kAllowedEnvKeys[i] != nullptr; ++i) {
        const char* key = kAllowedEnvKeys[i];
        // Skip if the test layer already supplied a value via forced.
        bool overridden = false;
        for (size_t j = 0; j < forced.size(); ++j) {
            if (forced[j].first == key) {
                overridden = true;
                break;
            }
        }
        if (overridden) {
            continue;
        }
        std::string val;
        if (TryReadParentEnv(key, val)) {
            out.emplace_back(std::string(key), std::move(val));
        }
    }
    for (size_t j = 0; j < forced.size(); ++j) {
        out.push_back(forced[j]);
    }
    return out;
}

// Pull STUB_CLAUDE_MODE and any STUB_TEST_* extra keys out of the parent
// env. The runner has no business propagating arbitrary STUB_*, but the
// test layer needs a way to control stub modes without poisoning the env
// allow-list. We carve a narrow exception: `STUB_CLAUDE_MODE` carries the
// mode selector; other keys do not survive.
//
// Production callers pass an empty parent and never see this branch.
std::vector<std::pair<std::string, std::string>> CollectTestModePassthrough() {
    std::vector<std::pair<std::string, std::string>> out;
    std::string v;
    if (TryReadParentEnv("STUB_CLAUDE_MODE", v)) {
        out.emplace_back("STUB_CLAUDE_MODE", v);
    }
    return out;
}

// Run a quick command via SubprocessCapture; return true on exitCode 0.
bool RunQuick(const std::string& argv0, const std::vector<std::string>& args, int timeoutMs,
              std::string& outStderr, std::string& outStdout) {
    SubprocessCapture::CaptureOptions opts;
    opts.argv0 = argv0;
    opts.args = args;
    opts.timeoutMs = timeoutMs;
    SubprocessCapture::CaptureResult res;
    std::string err;
    if (!SubprocessCapture::Run(opts, res, err)) {
        outStderr = err;
        return false;
    }
    outStdout = res.stdoutText;
    outStderr = res.stderrText;
    return res.exitCode == 0 && !res.timedOut && !res.cancelled;
}

// Convert a parsed JSON object into a StreamEvent. Tolerant: unknown shapes
// map to type="unknown" with the raw JSON in data.
StreamEvent ParseStreamEvent(const std::string& line) {
    StreamEvent ev;
    try {
        nlohmann::json j = nlohmann::json::parse(line);
        if (j.is_object()) {
            if (j.contains("type") && j["type"].is_string()) {
                ev.type = j["type"].get<std::string>();
            }
            if (j.contains("subtype") && j["subtype"].is_string()) {
                ev.subtype = j["subtype"].get<std::string>();
            }
            if (j.contains("data")) {
                ev.data = j["data"];
            } else {
                ev.data = j;
            }
        } else {
            ev.type = "unknown";
            ev.data = j;
        }
    } catch (const std::exception& e) {
        LOG_WARN("ClaudeCodeLocalRunner: malformed stream-json line (len=%zu): %s", line.size(), e.what());
        ev.type = "malformed";
        ev.data = nlohmann::json{{"raw", line}};
    }
    return ev;
}

} // namespace

ClaudeCodeLocalRunner::ClaudeCodeLocalRunner(Options opts) : m_opts(std::move(opts)) {}

ClaudeCodeLocalRunner::~ClaudeCodeLocalRunner() = default;

std::string ClaudeCodeLocalRunner::Name() const { return std::string("claude-code-local"); }

bool ClaudeCodeLocalRunner::Probe(std::string& outError) {
    const std::string bin = m_opts.binPath.empty() ? std::string("claude") : m_opts.binPath;
    std::vector<std::string> args;
    args.push_back("--version");
    std::string e, o;
    if (!RunQuick(bin, args, 10000, e, o)) {
        outError = "claude --version failed: " + e;
        LOG_WARN("ClaudeCodeLocalRunner: Probe failed — %s", outError.c_str());
        return false;
    }
    LOG_INFO("ClaudeCodeLocalRunner: Probe ok — %s", o.c_str());
    outError.clear();
    return true;
}

bool ClaudeCodeLocalRunner::Spawn(const Seed& seed,
                                  const std::string& worktreeDir,
                                  DeltaCallback onDelta,
                                  StateChangeCallback onStateChange,
                                  std::shared_ptr<std::atomic<bool>> cancelToken,
                                  RunResult& outResult,
                                  std::string& outError) {
    outResult = RunResult();
    outError.clear();

    if (worktreeDir.empty()) {
        outError = "worktreeDir is empty";
        return false;
    }

    // 1. Worktree creation (skippable for tests that provide a pre-made dir).
    if (!m_opts.skipWorktreeCreate) {
        if (DirExists(worktreeDir)) {
            // Already there — log + reuse rather than fail. The controller
            // (H4) owns idempotency; this branch keeps tests stable when a
            // prior run left the worktree behind.
            LOG_INFO("ClaudeCodeLocalRunner: worktree already exists, reusing: %s", worktreeDir.c_str());
        } else {
            std::vector<std::string> wtArgs;
            wtArgs.push_back("worktree");
            wtArgs.push_back("add");
            wtArgs.push_back(worktreeDir);
            wtArgs.push_back("-b");
            wtArgs.push_back(seed.targetBranch);
            std::string e, o;
            if (!RunQuick("git", wtArgs, 30000, e, o)) {
                outError = "git worktree add failed: " + e;
                LOG_ERROR("ClaudeCodeLocalRunner: %s", outError.c_str());
                return false;
            }
        }
    } else if (!DirExists(worktreeDir)) {
        outError = "skipWorktreeCreate set but worktreeDir does not exist: " + worktreeDir;
        return false;
    }

    // 2. Write SEED.json + SEED.md.
    const std::string seedJsonPath = JoinPath(worktreeDir, kSeedJsonName);
    const std::string seedMdPath   = JoinPath(worktreeDir, kSeedMdName);
    {
        const nlohmann::json js = SeedBuilder::FormatSeedJson(seed);
        if (!WriteFileText(seedJsonPath, js.dump(2), outError)) {
            return false;
        }
    }
    {
        const std::string md = SeedBuilder::FormatSeedMarkdown(seed);
        if (!WriteFileText(seedMdPath, md, outError)) {
            return false;
        }
    }

    // Stash live worktree dir so Resume() can drop USER_RESPONSE.json into
    // the same place. Set under the mutex so a parallel Resume call sees a
    // consistent value once Spawn passes this point.
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_liveWorktreeDir = worktreeDir;
    }

    if (onStateChange) {
        onStateChange("Running");
    }

    // 3. Build the env block (allow-list — decision #7).
    std::vector<std::pair<std::string, std::string>> envForced = CollectTestModePassthrough();
    auto envBlock = BuildAllowListEnv(envForced);

    // 4. Spawn the child via SubprocessCapture with line-streaming on stdout.
    const std::string bin = m_opts.binPath.empty() ? std::string("claude") : m_opts.binPath;
    SubprocessCapture::CaptureOptions opts;
    opts.argv0 = bin;
    opts.args.push_back("--print");
    opts.args.push_back("--output-format");
    opts.args.push_back("stream-json");
    opts.args.push_back("--verbose");
    opts.args.push_back("--permission-mode");
    opts.args.push_back("bypassPermissions");
    opts.args.push_back("--append-system-prompt-file");
    opts.args.push_back(seedMdPath);
    // Positional prompt — short reference; the seed carries the detail.
    {
        std::string prompt = "Implement issue " + seed.issueKey + " per SEED.json. Use the handoff-implementer agent.";
        opts.args.push_back(prompt);
    }
    opts.env = envBlock;
    opts.replaceParentEnv = true;
    opts.cwd = worktreeDir;
    opts.timeoutMs = m_opts.timeoutMs;
    opts.cancelToken = cancelToken;
    opts.stdoutByteCap = 64u * 1024u * 1024u; // 64 MB — stream-json can be chatty.
    opts.stderrByteCap = 2u * 1024u * 1024u;

    // Sentinel-file poll thread. Watches CLARIFICATION_NEEDED.json + PR_URL.txt
    // mtimes on a 1Hz tick and fires onStateChange. The flag flips false when
    // Spawn returns so the thread terminates cleanly.
    std::atomic<bool> stopPoll(false);
    std::thread pollThread;
    bool announcedClar = false;
    bool announcedPr = false;
    {
        const std::string clarPath = JoinPath(worktreeDir, kClarificationName);
        const std::string prPath   = JoinPath(worktreeDir, kPrUrlName);
        StateChangeCallback cb = onStateChange;
        pollThread = std::thread([&stopPoll, clarPath, prPath, cb, &announcedClar, &announcedPr]() {
            while (!stopPoll.load()) {
                if (!announcedClar && FileExists(clarPath)) {
                    announcedClar = true;
                    if (cb) cb("AwaitingUser");
                }
                if (!announcedPr && FileExists(prPath)) {
                    announcedPr = true;
                    if (cb) cb("PrOpen");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        });
    }

    // Line callback parses NDJSON, fires onDelta. Tolerates unknown / malformed.
    opts.onStdoutLine = [onDelta](const std::string& line) {
        if (line.empty()) {
            return;
        }
        if (!onDelta) {
            return;
        }
        const StreamEvent ev = ParseStreamEvent(line);
        onDelta(ev);
    };

    SubprocessCapture::CaptureResult res;
    std::string subErr;
    const bool ranOk = SubprocessCapture::Run(opts, res, subErr);

    stopPoll.store(true);
    if (pollThread.joinable()) {
        pollThread.join();
    }

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_liveWorktreeDir.clear();
    }

    if (!ranOk) {
        outError = "subprocess spawn failed: " + subErr;
        outResult.ok = false;
        outResult.errorMessage = outError;
        if (onStateChange) onStateChange("Failed");
        return false;
    }

    if (res.cancelled) {
        outResult.ok = false;
        outResult.errorMessage = "cancelled";
        if (onStateChange) onStateChange("Cancelled");
        return false;
    }

    // 5. Parse RUN_RESULT.json — written by the spawned harness on exit.
    const std::string runResultPath = JoinPath(worktreeDir, kRunResultName);
    if (FileExists(runResultPath)) {
        try {
            const std::string body = ReadFileText(runResultPath);
            nlohmann::json j = nlohmann::json::parse(body);
            outResult.ok            = j.value("ok", false);
            outResult.errorMessage  = j.value("errorMessage", std::string());
            outResult.prUrl         = j.value("prUrl", std::string());
            outResult.filesChanged  = j.value("filesChanged", 0);
            outResult.linesAdded    = j.value("linesAdded", 0);
            outResult.linesRemoved  = j.value("linesRemoved", 0);
            if (j.contains("toolUseSummary")) {
                outResult.toolUseSummary = j["toolUseSummary"];
            }
        } catch (const std::exception& e) {
            outError = std::string("failed to parse RUN_RESULT.json: ") + e.what();
            outResult.ok = false;
            outResult.errorMessage = outError;
            if (onStateChange) onStateChange("Failed");
            return false;
        }
    } else {
        outResult.ok = (res.exitCode == 0);
        if (!outResult.ok) {
            outResult.errorMessage = "child exited " + std::to_string(res.exitCode) + " without RUN_RESULT.json";
        }
    }

    if (onStateChange) {
        onStateChange(outResult.ok ? "Complete" : "Failed");
    }
    return outResult.ok;
}

bool ClaudeCodeLocalRunner::Resume(const ClarificationResponse& response,
                                   std::shared_ptr<std::atomic<bool>> /*cancelToken*/,
                                   std::string& outError) {
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        dir = m_liveWorktreeDir;
    }
    if (dir.empty()) {
        outError = "Resume: no live worktree (Spawn not in flight)";
        return false;
    }
    const std::string path = JoinPath(dir, kUserResponseName);
    nlohmann::json j = nlohmann::json::object();
    j["answer"] = response.answer;
    j["timestampUnixSec"] = response.timestampUnixSec;
    return WriteFileText(path, j.dump(2), outError);
}

} // namespace CodingHarness

#endif // SMATCHET_WITH_AGENTIC
