// BugReportService — the HEAVY half: context gather + dedicated-destination
// submit + screenshot upload. Links cpr / AppController / host symbols, so the
// doctest rig deliberately does NOT compile this TU (it links BugReportBody.cpp
// for the pure pieces). docs/plans/active/log-a-bug-github.md.

#include "BugReportService.h"

#include "AppController.h"
#include "BackendAuditTrail.h"
#include "GitHubClient.h"
#include "GitHubClientHelpers.h"
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "Logger.h"
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

// GitHub REST headers for the bug-report dev client (Bearer PAT). Mirrors
// GitHubClient's internal BuildGitHubHeaders (private to that TU).
cpr::Header BugReportGitHubHeaders(const std::string& pat) {
    return cpr::Header{
        {"Authorization", std::string("Bearer ") + pat},
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", "Smatchet-BugReport"},
    };
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

const char* HostOsName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

const char* HostArchName() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return sizeof(void*) == 8 ? "64-bit" : "32-bit";
#endif
}

std::string Base64EncodeBytes(const std::vector<unsigned char>& in) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(in[i]) << 16) |
                                (static_cast<std::uint32_t>(in[i + 1]) << 8) | static_cast<std::uint32_t>(in[i + 2]);
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back(tbl[(n >> 6) & 0x3F]);
        out.push_back(tbl[n & 0x3F]);
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        const std::uint32_t n = static_cast<std::uint32_t>(in[i]) << 16;
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const std::uint32_t n =
            (static_cast<std::uint32_t>(in[i]) << 16) | (static_cast<std::uint32_t>(in[i + 1]) << 8);
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back(tbl[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
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
            defaultBranch = nlohmann::json::parse(repoMeta.text).value("default_branch", std::string());
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
            headSha = nlohmann::json::parse(baseRef.text)["object"].value("sha", std::string());
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
        const nlohmann::json j = nlohmann::json::parse(resp.text);
        if (j.contains("content") && j["content"].contains("download_url")) {
            return j["content"].value("download_url", std::string());
        }
    } catch (const std::exception&) {
        // fall through
    }
    return "";
}

std::string FirstLine(const std::string& s) {
    const std::size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

} // namespace

ContextBundle GatherContext(const AppController& app, const BugReportOptions& opts) {
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
    env["arch"] = HostArchName();
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
    // but redact again defensively.
    std::string auditErr;
    const std::vector<nlohmann::json> events = BackendAuditTrail::ReadRecentEvents(opts.MaxAuditEvents, &auditErr);
    nlohmann::json auditArr = nlohmann::json::array();
    for (const nlohmann::json& ev : events) {
        auditArr.push_back(BackendAuditTrail::RedactJson(ev));
    }
    bundle.AuditEvents = auditArr;

    return bundle;
}

SubmitResult SubmitBugReport(AppController& app, const BugReportOptions& opts) {
    SubmitResult result;
    const TrackerConfig cfg = ConfigManager::Load();

    const char* envTok = std::getenv("SMATCHET_BUGREPORT_GITHUB_TOKEN");
    const ResolvedBugTarget target = ResolveBugReportTarget(cfg, envTok ? std::string(envTok) : std::string());
    if (!target.Ok) {
        result.Error = target.Error;
        return result;
    }

    const ContextBundle bundle = GatherContext(app, opts);

    // Screenshot: upload + inline-embed, degrading to local stage on failure.
    std::string screenshotMarkdown;
    if (opts.IncludeScreenshot && !opts.ScreenshotAbsPath.empty()) {
        const std::string stamp = TimestampStamp();
        const std::string rawUrl = UploadScreenshotAsset(target.BaseUrl, target.Pat, target.AssetsOwner,
                                                         target.AssetsRepo, opts.ScreenshotAbsPath, stamp);
        if (!rawUrl.empty()) {
            screenshotMarkdown = "![screenshot](" + rawUrl + ")";
        } else {
            // Degrade: copy to a local stage dir + note the path in the body.
            std::error_code ec;
            const std::string stageDir = ConfigManager::GetUserDataDirectory() + "bug_reports/" + stamp + "/";
            fs::create_directories(fs::path(stageDir), ec);
            const std::string destName = std::string("screenshot") + (opts.Censored ? "-censored" : "") + ".png";
            fs::copy_file(fs::path(opts.ScreenshotAbsPath), fs::path(stageDir + destName),
                          fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                result.LocalStageDir = stageDir;
                screenshotMarkdown = "_Screenshot saved locally (upload unavailable): `" + destName + "`_";
            } else {
                screenshotMarkdown = "_Screenshot capture present but could not be uploaded or saved._";
                LOG_WARN("BugReport: screenshot local-stage failed: %s", ec.message().c_str());
            }
        }
    }

    const std::string body = BuildMarkdownBody(opts, bundle, screenshotMarkdown);

    IssueDraft draft;
    draft.ProjectKey = target.Owner + "/" + target.Repo;
    const std::string firstLine = FirstLine(opts.UserDescription);
    draft.FieldValues["summary"] = std::string("[Bug] ") + (firstLine.empty() ? "Report from Smatchet" : firstLine);
    draft.FieldValues["description"] = body;

    GitHubClient devClient(target.BaseUrl, target.Pat);

    RequiredFieldSet required;          // GitHub has no issue type; do NOT read the active backend's metadata.
    required.RequiresIssueType = false; // FieldIds intentionally empty.

    const IssueCreateResult created = IssueCreatePipeline::Run(devClient, /*cache*/ nullptr, draft, required, {});
    if (!created.Ok) {
        result.Error = created.Error.empty() ? "Bug report submit failed" : created.Error;
        return result;
    }

    result.Ok = true;
    result.IssueKey = created.IssueKey;
    // Browse URL — built directly from the key, NOT app.BuildIssueBrowseUrl (which
    // targets the ACTIVE backend's cfg and would be wrong for this foreign repo).
    smatchet::github::ParsedIssueKey parsed;
    if (smatchet::github::ParseGitHubIssueKey(created.IssueKey, parsed)) {
        // api.github.com -> github.com; Enterprise "https://host/api/v3" -> "https://host".
        std::string host = "https://github.com";
        if (target.BaseUrl != "https://api.github.com") {
            const std::string apiSuffix = "/api/v3";
            if (target.BaseUrl.size() > apiSuffix.size() &&
                target.BaseUrl.compare(target.BaseUrl.size() - apiSuffix.size(), apiSuffix.size(), apiSuffix) == 0) {
                host = target.BaseUrl.substr(0, target.BaseUrl.size() - apiSuffix.size());
            }
        }
        result.Url = host + "/" + parsed.Owner + "/" + parsed.Repo + "/issues/" + std::to_string(parsed.Number);
    }
    return result;
}

} // namespace diagnostics
} // namespace smatchet
