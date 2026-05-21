// AgenticContextDoc.cpp — per-user agentic-triage project-context Markdown
// doc. Source-list-conditional on SMATCHET_WITH_AGENTIC (see CMakeLists.txt).
// The OFF build never compiles this TU — every consumer is itself
// agentic-gated. `GitHubClient::FetchRawFile` (the only cross-TU dependency
// beyond Logger + ghc::filesystem) lives behind the same gate.

#include "AgenticContextDoc.h"

#include "AgenticInferenceClientPure.h" // kMaxLlmResponseBytes / WouldExceedResponseCap
#include "AiClientFactory.h"
#include "AiTypes.h"
#include "ConfigManager.h"
#include "IAiClient.h"
#include "Logger.h"

#include <atomic>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

#include <ghc/filesystem.hpp>

// GitHubClient lives behind the same SMATCHET_WITH_AGENTIC gate; this TU is
// only compiled when the gate is ON so the include is unconditional here.
#include "GitHubClient.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = ghc::filesystem;

namespace AgenticContextDoc {

namespace {

// Filename literal (relative to the resolved base dir).
constexpr const char* kContextFileName = "agentic-context.md";

// Cache state for ReadCached. Guarded by g_cacheMu so concurrent triage
// requests (master poll + manual run) don't race on the cache fields.
std::mutex g_cacheMu;
bool g_cacheValid = false;
std::string g_cachedContent;
fs::file_time_type g_cachedMtime;

#if defined(_WIN32)
// Resolve the per-user app-data dir on Windows. Returns empty on failure
// (caller surfaces a populated outError if the path can't be assembled).
std::string GetWindowsLocalAppData() {
    const char* env = std::getenv("LOCALAPPDATA");
    if (env != nullptr && env[0] != '\0') {
        return std::string(env);
    }
    // Fallback: USERPROFILE\AppData\Local — covers the rare case of a service
    // session without LOCALAPPDATA set.
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile != nullptr && userProfile[0] != '\0') {
        return std::string(userProfile) + "\\AppData\\Local";
    }
    return std::string();
}
#else
std::string GetUnixHome() {
    const char* env = std::getenv("HOME");
    if (env != nullptr && env[0] != '\0') {
        return std::string(env);
    }
    return std::string();
}
#endif

// Compute the base directory (parent of the context file). Logs at LOG_WARN
// on degraded resolution.
std::string ResolveBaseDir() {
#if defined(_WIN32)
    const std::string local = GetWindowsLocalAppData();
    if (local.empty()) {
        LOG_WARN("AgenticContextDoc::ResolveBaseDir: LOCALAPPDATA / USERPROFILE both empty — context doc disabled");
        return std::string();
    }
    return local + "\\Smatchet";
#else
    const std::string home = GetUnixHome();
    if (home.empty()) {
        LOG_WARN("AgenticContextDoc::ResolveBaseDir: HOME empty — context doc disabled");
        return std::string();
    }
    return home + "/.smatchet";
#endif
}

// Read a file into a std::string. Returns false on open failure; on `not
// exist` the caller treats outContent.empty() + outError.empty() as the
// canonical "first-run default" signal.
bool ReadEntireFile(const std::string& path, std::string& outContent, std::string& outError) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        outError = std::string("failed to open '") + path + "' for read";
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.bad()) {
        outError = std::string("read error on '") + path + "'";
        return false;
    }
    outContent = ss.str();
    return true;
}

// Atomic write — temp file + platform rename. Mirrors the pattern in
// ConfigManager::AtomicWriteTextFile but inlined here to avoid a cross-module
// dependency (ConfigManager pulls in DPAPI / cpr indirectly).
bool AtomicWriteFile(const std::string& path, const std::string& content, std::string& outError) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            outError = std::string("failed to open temp file '") + tmp + "' for write";
            return false;
        }
        if (!content.empty()) {
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        file.flush();
        if (!file.good()) {
            outError = std::string("write failed on temp file '") + tmp + "'";
            file.close();
            std::remove(tmp.c_str());
            return false;
        }
    }
