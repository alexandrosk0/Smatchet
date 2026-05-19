// ClaudeCodeLocalRunner — first concrete ICodingHarnessRunner. See header
// for the security contract: the env allow-list is the real boundary;
// `bypassPermissions` is just "don't prompt on each tool call".

#include "ClaudeCodeLocalRunner.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "CodingHarnessSeedBuilder.h"
#include "Logger.h"
#include "SubprocessCapture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    "PATH",       "HOME",     "USER",         "USERPROFILE",       "TEMP",  "TMP",
    "SYSTEMROOT", "GH_TOKEN", "GITHUB_TOKEN", "ANTHROPIC_API_KEY", nullptr,
};

// Filenames the runner writes/reads inside the worktree. The contract is
// shared with the handoff-implementer agent docs; do not rename casually.
const char* const kSeedJsonName = "SEED.json";
const char* const kSeedMdName = "SEED.md";
const char* const kRunResultName = "RUN_RESULT.json";
const char* const kPrUrlName = "PR_URL.txt";
const char* const kClarificationName = "CLARIFICATION_NEEDED.json";
const char* const kUserResponseName = "USER_RESPONSE.json";
// (coderabbit-react phase-7) Sentinel for CI-failure ad-hoc dispatches —
// classifier-built payload (check-run name, conclusion, top N annotations,
// last N log lines). Read by handoff-implementer when dispatch_source
// matches `ci_*`. See AGENTS.md § Handoff envelope.
const char* const kCheckRunName = "CHECK_RUN.json";

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
std::vector<std::pair<std::string, std::string>>
BuildAllowListEnv(const std::vector<std::pair<std::string, std::string>>& forced) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(16);
    for (size_t i = 0; kAllowedEnvKeys[i] != nullptr; ++i) {
        const char* key = kAllowedEnvKeys[i];
        // Skip if the test layer already supplied a value via forced.
        const bool overridden =
            std::any_of(forced.begin(), forced.end(),
                        [key](const std::pair<std::string, std::string>& kv) { return kv.first == key; });
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

// Pull STUB_CLAUDE_MODE and the narrow set of STUB_* keys the test layer
// uses to drive stub_claude / stub_git / stub_gh out of the parent env.
// The runner has no business propagating arbitrary STUB_*, but the test
// layer needs a way to control stub modes without poisoning the env
// allow-list. Carved exceptions: STUB_CLAUDE_MODE drives the main harness
// stub; STUB_GIT_MODE / STUB_GH_MODE / STUB_GH_URL drive the helper-
// subprocess stubs invoked via RunQuick / RunQuickInDir (H6 fallback).
//
// Production callers do not have these set and the function returns
// empty for them — no production env leakage.
std::vector<std::pair<std::string, std::string>> CollectTestModePassthrough() {
    std::vector<std::pair<std::string, std::string>> out;
    static const char* const kStubKeys[] = {"STUB_CLAUDE_MODE", "STUB_GIT_MODE", "STUB_GH_MODE", "STUB_GH_URL",
                                            nullptr};
    for (size_t i = 0; kStubKeys[i] != nullptr; ++i) {
        std::string v;
        if (TryReadParentEnv(kStubKeys[i], v)) {
            out.emplace_back(std::string(kStubKeys[i]), std::move(v));
        }
    }
    return out;
}

// Apply the env allow-list (decision #7) to a CaptureOptions before Run.
// Sourced from kAllowedEnvKeys + the STUB_CLAUDE_MODE narrow exception used
// by the test layer. Setting replaceParentEnv=true ensures the child sees
// ONLY the allow-listed keys — without this, helper subprocesses (Probe,
// `git worktree add`, fallback `gh pr create`) inherited the full parent
// env and could carry SMATCHET_*/secret entries past the security
// boundary the Spawn() path enforces.
void ApplyAllowListEnv(SubprocessCapture::CaptureOptions& opts) {
    std::vector<std::pair<std::string, std::string>> forced = CollectTestModePassthrough();
    opts.env = BuildAllowListEnv(forced);
    opts.replaceParentEnv = true;
}

// Run a quick command via SubprocessCapture; return true on exitCode 0.
// Applies the env allow-list — helper subprocesses (Probe, git worktree
// add, the gh pr create fallback) MUST get the same env shape Spawn()
// gives the main harness, otherwise the security boundary leaks.
bool RunQuick(const std::string& argv0, const std::vector<std::string>& args, int timeoutMs, std::string& outStderr,
              std::string& outStdout) {
    SubprocessCapture::CaptureOptions opts;
    opts.argv0 = argv0;
    opts.args = args;
    opts.timeoutMs = timeoutMs;
    ApplyAllowListEnv(opts);
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

// Same as RunQuick but pins cwd to a specific directory. The fallback path
// pushes from the worktree dir (so `git` picks up the right remote / branch
// context) and creates the PR against the same worktree. Shares the same
// env allow-list as RunQuick.
bool RunQuickInDir(const std::string& argv0, const std::vector<std::string>& args, const std::string& cwd,
                   int timeoutMs, std::string& outStderr, std::string& outStdout, int& outExitCode) {
    SubprocessCapture::CaptureOptions opts;
    opts.argv0 = argv0;
    opts.args = args;
    opts.cwd = cwd;
    opts.timeoutMs = timeoutMs;
    ApplyAllowListEnv(opts);
    SubprocessCapture::CaptureResult res;
    std::string err;
    outExitCode = -1;
    if (!SubprocessCapture::Run(opts, res, err)) {
        outStderr = err;
        return false;
    }
    outStdout = res.stdoutText;
    outStderr = res.stderrText;
    outExitCode = res.exitCode;
    return res.exitCode == 0 && !res.timedOut && !res.cancelled;
}

// In-place placeholder substitution for the PR body template. The three
// substitutions are intentional and minimal — opening this to arbitrary
// `{key}` expansion creates a content-injection surface (the seed's issueKey
// would otherwise pass through user-controlled text). Keep this list narrow.
std::string SubstituteTemplate(const std::string& tmpl, std::int64_t proposalId, const std::string& issueKey,
                               const std::string& sourceTracker) {
    std::string out = tmpl;
    auto replaceAll = [&out](const std::string& needle, const std::string& replacement) {
        if (needle.empty()) {
            return;
        }
        std::string::size_type pos = 0;
        while ((pos = out.find(needle, pos)) != std::string::npos) {
            out.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        }
    };
    replaceAll("{proposalId}", std::to_string(proposalId));
    replaceAll("{issueKey}", issueKey);
    replaceAll("{sourceTracker}", sourceTracker);
    return out;
}

// Build the default PR body. Carries the `<!-- smatchet-handoff -->`
// bot-marker so PR-thread comment watchers (H7) can fingerprint our own PRs
// the same way `AgenticHandoffController::IsHandoffBotComment` already
// fingerprints issue-thread comments.
std::string BuildDefaultPrBody(std::int64_t proposalId, const std::string& issueKey, const std::string& sourceTracker) {
    std::ostringstream os;
    os << "Closes " << sourceTracker << "#" << issueKey << "\n\n"
       << "Agent handoff PR. Implements proposal #" << proposalId << ".\n\n"
       << "<!-- smatchet-handoff -->\n";
    return os.str();
}

// Strip the trailing newline from `gh pr create`'s URL output and validate
// it looks like a GitHub PR URL. The cheap regex form ("https://github.com/
// .../pull/<n>") is enough — anything else (`gh` returned the issue URL,
// the user is on a fork, a localhost mirror) should not auto-fingerprint as
// a successful PR-open.
std::string TrimSpace(const std::string& s) {
    const std::string ws = " \t\r\n";
    const auto first = s.find_first_not_of(ws);
    if (first == std::string::npos) {
        return std::string();
    }
    const auto last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}

// (coderabbit-react phase-7) Extract the integer PR number from a canonical
// GitHub PR URL like `https://github.com/<owner>/<repo>/pull/<N>`. Returns
// false on malformed input. Same parser shape as
// `PrCommentWatcher::ParsePrKeyFromUrl` but returns just the integer — the
// runner needs the bare number to name the worktree dir + branch.
bool ExtractPrNumberFromUrl(const std::string& prUrl, int& outNumber, std::string& outError) {
    outNumber = 0;
    outError.clear();
    if (prUrl.empty()) {
        outError = "prUrl is empty";
        return false;
    }
    std::string rest = prUrl;
    const auto schemeEnd = rest.find("://");
    if (schemeEnd != std::string::npos) {
        rest = rest.substr(schemeEnd + 3);
    }
    const auto firstSlash = rest.find('/');
    if (firstSlash == std::string::npos || firstSlash + 1 >= rest.size()) {
        outError = "prUrl missing path after host";
        return false;
    }
    rest = rest.substr(firstSlash + 1);
    std::vector<std::string> segs;
    std::string acc;
    for (char c : rest) {
        if (c == '/' || c == '?' || c == '#') {
            if (!acc.empty()) {
                segs.push_back(std::move(acc));
                acc.clear();
            }
            if (c == '?' || c == '#') {
                break;
            }
        } else {
            acc.push_back(c);
        }
    }
    if (!acc.empty()) {
        segs.push_back(std::move(acc));
    }
    if (segs.size() < 4 || segs[2] != "pull") {
        outError = "prUrl must have shape /<owner>/<repo>/pull/<N>";
        return false;
    }
    const std::string& nstr = segs[3];
    if (nstr.empty() || !std::all_of(nstr.begin(), nstr.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        outError = "prUrl number segment is empty or non-numeric";
        return false;
    }
    // Convert to int — clamp upper bound to int max (PR numbers stay well
    // below INT_MAX in practice; defensive guard for hostile input).
    try {
        const long long v = std::stoll(nstr);
        if (v <= 0 || v > 2147483647LL) {
            outError = "prUrl number out of range";
            return false;
        }
        outNumber = static_cast<int>(v);
    } catch (const std::exception& e) {
        outError = std::string("prUrl number parse failed: ") + e.what();
        return false;
    }
    return true;
}

bool LooksLikeGithubPrUrl(const std::string& url) {
    // Minimum shape: scheme + host + "/pull/" + a digit. Trailing slash /
    // querystring tolerated. Reject empty + whitespace-only outright.
    if (url.empty()) {
        return false;
    }
    if (url.find("https://") != 0 && url.find("http://") != 0) {
        return false;
    }
    // Must contain "/pull/" — distinguishes issue URLs from PR URLs even
    // when both share the same repo prefix.
    const auto pullPos = url.find("/pull/");
    if (pullPos == std::string::npos) {
        return false;
    }
    // At least one digit must follow "/pull/" before whatever comes next.
    const auto digitStart = pullPos + 6;
    if (digitStart >= url.size()) {
        return false;
    }
    return std::isdigit(static_cast<unsigned char>(url[digitStart])) != 0;
}

// Title heuristic for the fallback PR. The contract is: first line of the
// last commit (the harness's own commit message), else `proposal.issueTitle`,
// else `proposal.issueKey`, else a sentinel. Never empty — `gh pr create`
// rejects an empty `--title`.
std::string ResolvePrTitle(const std::string& worktreeDir, const std::string& gitBin, const std::string& issueTitle,
                           const std::string& issueKey) {
    std::vector<std::string> args;
    args.push_back("log");
    args.push_back("-1");
    args.push_back("--pretty=%s");
    std::string err, out;
    int exitCode = 0;
    if (RunQuickInDir(gitBin, args, worktreeDir, 10000, err, out, exitCode)) {
        const std::string first = TrimSpace(out);
        if (!first.empty()) {
            return first;
        }
    }
    if (!issueTitle.empty()) {
        return issueTitle;
    }
    if (!issueKey.empty()) {
        return "Agent handoff: " + issueKey;
    }
    return std::string("Agent handoff PR");
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

bool ClaudeCodeLocalRunner::Spawn(const Seed& seed, const std::string& worktreeDir, DeltaCallback onDelta,
                                  StateChangeCallback onStateChange, std::shared_ptr<std::atomic<bool>> cancelToken,
                                  RunResult& outResult, std::string& outError) {
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
    const std::string seedMdPath = JoinPath(worktreeDir, kSeedMdName);
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

    return SpawnCore(seed, worktreeDir, std::move(onDelta), std::move(onStateChange), std::move(cancelToken), outResult,
                     outError);
}

bool ClaudeCodeLocalRunner::SpawnCore(const Seed& seed, const std::string& worktreeDir, DeltaCallback onDelta,
                                      StateChangeCallback onStateChange, std::shared_ptr<std::atomic<bool>> cancelToken,
                                      RunResult& outResult, std::string& outError, bool allowPrAutoCreateFallback) {
    // Caller (Spawn / SpawnAdHoc) is responsible for worktree creation +
    // SEED.json + SEED.md (+ optional CHECK_RUN.json) writes. This helper
    // owns the env-allow-list build, subprocess spawn, line-streaming, poll
    // thread for clarification + PR sentinels, RUN_RESULT.json parse, and
    // the auto-PR-create fallback. Refactored out of Spawn() in phase-7 so
    // SpawnAdHoc shares the same child-process loop.
    const std::string seedMdPath = JoinPath(worktreeDir, kSeedMdName);

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
        const std::string prPath = JoinPath(worktreeDir, kPrUrlName);
        StateChangeCallback cb = onStateChange;
        pollThread = std::thread([&stopPoll, clarPath, prPath, cb, &announcedClar, &announcedPr]() {
            while (!stopPoll.load()) {
                if (!announcedClar && FileExists(clarPath)) {
                    announcedClar = true;
                    if (cb)
                        cb("AwaitingUser");
                }
                if (!announcedPr && FileExists(prPath)) {
                    announcedPr = true;
                    if (cb)
                        cb("PrOpen");
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
        if (onStateChange)
            onStateChange("Failed");
        return false;
    }

    if (res.cancelled) {
        outResult.ok = false;
        outResult.errorMessage = "cancelled";
        if (onStateChange)
            onStateChange("Cancelled");
        return false;
    }

    // 5. Parse RUN_RESULT.json — written by the spawned harness on exit.
    const std::string runResultPath = JoinPath(worktreeDir, kRunResultName);
    if (FileExists(runResultPath)) {
        try {
            const std::string body = ReadFileText(runResultPath);
            nlohmann::json j = nlohmann::json::parse(body);
            outResult.ok = j.value("ok", false);
            outResult.errorMessage = j.value("errorMessage", std::string());
            outResult.prUrl = j.value("prUrl", std::string());
            outResult.filesChanged = j.value("filesChanged", 0);
            outResult.linesAdded = j.value("linesAdded", 0);
            outResult.linesRemoved = j.value("linesRemoved", 0);
            if (j.contains("toolUseSummary")) {
                outResult.toolUseSummary = j["toolUseSummary"];
            }
        } catch (const std::exception& e) {
            outError = std::string("failed to parse RUN_RESULT.json: ") + e.what();
            outResult.ok = false;
            outResult.errorMessage = outError;
            if (onStateChange)
                onStateChange("Failed");
            return false;
        }
    } else {
        outResult.ok = (res.exitCode == 0);
        if (!outResult.ok) {
            outResult.errorMessage = "child exited " + std::to_string(res.exitCode) + " without RUN_RESULT.json";
        }
    }

    // PR-open fallback: harness may exit with ok=true without writing
    // PR_URL.txt (e.g. early-failure mode that recovered, or a harness
    // build that never learned the contract). Run `git push` + `gh pr
    // create` ourselves so the controller can still reach PrOpen with a
    // valid URL. Failure here is a hard stop — we tried, the result
    // surfaces to the audit trail via outResult.errorMessage. Skipped
    // when the run already failed (no PR to open for a broken branch).
    if (allowPrAutoCreateFallback && outResult.ok && outResult.prUrl.empty() && m_opts.autoCreatePrIfMissing) {
        const std::string prUrlSentinel = JoinPath(worktreeDir, kPrUrlName);
        if (FileExists(prUrlSentinel)) {
            // Sentinel showed up between the file check above and this
            // branch — read it like the canonical path would have. This
            // narrow race covers the case where the poll thread saw the
            // file ~simultaneously with our parse of RUN_RESULT.json.
            outResult.prUrl = TrimSpace(ReadFileText(prUrlSentinel));
        }
        if (outResult.prUrl.empty()) {
            const std::string gitBin = m_opts.gitBinPath.empty() ? std::string("git") : m_opts.gitBinPath;
            const std::string ghBin = m_opts.ghBinPath.empty() ? std::string("gh") : m_opts.ghBinPath;
            const std::string baseBranch = m_opts.prBaseBranch.empty() ? std::string("develop") : m_opts.prBaseBranch;

            // 1. `git push -u origin <branch>` from the worktree dir.
            std::vector<std::string> pushArgs;
            pushArgs.push_back("push");
            pushArgs.push_back("-u");
            pushArgs.push_back("origin");
            pushArgs.push_back(seed.targetBranch);
            std::string pushErr, pushOut;
            int pushExit = 0;
            const bool pushed = RunQuickInDir(gitBin, pushArgs, worktreeDir, 60000, pushErr, pushOut, pushExit);
            if (!pushed) {
                outError = "PR-open fallback: git push failed (exit=" + std::to_string(pushExit) + "): " + pushErr;
                outResult.ok = false;
                outResult.errorMessage = outError;
                if (onStateChange)
                    onStateChange("Failed");
                LOG_ERROR("ClaudeCodeLocalRunner: %s", outError.c_str());
                return false;
            }

            // 2. `gh pr create --draft --base <base> --title ... --body ...`.
            const std::string title = ResolvePrTitle(worktreeDir, gitBin, seed.issueTitle, seed.issueKey);
            std::string body;
            if (m_opts.prBodyTemplate.empty()) {
                body = BuildDefaultPrBody(seed.proposalId, seed.issueKey, seed.sourceTracker);
            } else {
                body = SubstituteTemplate(m_opts.prBodyTemplate, seed.proposalId, seed.issueKey, seed.sourceTracker);
            }
            std::vector<std::string> ghArgs;
            ghArgs.push_back("pr");
            ghArgs.push_back("create");
            ghArgs.push_back("--draft");
            ghArgs.push_back("--base");
            ghArgs.push_back(baseBranch);
            ghArgs.push_back("--title");
            ghArgs.push_back(title);
            ghArgs.push_back("--body");
            ghArgs.push_back(body);
            std::string ghErr, ghOut;
            int ghExit = 0;
            const bool created = RunQuickInDir(ghBin, ghArgs, worktreeDir, 60000, ghErr, ghOut, ghExit);
            if (!created) {
                outError = "PR-open fallback: gh pr create failed (exit=" + std::to_string(ghExit) + "): " + ghErr;
                outResult.ok = false;
                outResult.errorMessage = outError;
                if (onStateChange)
                    onStateChange("Failed");
                LOG_ERROR("ClaudeCodeLocalRunner: %s", outError.c_str());
                return false;
            }

            // 3. Parse + validate the URL `gh` printed to stdout.
            const std::string urlCandidate = TrimSpace(ghOut);
            if (!LooksLikeGithubPrUrl(urlCandidate)) {
                outError = "PR-open fallback: gh pr create returned non-PR URL: '" + urlCandidate + "'";
                outResult.ok = false;
                outResult.errorMessage = outError;
                if (onStateChange)
                    onStateChange("Failed");
                LOG_ERROR("ClaudeCodeLocalRunner: %s", outError.c_str());
                return false;
            }
            outResult.prUrl = urlCandidate;

            // 4. Write PR_URL.txt so the runner-side sentinel is uniform with
            // the harness-written path; downstream watchers / restart paths
            // pick up the same file. Failure here is non-fatal — the prUrl
            // is already on outResult and the controller transitions on that.
            std::string writeErr;
            if (!WriteFileText(prUrlSentinel, urlCandidate + "\n", writeErr)) {
                LOG_WARN("ClaudeCodeLocalRunner: PR-open fallback succeeded but PR_URL.txt write failed: %s",
                         writeErr.c_str());
            }
            LOG_INFO("ClaudeCodeLocalRunner: PR-open fallback created PR: %s", urlCandidate.c_str());
        }
    }

    if (outResult.ok && !outResult.prUrl.empty() && onStateChange) {
        // Belt-and-braces emit: the poll thread may have raced ahead and
        // emitted PrOpen already; the controller's FSM no-ops a duplicate
        // terminal-direction transition. This call ensures the fallback path
        // surfaces PrOpen even when the poll thread missed it (which it does
        // when we wrote PR_URL.txt above after stopPoll flipped).
        onStateChange("PrOpen");
    }
    if (onStateChange) {
        onStateChange(outResult.ok ? "Complete" : "Failed");
    }
    return outResult.ok;
}

bool ClaudeCodeLocalRunner::SpawnAdHoc(const AdHocSpawnRequest& req, std::string& outError) {
    // (coderabbit-react phase-7) Ad-hoc dispatch — CodeRabbit PR-comment
    // dispatches + CI-failure dispatches. Shared worktree per PR
    // (.claude/worktrees/coderabbit-pr<N>); iter-suffixed branch
    // (coderabbit/pr<N>/iter<n>) so successive dispatches on the same PR
    // don't collide. See header for the full contract.
    outError.clear();

    if (req.prUrl.empty()) {
        outError = "AdHocSpawnRequest.prUrl is empty";
        return false;
    }
    if (req.headRefName.empty()) {
        outError = "AdHocSpawnRequest.headRefName is empty";
        return false;
    }
    if (req.dispatchSource.empty()) {
        outError = "AdHocSpawnRequest.dispatchSource is empty";
        return false;
    }
    if (req.targetAgent != "handoff-implementer") {
        // Locked decision (2026-05-18): the spawned harness's first delegate
        // is ALWAYS handoff-implementer. Any deviation is a coding error,
        // not a config knob. Refuse rather than silently route around it.
        outError = "AdHocSpawnRequest.targetAgent must be 'handoff-implementer'";
        return false;
    }
    if (req.routedDelegate.empty()) {
        outError = "AdHocSpawnRequest.routedDelegate is empty";
        return false;
    }

    int prNumber = 0;
    {
        std::string parseErr;
        if (!ExtractPrNumberFromUrl(req.prUrl, prNumber, parseErr)) {
            outError = "AdHocSpawnRequest.prUrl malformed: " + parseErr;
            return false;
        }
    }
    const int iterN = (req.iteration > 0) ? req.iteration : 1;

    // Worktree path — .claude/worktrees/coderabbit-pr<N>. Per the plan,
    // one worktree per PR shared across CodeRabbit + CI dispatches.
    // Relative to CWD on purpose: the runner is invoked from the Smatchet
    // process whose CWD is the repo root. Tests inject an absolute root via
    // Options::adHocWorktreeRoot.
    const std::string adHocRoot =
        m_opts.adHocWorktreeRoot.empty() ? std::string(".claude/worktrees") : m_opts.adHocWorktreeRoot;
    std::string worktreeRoot = adHocRoot;
    if (!worktreeRoot.empty() && worktreeRoot.back() != '/' && worktreeRoot.back() != '\\') {
        worktreeRoot.push_back('/');
    }
    worktreeRoot += "coderabbit-pr" + std::to_string(prNumber);
    const std::string branchName = "coderabbit/pr" + std::to_string(prNumber) + "/iter" + std::to_string(iterN);

    // Worktree create. Reuse the existing dir if present (e.g. a prior
    // iter on the same PR left it). On fresh creation, base off
    // `origin/<headRefName>` so the spawned harness sees the in-flight
    // PR's commits.
    if (!m_opts.skipWorktreeCreate) {
        if (DirExists(worktreeRoot)) {
            LOG_INFO("ClaudeCodeLocalRunner::SpawnAdHoc: worktree already exists, reusing: %s", worktreeRoot.c_str());
            // Best-effort: checkout the iter-branch (creating it if absent).
            // `git worktree add` would fail on an existing dir; use the
            // `git -C <dir> checkout -B <branch> origin/<headRefName>`
            // shape so reruns advance to the new iter cleanly.
            std::vector<std::string> coArgs;
            coArgs.push_back("-C");
            coArgs.push_back(worktreeRoot);
            coArgs.push_back("checkout");
            coArgs.push_back("-B");
            coArgs.push_back(branchName);
            coArgs.push_back(std::string("origin/") + req.headRefName);
            std::string ce, co;
            const std::string gitBin = m_opts.gitBinPath.empty() ? std::string("git") : m_opts.gitBinPath;
            if (!RunQuick(gitBin, coArgs, 30000, ce, co)) {
                outError = "git checkout -B failed: " + ce;
                LOG_ERROR("ClaudeCodeLocalRunner::SpawnAdHoc: %s", outError.c_str());
                return false;
            }
        } else {
            std::vector<std::string> wtArgs;
            wtArgs.push_back("worktree");
            wtArgs.push_back("add");
            wtArgs.push_back(worktreeRoot);
            wtArgs.push_back("-b");
            wtArgs.push_back(branchName);
            wtArgs.push_back(std::string("origin/") + req.headRefName);
            std::string we, wo;
            const std::string gitBin = m_opts.gitBinPath.empty() ? std::string("git") : m_opts.gitBinPath;
            if (!RunQuick(gitBin, wtArgs, 30000, we, wo)) {
                outError = "git worktree add failed: " + we;
                LOG_ERROR("ClaudeCodeLocalRunner::SpawnAdHoc: %s", outError.c_str());
                return false;
            }
        }
    } else if (!DirExists(worktreeRoot)) {
        outError = "skipWorktreeCreate set but worktree dir does not exist: " + worktreeRoot;
        return false;
    }

    // Build the seed. Payload extras carry the dispatch_source +
    // routed_delegate + comment body so the spawned harness reads them via
    // SeedBuilder's forward-compat passthrough.
    Seed seed;
    seed.proposalId = 0; // No proposal id for ad-hoc dispatches.
    seed.sourceTracker = "github";
    seed.issueKey = std::string();
    seed.issueTitle = std::string("PR #") + std::to_string(prNumber) + " · " + req.dispatchSource;
    seed.issueBodyMarkdown = req.commentBodyOrFailureBody;
    {
        std::ostringstream approach;
        approach << "Ad-hoc dispatch from PR #" << prNumber << " (source=" << req.dispatchSource << ", routed via "
                 << req.routedDelegate << ").";
        seed.approachOutline = approach.str();
    }
    seed.complexityHint = "medium";
    seed.targetBranch = branchName;
    seed.workingDirectory = worktreeRoot;
    seed.timestampUnixSec =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    // (phase-7) payloadExtra discriminator fields. SeedBuilder passes these
    // through verbatim — the spawned handoff-implementer reads them to pick
    // a routed sub-delegate per the 2026-05-18 locked decision.
    seed.payloadExtra = nlohmann::json::object();
    seed.payloadExtra["dispatch_source"] = req.dispatchSource;
    seed.payloadExtra["pr_url"] = req.prUrl;
    seed.payloadExtra["head_ref_name"] = req.headRefName;
    seed.payloadExtra["comment_body"] = req.commentBodyOrFailureBody;
    seed.payloadExtra["target_agent"] = req.targetAgent;
    seed.payloadExtra["routed_delegate"] = req.routedDelegate;
    seed.payloadExtra["iteration"] = iterN;

    // SEED.json — canonical payload.
    const std::string seedJsonPath = JoinPath(worktreeRoot, kSeedJsonName);
    {
        const nlohmann::json js = SeedBuilder::FormatSeedJson(seed);
        if (!WriteFileText(seedJsonPath, js.dump(2), outError)) {
            return false;
        }
    }

    // SEED.md — opens with the routing header per the locked decision so the
    // spawned `claude` reads the first-delegate + routed sub-delegate at
    // a glance. The base SeedBuilder markdown follows.
    const std::string seedMdPath = JoinPath(worktreeRoot, kSeedMdName);
    {
        std::ostringstream md;
        md << "## First delegate: handoff-implementer\n\n"
           << "## Routed via: " << req.routedDelegate << "\n\n"
           << "- **dispatch_source:** " << req.dispatchSource << "\n"
           << "- **pr_url:** " << req.prUrl << "\n"
           << "- **head_ref_name:** " << req.headRefName << "\n"
           << "- **iter_branch:** " << branchName << "\n"
           << "- **iteration:** " << iterN << "\n\n";
        md << SeedBuilder::FormatSeedMarkdown(seed);
        if (!WriteFileText(seedMdPath, md.str(), outError)) {
            return false;
        }
    }

    // CHECK_RUN.json — CI dispatches only. Phase-7 sentinel addition per
    // AGENTS.md § Handoff envelope. Empty payload is permitted (the
    // dispatcher may not have a full classifier output to hand off); the
    // file is still written so handoff-implementer's `if exists` probe
    // matches the documented contract.
    const bool isCiDispatch = (req.dispatchSource.compare(0, 3, "ci_") == 0);
    const std::string checkRunPath = JoinPath(worktreeRoot, kCheckRunName);
    if (isCiDispatch) {
        const nlohmann::json payload = req.checkRunPayload.is_null() ? nlohmann::json::object() : req.checkRunPayload;
        if (!WriteFileText(checkRunPath, payload.dump(2), outError)) {
            return false;
        }
    } else if (FileExists(checkRunPath)) {
        // Re-used per-PR worktrees can leave a CHECK_RUN.json from an
        // earlier `ci_*` dispatch; clear it before spawning a
        // `coderabbit_comment` (or other non-CI) child so the
        // documented `if exists` contract is not violated.
        //
        // (CodeRabbit PR #303) Best-effort delete — log on failure so a
        // stale CI payload that survives is diagnosable, but do not
        // error out (the child can still proceed; the worst case is the
        // handoff-implementer reads a stale CHECK_RUN.json which it
        // already tolerates per the dispatch_source contract).
        if (std::remove(checkRunPath.c_str()) != 0) {
            const int err = errno;
            LOG_WARN("ClaudeCodeLocalRunner::SpawnAdHoc: failed to remove stale CHECK_RUN.json at %s (errno=%d: %s)",
                     checkRunPath.c_str(), err, std::strerror(err));
        }
    }

    // Forward to the shared child loop. No delta / state callbacks for
    // ad-hoc dispatches today — the operator audit-trails them via the PR
    // comments + the controller's audit sink. Future variants can plumb
    // toast / panel callbacks through.
    RunResult result;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    // SpawnAdHoc disables the shared PR auto-create fallback — the iter-branch
    // lives off the PR's head ref, not develop, so a fallback `gh pr create`
    // would mint a duplicate PR. The handoff-implementer is expected to push
    // the iter-branch and the operator merges back manually.
    if (!SpawnCore(seed, worktreeRoot, /*onDelta*/ nullptr, /*onStateChange*/ nullptr, cancel, result, outError,
                   /*allowPrAutoCreateFallback=*/false)) {
        return false;
    }
    return result.ok;
}

bool ClaudeCodeLocalRunner::Resume(const ClarificationResponse& response,
                                   std::shared_ptr<std::atomic<bool>> /*cancelToken*/, std::string& outError) {
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
