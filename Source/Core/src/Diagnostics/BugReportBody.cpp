// BugReportBody — the PURE half of BugReportService: dev-repo/PAT resolution +
// markdown body assembly. No cpr / no host symbols / no AppController, so the
// doctest rig links this TU directly. The heavy gather/submit half lives in
// BugReportService.cpp. docs/plans/shipped/log-a-bug-github.md.

#include "BugReportService.h"

#include "GitHubClientHelpers.h"
#include "TextRedaction.h"

#include <sstream>

namespace smatchet {
namespace diagnostics {

namespace {

// GitHub rejects issue bodies over 65536 bytes. Leave headroom for the closing
// markdown we always append after the (potentially truncated) log block.
const std::size_t kBodyHardCap = 60000;

// Split "owner/repo" -> {owner, repo}. Returns false on malformed input.
bool SplitOwnerRepo(const std::string& s, std::string& owner, std::string& repo) {
    const std::size_t slash = s.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size()) {
        return false;
    }
    owner = s.substr(0, slash);
    repo = s.substr(slash + 1);
    return owner.find('/') == std::string::npos && repo.find('/') == std::string::npos;
}

} // namespace

ResolvedBugTarget ResolveBugReportTarget(const TrackerConfig& cfg, const std::string& envToken) {
    ResolvedBugTarget out;

    // Relay mode (preferred for external distribution) — the server holds the
    // token, so the client needs no owner/repo/PAT. Overrides the direct path.
    if (!cfg.BugReportRelayUrl.empty()) {
        out.UseRelay = true;
        out.RelayUrl = cfg.BugReportRelayUrl;
        out.RelayKey = cfg.BugReportRelayKey;
        out.Ok = true;
        return out;
    }

    if (cfg.BugReportGitHubOwner.empty() || cfg.BugReportGitHubRepo.empty()) {
        out.Error = "Bug-report dev repo is not configured (set Preferences > Bug Report > owner/repo).";
        return out;
    }
    out.Owner = cfg.BugReportGitHubOwner;
    out.Repo = cfg.BugReportGitHubRepo;

    // PAT: env -> opt-in persisted bug-report PAT ONLY. The bug reporter must NEVER
    // borrow the user's tracker GitHub PAT (cfg.GitHubPat) — that is a separate,
    // personal credential for a different purpose; conflating them would file under
    // the wrong identity / leak its scope. Dedicated token, or use relay mode.
    if (!envToken.empty()) {
        out.Pat = envToken;
    } else if (!cfg.BugReportGitHubPat.empty()) {
        out.Pat = cfg.BugReportGitHubPat;
    }
    if (out.Pat.empty()) {
        out.Error = "No bug-report GitHub token configured (set $SMATCHET_BUGREPORT_GITHUB_TOKEN, a dedicated "
                    "BugReportGitHubPat, or use relay mode). The tracker GitHub PAT is intentionally NOT used.";
        return out;
    }

    // baseUrl: bug-report override -> tracker GitHub base URL -> cloud default.
    std::string baseUrl = cfg.BugReportGitHubBaseUrl;
    if (baseUrl.empty()) {
        baseUrl = cfg.GitHubBaseUrl;
    }
    if (baseUrl.empty()) {
        baseUrl = "https://api.github.com";
    }
    std::string baseErr;
    if (!smatchet::github::IsValidGitHubBaseUrl(baseUrl, baseErr)) {
        out.Error = "Bug-report GitHub base URL invalid: " + baseErr;
        return out;
    }
    out.BaseUrl = baseUrl;

    // Assets target: explicit "owner/repo" -> reuse the issue repo.
    if (!cfg.BugReportAssetsRepo.empty()) {
        if (!SplitOwnerRepo(cfg.BugReportAssetsRepo, out.AssetsOwner, out.AssetsRepo)) {
            out.Error = "Bug-report assets repo must be 'owner/repo' (got '" + cfg.BugReportAssetsRepo + "').";
            return out;
        }
    } else {
        out.AssetsOwner = out.Owner;
        out.AssetsRepo = out.Repo;
    }

    out.Ok = true;
    return out;
}

nlohmann::json BuildRelayRequest(const std::string& title, const std::string& body, const std::string& screenshotBase64,
                                 bool censored, const std::string& dumpBase64, const std::string& dumpName) {
    nlohmann::json out = nlohmann::json::object();
    out["title"] = title;
    out["body"] = body;
    out["censored"] = censored;
    if (!screenshotBase64.empty()) {
        out["screenshotBase64"] = screenshotBase64;
    }
    if (!dumpBase64.empty()) {
        out["dumpBase64"] = dumpBase64;
        out["dumpName"] = dumpName.empty() ? std::string("crash.dmp") : dumpName;
    }
    return out;
}

std::string BuildMarkdownBody(const BugReportOptions& opts, const ContextBundle& bundle,
                              const std::string& screenshotMarkdown) {
    const nlohmann::json& env = bundle.Env;
    auto envStr = [&env](const char* key) -> std::string {
        return env.is_object() ? env.value(key, std::string()) : std::string();
    };

    std::ostringstream head;
    head << "## Bug report\n\n";
    head << "| Field | Value |\n|---|---|\n";
    head << "| Version | " << envStr("version") << " |\n";
    head << "| Build | " << envStr("build_tag") << " |\n";
    head << "| OS / Arch | " << envStr("os") << " / " << envStr("arch") << " |\n";
    head << "| Active tracker | " << envStr("tracker") << " |\n";
    head << "| Captured (UTC) | " << envStr("utc") << " |\n\n";
    head << "### Description\n\n";
    const std::string desc = opts.UserDescription.empty() ? "_(no description provided)_" : opts.UserDescription;
    head << desc << "\n\n";
    if (!screenshotMarkdown.empty()) {
        head << screenshotMarkdown << "\n\n";
    }

    // Tail = audit + env blocks (always small relative to the cap).
    std::ostringstream tail;
    if (bundle.AuditEvents.is_array() && !bundle.AuditEvents.empty()) {
        tail << "<details><summary>Recent audit events (" << bundle.AuditEvents.size() << ")</summary>\n\n";
        tail << "```json\n" << bundle.AuditEvents.dump(2) << "\n```\n\n</details>\n\n";
    }
    if (env.is_object() && env.contains("summary")) {
        tail << "<details><summary>Environment</summary>\n\n";
        tail << "```json\n" << env["summary"].dump(2) << "\n```\n\n</details>\n\n";
    }

    const std::string headStr = head.str();
    const std::string tailStr = tail.str();

    // Budget for the redacted log block (the only part we truncate).
    std::size_t used = headStr.size() + tailStr.size();
    const std::string logOpenFmt = "<details><summary>Recent log</summary>\n\n```\n";
    const std::string logClose = "\n```\n\n</details>\n";
    used += logOpenFmt.size() + logClose.size() + 32; // +32 slack for the count text + truncation marker
    std::size_t logBudget = (used < kBodyHardCap) ? (kBodyHardCap - used) : 0;

    // Keep newest lines (end of vector); drop from the front until it fits.
    std::vector<std::string> redacted;
    redacted.reserve(bundle.LogLines.size());
    for (const std::string& line : bundle.LogLines) {
        redacted.push_back(privacy::RedactLogLine(line));
    }
    std::size_t total = 0;
    std::size_t firstKept = redacted.size();
    for (std::size_t i = redacted.size(); i-- > 0;) {
        const std::size_t lineCost = redacted[i].size() + 1; // +newline
        if (total + lineCost > logBudget) {
            break;
        }
        total += lineCost;
        firstKept = i;
    }
    const bool truncated = firstKept > 0;

    std::ostringstream out;
    out << headStr;
    out << "<details><summary>Recent log (" << (redacted.size() - firstKept) << " of " << redacted.size()
        << " lines)</summary>\n\n```\n";
    if (truncated) {
        out << "… (truncated)\n";
    }
    for (std::size_t i = firstKept; i < redacted.size(); ++i) {
        out << redacted[i] << "\n";
    }
    out << "```\n\n</details>\n\n";
    out << tailStr;

    std::string result = out.str();
    if (result.size() > 65536) {
        result.resize(65536); // absolute backstop — should never trip given the cap above
    }
    return result;
}

} // namespace diagnostics
} // namespace smatchet
