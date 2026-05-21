// AgenticContextDoc.h — per-user editable Markdown document prepended to every
// agentic-triage LLM system prompt.
//
// Stored on disk at:
//   - Windows:     %LOCALAPPDATA%/Smatchet/agentic-context.md
//   - non-Windows: ${HOME}/.smatchet/agentic-context.md
//
// Surface lives behind the SMATCHET_WITH_AGENTIC source-list gate (see the
// matching `.cpp` — the TU is added/removed from CORE_SOURCES via the same
// pattern as GitHubClient.cpp). Consumers that hold an `AgenticContextDoc`
// reference must `#if defined(SMATCHET_WITH_AGENTIC)`-gate their include of
// this header so the OFF build does not see the symbol.

#ifndef SMATCHET_AGENTIC_CONTEXT_DOC_H
#define SMATCHET_AGENTIC_CONTEXT_DOC_H

#include <string>

class GitHubClient;

namespace AgenticContextDoc {

// Returns the absolute path to the per-user context doc file. Honours
// LOCALAPPDATA on Windows, HOME elsewhere. Idempotent: creates the parent
// dir on demand (best-effort; failure is silent — the subsequent Read /
// Write surfaces the actual I/O error).
std::string GetContextDocPath();

// Reads the doc from disk. Returns true + empty content + empty `outError`
// when the file does not exist (not an error condition; first-run default).
// Returns false + populated `outError` on I/O failure.
bool Read(std::string& outContent, std::string& outError);

// Writes content atomically (write to temp + rename). Creates the parent dir
// on demand. Returns false + populated `outError` on any failure.
bool Write(const std::string& content, std::string& outError);

// Cached read — same as Read() but caches the content + mtime in a TU-static
// so per-LLM-call reads don't hit the filesystem. Cache is invalidated when
// file mtime changes. Called by `AgenticInferenceClient` on every triage
// request. Thread-safe via internal mutex.
bool ReadCached(std::string& outContent, std::string& outError);

// Fetches README.md / CONTRIBUTING.md / AGENTS.md from `owner/repo` via the
// GitHub Raw API (via `GitHubClient::FetchRawFile`). Returns concatenated
// markdown with `## <filename>` headers. Missing files are skipped (logged
// at LOG_WARN, not fatal). Returns false only on hard failure (all three
// missing or auth failure).
bool GenerateFromGitHubProject(const std::string& owner, const std::string& repo, GitHubClient& gh,
                               std::string& outContent, std::string& outError);

// Sends `rawProjectDocs` (typically the concatenated output of
// `GenerateFromGitHubProject`) to the configured LLM provider with a
// project-analysis system prompt. The LLM produces a structured Markdown
// document mapping the project's purpose, features, modules, conventions,
// glossary, and entry points. Returns the LLM output verbatim in
// `outAnalysis`.
//
// Provider resolution mirrors `AgenticInferenceClient::RequestProposals` —
// reads `TrackerConfig` via `ConfigManager::Load()` to pick provider, model,
// base URL, API key. Returns false + populated `outError` when:
//   - AI provider is unconfigured (empty API key for OpenAI/Anthropic/DeepSeek)
//   - LLM stream errors out
//   - LLM returns empty response
//   - Response exceeds the inference cap (`kMaxLlmResponseBytes`)
//
// Thread-safe: no internal state mutation. Safe to call from a background
// worker thread (the standard caller path from the Preferences "Generate"
// button via `LaunchBackgroundTask`).
bool AnalyzeProjectWithLLM(const std::string& rawProjectDocs, std::string& outAnalysis, std::string& outError);

// Convenience: `GenerateFromGitHubProject` + `AnalyzeProjectWithLLM` chained.
// Returns the LLM-produced analysis on success; the intermediate raw concat
// is logged at LOG_TRACE and discarded. On hard failure of either step,
// returns false + populated `outError`. On LLM-analysis failure with a
// successful fetch, the raw concat is surfaced as a graceful fallback
// (logged at LOG_WARN) so the user still gets *something* in the editor.
bool GenerateAndAnalyzeFromGitHubProject(const std::string& owner, const std::string& repo, GitHubClient& gh,
                                         std::string& outAnalysis, std::string& outError);

} // namespace AgenticContextDoc

#endif // SMATCHET_AGENTIC_CONTEXT_DOC_H