#if defined(_WIN32)
    // Use the Win32 API directly — std::rename on Windows fails when the dest
    // exists, which is the common case (saving an existing doc).
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long err = static_cast<unsigned long>(GetLastError());
        std::ostringstream oss;
        oss << "MoveFileEx failed '" << tmp << "' -> '" << path << "' err=" << err;
        outError = oss.str();
        std::remove(tmp.c_str());
        return false;
    }
    return true;
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::ostringstream oss;
        oss << "rename failed '" << tmp << "' -> '" << path << "' errno=" << errno;
        outError = oss.str();
        std::remove(tmp.c_str());
        return false;
    }
    return true;
#endif
}

} // namespace

std::string GetContextDocPath() {
    LOG_TRACE("AgenticContextDoc::GetContextDocPath enter");
    const std::string baseDir = ResolveBaseDir();
    if (baseDir.empty()) {
        // Caller surfaces an error on subsequent Read / Write; return an empty
        // path here so a unit-level introspection ("did the base resolve?")
        // can distinguish without parsing log output.
        return std::string();
    }
    // Best-effort parent dir creation; ignore the error_code — Read / Write
    // surface the actual filesystem error later if it persists.
    std::error_code ec;
    fs::create_directories(fs::path(baseDir), ec);
    if (ec) {
        LOG_WARN("AgenticContextDoc::GetContextDocPath: create_directories('%s') failed: %s", baseDir.c_str(),
                 ec.message().c_str());
    }
#if defined(_WIN32)
    return baseDir + "\\" + kContextFileName;
#else
    return baseDir + "/" + kContextFileName;
#endif
}

bool Read(std::string& outContent, std::string& outError) {
    LOG_TRACE("AgenticContextDoc::Read enter");
    outContent.clear();
    outError.clear();

    const std::string path = GetContextDocPath();
    if (path.empty()) {
        outError = "could not resolve context doc base directory";
        LOG_ERROR("AgenticContextDoc::Read: %s", outError.c_str());
        return false;
    }

    std::error_code ec;
    if (!fs::exists(fs::path(path), ec)) {
        // First-run default — not an error.
        LOG_TRACE("AgenticContextDoc::Read: '%s' does not exist yet (first-run default)", path.c_str());
        return true;
    }

    std::string readErr;
    if (!ReadEntireFile(path, outContent, readErr)) {
        outError = readErr;
        LOG_ERROR("AgenticContextDoc::Read: %s", outError.c_str());
        outContent.clear();
        return false;
    }
    LOG_DEBUG("AgenticContextDoc::Read read %zu bytes from '%s'", outContent.size(), path.c_str());
    return true;
}

bool Write(const std::string& content, std::string& outError) {
    LOG_TRACE("AgenticContextDoc::Write enter (bytes=%zu)", content.size());
    outError.clear();

    const std::string path = GetContextDocPath();
    if (path.empty()) {
        outError = "could not resolve context doc base directory";
        LOG_ERROR("AgenticContextDoc::Write: %s", outError.c_str());
        return false;
    }

    std::string writeErr;
    if (!AtomicWriteFile(path, content, writeErr)) {
        outError = writeErr;
        LOG_ERROR("AgenticContextDoc::Write: %s", outError.c_str());
        return false;
    }

    // Invalidate the cache so the next ReadCached re-stats the file. Use the
    // mutex to publish the invalidation atomically vs concurrent ReadCached
    // callers from triage workers.
    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        g_cacheValid = false;
        g_cachedContent.clear();
    }

    LOG_INFO("AgenticContextDoc::Write: wrote %zu bytes to '%s'", content.size(), path.c_str());
    return true;
}

