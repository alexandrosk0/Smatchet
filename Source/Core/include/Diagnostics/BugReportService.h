#ifndef SMATCHET_DIAGNOSTICS_BUG_REPORT_SERVICE_H
#define SMATCHET_DIAGNOSTICS_BUG_REPORT_SERVICE_H

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ConfigManager.h"

// BugReportService — captures runtime context and files a bug report as a new
// GitHub issue on a FIXED, configured dev repo (never the active tracker).
// Layered so the pure pieces (target/PAT resolution, markdown body) are
// unit-testable without network, and the heavy pieces (context gather, HTTP
// submit, screenshot upload) live in a separate TU that the doctest rig skips.
//
// docs/plans/shipped/log-a-bug-github.md.

class IAppMeta; // narrow AppController facet — the service reads only the app version

namespace smatchet {
namespace diagnostics {

struct BugReportOptions {
    std::string UserDescription;
    std::string ScreenshotAbsPath; // filled by the standalone UI; empty otherwise (no live frame in CLI/MCP)
    std::string DumpAbsPath;       // phase-2 crash: minidump to upload as a GitHub Release asset
    // When non-empty, this exact text is the issue body (the user-edited egress
    // preview — WYSIWYG consent). When empty, the body is built via BuildMarkdownBody.
    std::string BodyOverride;
    bool IncludeScreenshot = true;
    bool Censored = false;
    std::size_t MaxLogLines = 200;
    std::size_t MaxAuditEvents = 50;
};

// Volatile context snapshot, gathered once on the worker thread. BuildMarkdownBody
// is a pure function of (options, bundle) so it can be unit-tested with a
// hand-built bundle.
struct ContextBundle {
    nlohmann::json Env = nlohmann::json::object();        // version/build/os/arch/tracker/utc + redacted env summary
    std::vector<std::string> LogLines;                    // newest-last, already trimmed to MaxLogLines
    nlohmann::json AuditEvents = nlohmann::json::array(); // already redacted by BackendAuditTrail::RedactJson
};

// Resolved dev-repo destination + assets target + PAT. Produced by the pure
// resolver below; consumed by SubmitBugReport.
struct ResolvedBugTarget {
    bool Ok = false;
    std::string Error;
    // Relay mode — when UseRelay is true the client POSTs to RelayUrl and the
    // direct GitHub fields below are unused (no token needed on the client).
    bool UseRelay = false;
    std::string RelayUrl, RelayKey;
    std::string Owner, Repo, BaseUrl, Pat;
    std::string AssetsOwner, AssetsRepo; // screenshot upload target (defaults to the issue repo)
};

struct SubmitResult {
    bool Ok = false;
    std::string IssueKey; // owner/repo#N
    std::string Url;      // browse URL
    std::string Error;
    std::string LocalStageDir; // set when a screenshot degraded to local save
};

// ---- pure (BugReportBody.cpp — linked into the doctest rig) ----

/// Resolve the dev-repo target + PAT + assets repo from config. `envToken` is the
/// caller-supplied value of $SMATCHET_BUGREPORT_GITHUB_TOKEN (empty if unset) —
/// passed in (not read here) so the resolver stays pure/testable. PAT order:
/// envToken -> cfg.BugReportGitHubPat (a DEDICATED bug-report token only). The
/// tracker GitHub PAT (cfg.GitHubPat) is intentionally NEVER used. baseUrl order:
/// BugReportGitHubBaseUrl -> GitHubBaseUrl -> https://api.github.com, then
/// validated with IsValidGitHubBaseUrl. Relay mode (BugReportRelayUrl set) needs
/// no client token. Missing owner/repo, no PAT, or invalid baseUrl -> Ok=false.
ResolvedBugTarget ResolveBugReportTarget(const TrackerConfig& cfg, const std::string& envToken);

/// Build the GitHub-flavored markdown issue body from a gathered bundle. Pure;
/// applies log redaction; emits a header table + <details> log/audit/env blocks;
/// hard-caps total length at ~60KB (GitHub body limit is 65536) and truncates the
/// log slice with a "… (truncated)" marker. `screenshotMarkdown`, when non-empty,
/// is inserted verbatim (e.g. `![screenshot](url)` or a local-path note).
std::string BuildMarkdownBody(const BugReportOptions& opts, const ContextBundle& bundle,
                              const std::string& screenshotMarkdown);

/// Build the "[Bug] <first description line>" issue title, capped UTF-8-safely
/// under GitHub's 256-char title limit (a longer title 422s the create — #989).
/// Pure; falls back to "Report from Smatchet" on an empty description.
std::string BuildIssueTitle(const std::string& userDescription);

/// Truncate `s` to at most `cap` bytes without splitting a UTF-8 sequence
/// (backs off continuation bytes). A byte-level resize can produce invalid
/// UTF-8 that GitHub rejects with 422 Validation Failed (#989). Pure.
void TruncateUtf8(std::string& s, std::size_t cap);

/// Build the JSON payload POSTed to the bug-report relay (`tools/bug-report-relay`).
/// Pure. `screenshotBase64` is included only when non-empty. The relay re-files
/// the issue + uploads the screenshot server-side, so no GitHub token is needed
/// on the client.
nlohmann::json BuildRelayRequest(const std::string& title, const std::string& body, const std::string& screenshotBase64,
                                 bool censored, const std::string& dumpBase64 = std::string(),
                                 const std::string& dumpName = std::string());

// ---- heavy (BugReportService.cpp — NOT linked into the doctest rig) ----

/// Gather runtime context: app version, build tag, OS/arch, active tracker, UTC,
/// the recent log tail (trimmed), and recent audit events (redacted).
ContextBundle GatherContext(const IAppMeta& app, const BugReportOptions& opts);

/// Blocking submit — construct a dedicated GitHubClient against the dev repo and
/// run IssueCreatePipeline. Worker-thread only (blocks on HTTP). Uploads the
/// screenshot (if any) via the Contents API, degrading to local stage on failure.
SubmitResult SubmitBugReport(IAppMeta& app, const BugReportOptions& opts);

} // namespace diagnostics
} // namespace smatchet

#endif // SMATCHET_DIAGNOSTICS_BUG_REPORT_SERVICE_H
