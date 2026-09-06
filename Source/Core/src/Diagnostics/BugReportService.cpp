// BugReportService — the HEAVY half: context gather + dedicated-destination
// submit + screenshot upload. Links cpr / AppController / host symbols, so the
// doctest rig deliberately does NOT compile this TU (it links BugReportBody.cpp
// for the pure pieces). docs/plans/shipped/log-a-bug-github.md.

#include "BugReportService.h"

#include "GitHubRestHeaders.h"

#include "Interfaces/IAppMeta.h"
#include "BackendAuditTrail.h"
#include "Diagnostics/HostMachineInfo.h"
#include "GitHubClient.h"
#include "GitHubClientHelpers.h"
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "StringUtil.h" // Base64Encode — single-sourced base64
#include "TextRedaction.h"
#include "TrackerHttpUtils.h"
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
#include "Ui/SmatchetImGuiHostC.h"
#endif

#include <cpr/cpr.h>
#include <ghc/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace fs = ghc::filesystem;

namespace smatchet {
namespace diagnostics {

namespace {

const char* const kAssetsBranch = "bug-report-assets";

// GitHub REST headers for the bug-report dev client (shared recipe, bug-report User-Agent).
cpr::Header BugReportGitHubHeaders(const std::string& pat) {
    return smatchet::github::GitHubRestHeaders(pat, "Smatchet-BugReport");
}

std::string UtcNowIso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

std::string TimestampStamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    return std::string(buf);
}

// Byte-buffer front-end for StringUtil.h's Base64Encode. This TU used to carry a third,
// independently-written RFC 4648 encoder — same output, different code, so the duplication
// gate (which detects copy-paste, not semantic twins) never saw it.
std::string Base64EncodeBytes(const std::vector<unsigned char>& in) {
    return Base64Encode(std::string(in.begin(), in.end()));
}

bool ReadFileBytes(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz <= 0) {
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return static_cast<bool>(f);
}

// Best-effort ensure the dedicated assets branch exists. Returns true if it
// exists (or was created). Any failure -> false (caller degrades to local save).
bool EnsureAssetsBranch(const std::string& baseUrl, const std::string& pat, const std::string& owner,
                        const std::string& repo) {
    const cpr::Header headers = BugReportGitHubHeaders(pat);
    const std::string repoUrl = baseUrl + "/repos/" + owner + "/" + repo;

    const cpr::Response existing = TrackerGetLogged("BugReport", repoUrl + "/git/ref/heads/" + kAssetsBranch, headers);
    if (existing.status_code == 200) {
        return true;
    }
    if (existing.status_code != 404) {
        LOG_WARN("BugReport: assets-branch probe HTTP %ld", existing.status_code);
        return false;
    }

    // Resolve default branch + its head SHA, then create refs/heads/<assets>.
    std::string defaultBranch;
    {
        const cpr::Response repoMeta = TrackerGetLogged("BugReport", repoUrl, headers);
        if (repoMeta.status_code != 200) {
            return false;
        }
        try {
            std::string parseErr;
            const nlohmann::json j = smatchet::json_safe::ParseBounded(repoMeta.text, parseErr);
            if (!parseErr.empty()) {
                return false;
            }
            defaultBranch = j.value("default_branch", std::string());
        } catch (const std::exception&) {
            return false;
        }
    }
    if (defaultBranch.empty()) {
        return false;
    }
    std::string headSha;
    {
        const cpr::Response baseRef =
            TrackerGetLogged("BugReport", repoUrl + "/git/ref/heads/" + defaultBranch, headers);
        if (baseRef.status_code != 200) {
            return false;
        }
        try {
            std::string parseErr;
            const nlohmann::json j = smatchet::json_safe::ParseBounded(baseRef.text, parseErr);
            if (!parseErr.empty()) {
                return false;
            }
            // CPP_CODE_AUDIT.md #33: index-then-value on unvalidated network JSON — a response
            // missing "object" (or where it's not an object) previously threw type_error, caught
            // below and degraded to local staging (not a crash, but fragile vs guarded siblings).
            if (j.contains("object") && j["object"].is_object()) {
                headSha = j["object"].value("sha", std::string());
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    if (headSha.empty()) {
        return false;
    }
    nlohmann::json createBody = nlohmann::json::object();
    createBody["ref"] = std::string("refs/heads/") + kAssetsBranch;
    createBody["sha"] = headSha;
    const cpr::Response created = TrackerPostLogged("BugReport", repoUrl + "/git/refs", headers, createBody.dump());
    if (created.status_code == 201) {
        return true;
    }
    LOG_WARN("BugReport: create assets-branch HTTP %ld", created.status_code);
    return false;
}

// Upload a PNG via the Contents API to the assets branch. Returns the rendered
// raw/download URL on success, empty on any failure.
std::string UploadScreenshotAsset(const std::string& baseUrl, const std::string& pat, const std::string& owner,
                                  const std::string& repo, const std::string& localPath, const std::string& stamp) {
    if (!EnsureAssetsBranch(baseUrl, pat, owner, repo)) {
        return "";
    }
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(localPath, bytes)) {
        LOG_WARN("BugReport: cannot read screenshot for upload: %s", localPath.c_str());
        return "";
    }
    const std::string repoPath = std::string("bug-assets/") + stamp + ".png";
    const std::string url = baseUrl + "/repos/" + owner + "/" + repo + "/contents/" + repoPath;
    nlohmann::json body = nlohmann::json::object();
    body["message"] = std::string("bug-report screenshot ") + stamp;
    body["content"] = Base64EncodeBytes(bytes);
    body["branch"] = kAssetsBranch;
    const cpr::Response resp = TrackerPutLogged("BugReport", url, BugReportGitHubHeaders(pat), body.dump());
    if (resp.status_code != 201 && resp.status_code != 200) {
        LOG_WARN("BugReport: screenshot upload HTTP %ld", resp.status_code);
        return "";
    }
    try {
        std::string parseErr;
        const nlohmann::json j = smatchet::json_safe::ParseBounded(resp.text, parseErr);
        if (!parseErr.empty()) {
            return "";
        }
        if (j.contains("content") && j["content"].contains("download_url")) {
            return j["content"].value("download_url", std::string());
        }
    } catch (const std::exception&) {
        // fall through
    }
    return "";
}

// Upload a minidump as a GitHub Release asset (binaries off the git tree).
// Ensures a `crash-dumps` prerelease exists, then uploads the .dmp. Returns the
// browser download URL, or "" on any failure. cloud + Enterprise both work — the
// upload host comes from the release JSON's `upload_url`.
std::string UploadCrashDumpRelease(const std::string& baseUrl, const std::string& pat, const std::string& owner,
                                   const std::string& repo, const std::string& dumpPath, const std::string& dumpName) {
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(dumpPath, bytes)) {
        LOG_WARN("BugReport: cannot read crash dump for upload: %s", dumpPath.c_str());
        return "";
    }
    const cpr::Header headers = BugReportGitHubHeaders(pat);
    const std::string relBase = baseUrl + "/repos/" + owner + "/" + repo + "/releases";

    // Ensure the crash-dumps release exists; capture its upload_url.
    std::string uploadUrl;
    {
        const cpr::Response existing = TrackerGetLogged("BugReport", relBase + "/tags/crash-dumps", headers);
        if (existing.status_code == 200) {
            try {
                std::string parseErr;
                const nlohmann::json j = smatchet::json_safe::ParseBounded(existing.text, parseErr);
                if (parseErr.empty()) {
                    uploadUrl = j.value("upload_url", std::string());
                }
            } catch (const std::exception&) {
            }
        } else if (existing.status_code == 404) {
            nlohmann::json create = nlohmann::json::object();
            create["tag_name"] = "crash-dumps";
            create["name"] = "Crash dumps";
            create["body"] = "Minidumps attached automatically by the Smatchet crash reporter.";
            create["prerelease"] = true;
            const cpr::Response made = TrackerPostLogged("BugReport", relBase, headers, create.dump());
            if (made.status_code == 201) {
                try {
                    std::string parseErr;
                    const nlohmann::json j = smatchet::json_safe::ParseBounded(made.text, parseErr);
                    if (parseErr.empty()) {
                        uploadUrl = j.value("upload_url", std::string());
                    }
                } catch (const std::exception&) {
                }
            } else {
                LOG_WARN("BugReport: create crash-dumps release HTTP %ld", made.status_code);
            }
        }
    }
    if (uploadUrl.empty()) {
        return "";
    }
    // upload_url is a URI template ".../assets{?name,label}" — strip the template + add name.
    const std::size_t brace = uploadUrl.find('{');
    if (brace != std::string::npos) {
        uploadUrl = uploadUrl.substr(0, brace);
    }
    uploadUrl += "?name=" + UrlEncode(dumpName); // encode spaces / specials in the filename

    cpr::Header up = headers;
    up["Content-Type"] = "application/octet-stream";
    const std::string bin(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const cpr::Response resp = TrackerPostLogged("BugReport", uploadUrl, up, bin);
    if (resp.status_code != 201 && resp.status_code != 200) {
        LOG_WARN("BugReport: crash-dump asset upload HTTP %ld", resp.status_code);
        return "";
    }
    try {
        std::string parseErr;
        const nlohmann::json j = smatchet::json_safe::ParseBounded(resp.text, parseErr);
        if (!parseErr.empty()) {
            return "";
        }
        return j.value("browser_download_url", std::string());
    } catch (const std::exception&) {
        return "";
    }
}

// Relay path — POST the report to the server-side relay (tools/bug-report-relay).
// The relay holds the GitHub token and does the issue create + screenshot upload,
// so no GitHub credential is needed on the client.
SubmitResult SubmitViaRelay(const ResolvedBugTarget& target, const BugReportOptions& opts,
                            const ContextBundle& bundle) {
    SubmitResult result;

    // The relay embeds the screenshot server-side, so the body carries no image
    // markdown here. WYSIWYG: a user-edited preview overrides the generated body.
    const std::string body =
        opts.BodyOverride.empty() ? BuildMarkdownBody(opts, bundle, std::string()) : opts.BodyOverride;
    const std::string title = BuildIssueTitle(opts.UserDescription);

    std::string screenshotBase64;
    if (opts.IncludeScreenshot && !opts.ScreenshotAbsPath.empty()) {
        std::vector<unsigned char> bytes;
        if (ReadFileBytes(opts.ScreenshotAbsPath, bytes)) {
            screenshotBase64 = Base64EncodeBytes(bytes);
        } else {
            LOG_WARN("BugReport: cannot read screenshot for relay: %s", opts.ScreenshotAbsPath.c_str());
        }
    }

    // Phase-2 crash path. The relay uploads the minidump server-side as a Release asset.
    std::string dumpBase64, dumpName;
    if (!opts.DumpAbsPath.empty()) {
        std::vector<unsigned char> bytes;
        if (ReadFileBytes(opts.DumpAbsPath, bytes)) {
            dumpBase64 = Base64EncodeBytes(bytes);
            dumpName = fs::path(opts.DumpAbsPath).filename().string();
        } else {
            LOG_WARN("BugReport: cannot read crash dump for relay: %s", opts.DumpAbsPath.c_str());
        }
    }

    const nlohmann::json payload =
        BuildRelayRequest(title, body, screenshotBase64, opts.Censored, dumpBase64, dumpName);

    cpr::Header headers{{"Content-Type", "application/json"}, {"User-Agent", "Smatchet-BugReport"}};
    if (!target.RelayKey.empty()) {
        headers["x-relay-key"] = target.RelayKey;
    }
    const cpr::Response resp = TrackerPostLogged("BugReportRelay", target.RelayUrl, headers, payload.dump());

    if (resp.status_code < 200 || resp.status_code >= 300) {
        std::string msg = "HTTP " + std::to_string(resp.status_code);
        try {
            std::string parseErr;
            const nlohmann::json j = smatchet::json_safe::ParseBounded(resp.text, parseErr);
            if (parseErr.empty() && j.contains("error") && j["error"].is_string()) {
                msg = j.value("error", msg);
            }
        } catch (const std::exception&) {
            // keep the HTTP-status message
        }
        result.Error = "Bug-report relay failed: " + msg;
        return result;
    }

    try {
        std::string parseErr;
        const nlohmann::json j = smatchet::json_safe::ParseBounded(resp.text, parseErr);
        if (!parseErr.empty()) {
            LOG_WARN("BugReportService: relay response parse failed: %s", parseErr.c_str());
            result.Error = "The bug-report relay returned an unexpected response — try again in a few minutes.";
            return result;
        }
        if (!j.value("ok", false)) {
            result.Error = "Bug-report relay rejected: " + j.value("error", std::string("unknown error"));
            return result;
        }
        result.IssueKey = j.value("issueKey", std::string());
        result.Url = j.value("url", std::string());
        if (result.IssueKey.empty()) {
            // Defend against a malformed relay reply ({ok:true} with no key) — never
            // report a false-positive "submitted".
            result.Error = "Bug-report relay returned success but no issue key.";
            return result;
        }
        result.Ok = true;
        return result;
    } catch (const std::exception& ex) {
        LOG_WARN("BugReportService: relay response parse failed: %s", ex.what());
        result.Error = "The bug-report relay returned an unexpected response — try again in a few minutes.";
        return result;
    }
}

// Upload + inline-embed the screenshot, degrading to a local stage dir on upload failure.
// Returns the markdown to embed; on local-stage success sets result.LocalStageDir.
std::string BuildScreenshotMarkdown(const ResolvedBugTarget& target, const BugReportOptions& opts,
                                    SubmitResult& result) {
    if (!opts.IncludeScreenshot || opts.ScreenshotAbsPath.empty()) {
        return std::string();
    }
    const std::string stamp = TimestampStamp();
    const std::string rawUrl = UploadScreenshotAsset(target.BaseUrl, target.Pat, target.AssetsOwner, target.AssetsRepo,
                                                     opts.ScreenshotAbsPath, stamp);
    if (!rawUrl.empty()) {
        return "![screenshot](" + rawUrl + ")";
    }
    // Degrade: copy to a local stage dir + note the path in the body.
    std::error_code ec;
    const std::string stageDir = ConfigManager::GetUserDataDirectory() + "bug_reports/" + stamp + "/";
    fs::create_directories(fs::path(stageDir), ec);
    const std::string destName = std::string("screenshot") + (opts.Censored ? "-censored" : "") + ".png";
    fs::copy_file(fs::path(opts.ScreenshotAbsPath), fs::path(stageDir + destName), fs::copy_options::overwrite_existing,
                  ec);
    if (!ec) {
        result.LocalStageDir = stageDir;
        return "_Screenshot saved locally (upload unavailable): `" + destName + "`_";
    }
    LOG_WARN("BugReport: screenshot local-stage failed: %s", ec.message().c_str());
    return "_Screenshot capture present but could not be uploaded or saved._";
}

// Build the github.com browse URL for a created issue key, mapping the API base URL back to the
// web host. The public api host maps to github.com and an Enterprise api/v3 base maps to its host.
std::string BuildBugReportBrowseUrl(const ResolvedBugTarget& target, const std::string& issueKey) {
    smatchet::github::ParsedIssueKey parsed;
    if (!smatchet::github::ParseGitHubIssueKey(issueKey, parsed)) {
        return std::string();
    }
    std::string host = "https://github.com";
    if (target.BaseUrl != "https://api.github.com") {
        const std::string apiSuffix = "/api/v3";
        if (target.BaseUrl.size() > apiSuffix.size() &&
            target.BaseUrl.compare(target.BaseUrl.size() - apiSuffix.size(), apiSuffix.size(), apiSuffix) == 0) {
            host = target.BaseUrl.substr(0, target.BaseUrl.size() - apiSuffix.size());
        }
    }
    return host + "/" + parsed.Owner + "/" + parsed.Repo + "/issues/" + std::to_string(parsed.Number);
}

} // namespace

ContextBundle GatherContext(const IAppMeta& app, const BugReportOptions& opts) {
    ContextBundle bundle;
    const TrackerConfig cfg = ConfigManager::Load();

    nlohmann::json env = nlohmann::json::object();
    env["version"] = app.GetAppVersion();
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
    // The packaged-host build tag only links into SmatchetImGuiHost_DX12 (Unreal).
    const char* buildTag = SmatchetHost_GetBuildTag();
    env["build_tag"] = buildTag ? buildTag : "";
#else
    env["build_tag"] = std::string("standalone | built ") + __DATE__ + " " + __TIME__;
#endif
    env["os"] = HostOsName();
    env["arch"] = HostArchName(); // compile-time build target
    // Native silicon + emulation telemetry (e.g. x86_64 build under Prism on an
    // arm64 host). Omitted entirely when unresolvable (pre-1709 / non-Windows).
    const HostMachineInfo machine = DetectHostMachine();
    if (machine.Resolved) {
        env["emulated"] = machine.Emulated;
        if (machine.NativeArch[0] != '\0') {
            env["host_arch"] = machine.NativeArch;
        }
    }
    env["tracker"] = cfg.TrackerType.empty() ? std::string("(none)") : cfg.TrackerType;
    env["utc"] = UtcNowIso8601();

    // Small redacted env summary (no secrets — keys run through RedactJson anyway).
    nlohmann::json summary = nlohmann::json::object();
    summary["user_data_dir"] = ConfigManager::GetUserDataDirectory();
    summary["read_only_mode"] = cfg.ReadOnlyMode;
    summary["backend_reachable"] = cfg.BackendHasBeenReachable;
    env["summary"] = BackendAuditTrail::RedactJson(summary);
    bundle.Env = env;

    // Recent log tail — newest-last, trimmed to MaxLogLines.
    const std::vector<LogEntry> entries = Logger::Instance().GetEntriesSnapshot();
    const std::size_t keep = opts.MaxLogLines == 0 ? entries.size() : opts.MaxLogLines;
    const std::size_t start = entries.size() > keep ? entries.size() - keep : 0;
    for (std::size_t i = start; i < entries.size(); ++i) {
        std::ostringstream line;
        line << "[" << Logger::LogLevelToString(entries[i].level) << "] " << entries[i].message;
        bundle.LogLines.push_back(line.str());
    }

    // Recent audit events — already redacted by ReadRecentEvents' RedactJson pass,
    // but redact again defensively. A read error is non-fatal for a bug bundle: fall
    // back to no audit events (the same empty-vector outcome as before).
    const std::vector<nlohmann::json> events = BackendAuditTrail::ReadRecentEvents(opts.MaxAuditEvents).value_or({});
    nlohmann::json auditArr = nlohmann::json::array();
    for (const nlohmann::json& ev : events) {
        auditArr.push_back(BackendAuditTrail::RedactJson(ev));
    }
    bundle.AuditEvents = auditArr;

    return bundle;
}

SubmitResult SubmitBugReport(IAppMeta& app, const BugReportOptions& opts) {
    SubmitResult result;
    const TrackerConfig cfg = ConfigManager::Load();

    const char* envTok = std::getenv("SMATCHET_BUGREPORT_GITHUB_TOKEN");
    const ResolvedBugTarget target = ResolveBugReportTarget(cfg, envTok ? std::string(envTok) : std::string());
    if (!target.Ok) {
        result.Error = target.Error;
        return result;
    }

    const ContextBundle bundle = GatherContext(app, opts);

    // Relay mode — POST the report to the server-side relay (it holds the GitHub
    // token + does the issue create / screenshot upload). No client-side token.
    if (target.UseRelay) {
        return SubmitViaRelay(target, opts, bundle);
    }

    // Screenshot: upload + inline-embed, degrading to local stage on failure.
    const std::string screenshotMarkdown = BuildScreenshotMarkdown(target, opts, result);

    // Phase-2 crash path. Upload the minidump as a Release asset and link it in the body.
    std::string dumpMarkdown;
    if (!opts.DumpAbsPath.empty()) {
        const std::string dumpName = fs::path(opts.DumpAbsPath).filename().string();
        const std::string url = UploadCrashDumpRelease(target.BaseUrl, target.Pat, target.AssetsOwner,
                                                       target.AssetsRepo, opts.DumpAbsPath, dumpName);
        dumpMarkdown = url.empty() ? ("_Crash minidump at `" + opts.DumpAbsPath + "` (upload failed)._")
                                   : ("[Crash minidump](" + url + ")");
    }

    // WYSIWYG: when the user edited the egress preview, that text IS the body; we
    // still append the uploaded-screenshot + minidump markdown so they render.
    std::string body;
    if (!opts.BodyOverride.empty()) {
        body = opts.BodyOverride;
        if (!screenshotMarkdown.empty()) {
            body += "\n\n" + screenshotMarkdown;
        }
    } else {
        body = BuildMarkdownBody(opts, bundle, screenshotMarkdown);
    }
    if (!dumpMarkdown.empty()) {
        body += "\n\n" + dumpMarkdown;
    }

    IssueDraft draft;
    draft.ProjectKey = target.Owner + "/" + target.Repo;
    draft.FieldValues["summary"] = BuildIssueTitle(opts.UserDescription);
    draft.FieldValues["description"] = body;

    GitHubClient devClient(target.BaseUrl, target.Pat);

    RequiredFieldSet required;          // GitHub has no issue type; do NOT read the active backend's metadata.
    required.RequiresIssueType = false; // FieldIds intentionally empty.

    const IssueCreateResult created =
        IssueCreatePipeline::Run(devClient, /*cache*/ nullptr, /*cacheBackendKey*/ std::string(), draft, required, {});
    if (!created.Ok) {
        result.Error = created.Error.empty() ? "Bug report submit failed" : created.Error;
        return result;
    }

    result.Ok = true;
    result.IssueKey = created.IssueKey;
    // Browse URL — built directly from the key, NOT app.BuildIssueBrowseUrl (which
    // targets the ACTIVE backend's cfg and would be wrong for this foreign repo).
    result.Url = BuildBugReportBrowseUrl(target, created.IssueKey);
    return result;
}

} // namespace diagnostics
} // namespace smatchet