bool ReadCached(std::string& outContent, std::string& outError) {
    LOG_TRACE("AgenticContextDoc::ReadCached enter");
    outContent.clear();
    outError.clear();

    const std::string path = GetContextDocPath();
    if (path.empty()) {
        outError = "could not resolve context doc base directory";
        LOG_ERROR("AgenticContextDoc::ReadCached: %s", outError.c_str());
        return false;
    }

    // Snapshot mtime under a no-throw error_code so a missing file is benign.
    std::error_code statEc;
    fs::file_time_type currentMtime{};
    const bool fileExists = fs::exists(fs::path(path), statEc);
    if (fileExists) {
        currentMtime = fs::last_write_time(fs::path(path), statEc);
        if (statEc) {
            outError = std::string("last_write_time failed for '") + path + "': " + statEc.message();
            LOG_WARN("AgenticContextDoc::ReadCached: %s", outError.c_str());
            // Fall through and try a direct Read — non-fatal degraded path.
            return Read(outContent, outError);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        if (g_cacheValid && fileExists && g_cachedMtime == currentMtime) {
            outContent = g_cachedContent;
            LOG_TRACE("AgenticContextDoc::ReadCached cache HIT (%zu bytes)", outContent.size());
            return true;
        }
        if (g_cacheValid && !fileExists) {
            // File was deleted since last cache — surface empty (matches Read).
            g_cacheValid = false;
            g_cachedContent.clear();
        }
    }

    // Cache miss — re-read from disk + repopulate the cache.
    std::string content;
    std::string readErr;
    if (!Read(content, readErr)) {
        outError = readErr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        g_cachedContent = content;
        g_cachedMtime = currentMtime;
        g_cacheValid = fileExists;
    }
    outContent = std::move(content);
    LOG_DEBUG("AgenticContextDoc::ReadCached cache MISS — repopulated (%zu bytes)", outContent.size());
    return true;
}

bool GenerateFromGitHubProject(const std::string& owner, const std::string& repo, GitHubClient& gh,
                               std::string& outContent, std::string& outError) {
    LOG_TRACE("AgenticContextDoc::GenerateFromGitHubProject enter (owner='%s' repo='%s')", owner.c_str(), repo.c_str());
    outContent.clear();
    outError.clear();

    if (owner.empty() || repo.empty()) {
        outError = "owner / repo must be non-empty";
        LOG_WARN("AgenticContextDoc::GenerateFromGitHubProject: %s", outError.c_str());
        return false;
    }

    // Canonical file list — kept short so the generated doc stays inside the
    // ~8 KB system-prompt budget on small repos. Order is README first so the
    // LLM sees the high-level project overview before the contributor /
    // agent-specific guidance.
    struct CandidateFile {
        const char* heading;
        const char* path;
    };
    const CandidateFile kFiles[] = {
        {"README.md", "README.md"},
        {"CONTRIBUTING.md", "CONTRIBUTING.md"},
        {"AGENTS.md", "AGENTS.md"},
    };

    std::string assembled;
    int fetched = 0;
    for (std::size_t i = 0; i < sizeof(kFiles) / sizeof(kFiles[0]); ++i) {
        std::string body;
        std::string fetchErr;
        if (gh.FetchRawFile(owner, repo, kFiles[i].path, body, fetchErr)) {
            LOG_INFO("AgenticContextDoc::GenerateFromGitHubProject: fetched '%s' (%zu bytes) from %s/%s",
                     kFiles[i].path, body.size(), owner.c_str(), repo.c_str());
            assembled += "## ";
            assembled += kFiles[i].heading;
            assembled += "\n\n";
            assembled += body;
            if (!body.empty() && body.back() != '\n') {
                assembled += "\n";
            }
            assembled += "\n";
            ++fetched;
        } else {
            // Missing files are normal (CONTRIBUTING.md often absent); warn
            // but keep going. Auth failures show up in every file's error,
            // so a 0-fetched outcome is the hard-fail signal.
            LOG_WARN("AgenticContextDoc::GenerateFromGitHubProject: skipped '%s' (%s)", kFiles[i].path,
                     fetchErr.c_str());
        }
    }

    if (fetched == 0) {
        outError = "no project context files could be fetched (check repo, PAT, network)";
        LOG_WARN("AgenticContextDoc::GenerateFromGitHubProject: %s", outError.c_str());
        return false;
    }

    outContent = std::move(assembled);
    LOG_INFO("AgenticContextDoc::GenerateFromGitHubProject: assembled %zu bytes from %d files (%s/%s)",
             outContent.size(), fetched, owner.c_str(), repo.c_str());
    return true;
}

namespace {

// System prompt for the project-analysis LLM call. Locks the output shape to
// a single Markdown document the user can hand-edit afterwards. No JSON, no
// preamble, no triple-fence wrapper.
const char* kProjectAnalysisSystemPrompt =
    "You are a software project analyst. Given raw documentation files from a GitHub repository (README, "
    "CONTRIBUTING, AGENTS, etc.), produce a concise structured Markdown document that lets a downstream LLM "
    "triage issues against this project intelligently.\n"
    "\n"
    "Required sections (Markdown headings, in this order):\n"
    "\n"
    "## Purpose\n"
    "One or two sentences naming what the project is + who its users are.\n"
    "\n"
    "## Key features\n"
    "Bulleted list. One feature per line. Include enough detail that a triage agent can match an incoming issue "
    "to a feature area.\n"
    "\n"
    "## Modules / subsystems\n"
    "Bulleted list. `**<name>**: <one-line role>`. Cover every distinct module / directory / subsystem named in "
    "the input docs.\n"
    "\n"
    "## Conventions\n"
    "Bulleted list. Language version, build system, code-style rules, banned features, mandatory invariants.\n"
    "\n"
    "## Glossary\n"
    "Bulleted list. Project-specific terms or acronyms with a one-line definition.\n"
    "\n"
    "## Entry points\n"
    "Bulleted list. How to build / test / run / ship the project, with the exact commands when the input docs "
    "name them.\n"
    "\n"
    "Constraints:\n"
    "- Output ONLY the Markdown document. No JSON envelope. No prose preamble. No ```markdown fence around the "
    "whole output.\n"
    "- Stay grounded in the provided input — do not invent features the docs don't mention.\n"
    "- Omit a section entirely (heading + body) if the input has nothing to say about it.\n";

// Mirrors AgenticInferenceClient — provider / base-URL / API-key / model
// resolution all read from TrackerConfig::AiProviderKind + sibling fields.
// Kept as a small static helper to avoid pulling AgenticInferenceClient.cpp's
// anonymous-namespace functions across translation units.
struct ResolvedAiTarget {
    AiProvider Provider;
    std::string BaseUrl;
    std::string ApiKey;
    std::string Model;
};

bool ResolveAiTarget(const TrackerConfig& cfg, ResolvedAiTarget& outTarget) {
    switch (cfg.AiProviderKind) {
    case 0:
        outTarget.Provider = AiProvider::OpenAi;
        outTarget.BaseUrl = cfg.AiBaseUrl;
        outTarget.ApiKey = cfg.AiApiKey;
        outTarget.Model = cfg.AiModelOpenAi;
        break;
    case 1:
        outTarget.Provider = AiProvider::Anthropic;
        outTarget.BaseUrl = cfg.AiBaseUrl;
        outTarget.ApiKey = cfg.AiAnthropicApiKey;
        outTarget.Model = cfg.AiModelAnthropic;
        break;
    case 2:
        outTarget.Provider = AiProvider::OllamaOpenAiCompat;
        outTarget.BaseUrl = cfg.AiBaseUrl.empty() ? cfg.AiOllamaBaseUrl : cfg.AiBaseUrl;
        outTarget.ApiKey = cfg.AiApiKey;
        outTarget.Model = cfg.AiModelOllama;
        break;
    case 3:
        outTarget.Provider = AiProvider::OllamaNative;
        outTarget.BaseUrl = cfg.AiOllamaBaseUrl;
        outTarget.ApiKey = std::string();
        outTarget.Model = cfg.AiModelOllama;
        break;
    case 4:
        outTarget.Provider = AiProvider::DeepSeek;
        outTarget.BaseUrl =
            cfg.AiDeepSeekBaseUrl.empty() ? std::string("https://api.deepseek.com") : cfg.AiDeepSeekBaseUrl;
        outTarget.ApiKey = cfg.AiDeepSeekApiKey;
        outTarget.Model = cfg.AiModelDeepSeek;
        break;
    default:
        outTarget.Provider = AiProvider::OpenAi;
        outTarget.BaseUrl = cfg.AiBaseUrl;
        outTarget.ApiKey = cfg.AiApiKey;
        outTarget.Model = cfg.AiModelOpenAi;
        break;
    }
    return true;
}

} // namespace

bool AnalyzeProjectWithLLM(const std::string& rawProjectDocs, std::string& outAnalysis, std::string& outError) {
    LOG_TRACE("AgenticContextDoc::AnalyzeProjectWithLLM enter (rawProjectDocs_bytes=%zu)", rawProjectDocs.size());
    outAnalysis.clear();
    outError.clear();

    if (rawProjectDocs.empty()) {
        outError = "no project docs to analyze (empty input)";
        LOG_WARN("AgenticContextDoc::AnalyzeProjectWithLLM: %s", outError.c_str());
        return false;
    }

    const TrackerConfig cfg = ConfigManager::Load();
    ResolvedAiTarget target{};
    ResolveAiTarget(cfg, target);
    LOG_DEBUG("AgenticContextDoc::AnalyzeProjectWithLLM resolved provider='%s' model='%s' baseUrl='%s'",
              AiClientFactory::ProviderToString(target.Provider).c_str(), target.Model.c_str(), target.BaseUrl.c_str());

    // Ollama-native doesn't require an API key; everyone else does.
    if (target.Provider != AiProvider::OllamaNative && target.ApiKey.empty()) {
        outError = std::string("AI provider '") + AiClientFactory::ProviderToString(target.Provider) +
                   "' is not configured (set API key in Preferences -> AI)";
        LOG_WARN("AgenticContextDoc::AnalyzeProjectWithLLM: %s", outError.c_str());
        return false;
    }

    std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(target.Provider);
    if (!client) {
        outError = std::string("AiClientFactory returned null for provider ") +
                   AiClientFactory::ProviderToString(target.Provider);
        LOG_ERROR("AgenticContextDoc::AnalyzeProjectWithLLM: %s", outError.c_str());
        return false;
    }

    AiClientConfig clientCfg;
    clientCfg.ApiKey = target.ApiKey;
    clientCfg.BaseUrl = target.BaseUrl;
    // Project-analysis is a single one-shot call; allow generous budget for
    // the model to produce a multi-kB structured doc but cap end-to-end so a
    // wedge can't hang the Preferences worker thread forever.
    clientCfg.TotalTimeoutMs = 600000; // 10 min hard cap

    AiChatRequest chatReq;
    chatReq.Model = target.Model;
    chatReq.SystemPrompt = kProjectAnalysisSystemPrompt;
    AiMessage userMsg;
    userMsg.Role = "user";
    userMsg.Content = rawProjectDocs;
    chatReq.History.push_back(std::move(userMsg));

    LOG_TRACE("AgenticContextDoc::AnalyzeProjectWithLLM system_prompt (%zu bytes):\n%s", chatReq.SystemPrompt.size(),
              chatReq.SystemPrompt.c_str());
    LOG_TRACE("AgenticContextDoc::AnalyzeProjectWithLLM user_message (%zu bytes):\n%s", rawProjectDocs.size(),
              rawProjectDocs.c_str());

    std::string accumulated;
    AiStreamError captured;
    bool sawError = false;
    bool capExceeded = false;
    AiCancelToken cancel = std::make_shared<std::atomic<bool>>(false);

    auto onDelta = [&accumulated, &capExceeded, &cancel](const AiStreamDelta& d) {
        if (capExceeded) {
            return;
        }
        if (AgenticInferenceClientPure::WouldExceedResponseCap(accumulated.size(), d.TokenChunk.size())) {
            capExceeded = true;
            cancel->store(true);
            LOG_ERROR("AgenticContextDoc::AnalyzeProjectWithLLM: response exceeded %zu-byte cap at %zu bytes — "
                      "cancelling stream",
                      AgenticInferenceClientPure::kMaxLlmResponseBytes, accumulated.size());
            return;
        }
        accumulated += d.TokenChunk;
    };
    auto onError = [&captured, &sawError](const AiStreamError& e) {
        captured = e;
        sawError = true;
    };

    LOG_INFO("AgenticContextDoc::AnalyzeProjectWithLLM: dispatching analysis request provider='%s' model='%s' "
             "input_bytes=%zu",
             AiClientFactory::ProviderToString(target.Provider).c_str(), target.Model.c_str(), rawProjectDocs.size());

    client->SendStreaming(clientCfg, chatReq, onDelta, onError, cancel);
    LOG_DEBUG("AgenticContextDoc::AnalyzeProjectWithLLM: SendStreaming returned (accumulated_bytes=%zu sawError=%d "
              "capExceeded=%d)",
              accumulated.size(), sawError ? 1 : 0, capExceeded ? 1 : 0);

    LOG_TRACE("AgenticContextDoc::AnalyzeProjectWithLLM raw_response (%zu bytes):\n%s", accumulated.size(),
              accumulated.c_str());

    if (capExceeded) {
        outError = "LLM analysis response exceeded the 10 MB cap";
        return false;
    }
    if (sawError) {
        outError = std::string("LLM analysis stream error: ") + captured.Message;
        LOG_ERROR("AgenticContextDoc::AnalyzeProjectWithLLM: %s (httpStatus=%d cancelled=%d)", outError.c_str(),
                  captured.HttpStatus, captured.WasCancelled ? 1 : 0);
        return false;
    }
    if (accumulated.empty()) {
        outError = "LLM analysis returned empty response";
        LOG_WARN("AgenticContextDoc::AnalyzeProjectWithLLM: %s", outError.c_str());
        return false;
    }

    outAnalysis = std::move(accumulated);
    LOG_INFO("AgenticContextDoc::AnalyzeProjectWithLLM: produced %zu bytes of analysis", outAnalysis.size());
    return true;
}

bool GenerateAndAnalyzeFromGitHubProject(const std::string& owner, const std::string& repo, GitHubClient& gh,
                                         std::string& outAnalysis, std::string& outError) {
    LOG_TRACE("AgenticContextDoc::GenerateAndAnalyzeFromGitHubProject enter (owner=%s repo=%s)", owner.c_str(),
              repo.c_str());
    outAnalysis.clear();
    outError.clear();

    std::string rawConcat;
    std::string fetchErr;
    if (!GenerateFromGitHubProject(owner, repo, gh, rawConcat, fetchErr)) {
        outError = fetchErr;
        LOG_WARN("AgenticContextDoc::GenerateAndAnalyzeFromGitHubProject: fetch step failed: %s", fetchErr.c_str());
        return false;
    }
    LOG_TRACE("AgenticContextDoc::GenerateAndAnalyzeFromGitHubProject: raw concat (%zu bytes) ready for LLM",
              rawConcat.size());

    std::string analysis;
    std::string analyzeErr;
    if (!AnalyzeProjectWithLLM(rawConcat, analysis, analyzeErr)) {
        // Graceful fallback — surface raw concat so the editor still
        // populates. User can re-trigger Generate when the AI is configured.
        LOG_WARN("AgenticContextDoc::GenerateAndAnalyzeFromGitHubProject: LLM analysis failed (%s) — "
                 "falling back to raw concat",
                 analyzeErr.c_str());
        outAnalysis = std::move(rawConcat);
        outError = analyzeErr;
        return false;
    }
    outAnalysis = std::move(analysis);
    LOG_INFO("AgenticContextDoc::GenerateAndAnalyzeFromGitHubProject: analysis ready (%zu bytes)", outAnalysis.size());
    return true;
}

} // namespace AgenticContextDoc
