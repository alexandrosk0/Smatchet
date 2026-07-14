#include "AttachmentAppUpdateService.h"

#include "IAttachmentAppUpdateDeps.h"
#include "Types/HostCallbacks.h"

#include <algorithm>
#include <array>
// SMATCHET_DEVIATION(rule=duplication; reason=include clone; owner=security-audit; revisit=2026-09-30)
#include <atomic>
// SMATCHET_DEVIATION(rule=duplication; reason=include clone; owner=security-audit; revisit=2026-09-30)
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

// SMATCHET_DEVIATION(rule=duplication; reason=include clone; owner=security-audit; revisit=2026-09-30)
#include "AttachmentMimeUtils.h"
#include "ConfigManager.h"
#include "EnvUtil.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "SemanticVersionPure.h"
#include "StringUtil.h"
#include "TrackerHttpUtils.h"

#include <cpr/cpr.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <wintrust.h> // WinVerifyTrust / WINTRUST_DATA — requires windows.h first
#include <softpub.h>  // WINTRUST_ACTION_GENERIC_VERIFY_V2 — requires wintrust.h first
#elif defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

// File-local helpers moved verbatim from AppController_Attachments.cpp (attachment download path)
// and AppController.cpp (semantic-version + url helpers). All were used only by the eight methods
// relocated into this service.
namespace {
std::string MakeUniqueTempFilePath(const std::string& filename, const std::string& extension);

std::string NormalizeDomain(const std::string& rawDomain) {
    std::string value = TrimCopyAsciiWhitespace(rawDomain);
    const size_t schemeSep = value.find("://");
    if (schemeSep != std::string::npos) {
        value = value.substr(schemeSep + 3);
    }
    const size_t slashPos = value.find('/');
    if (slashPos != std::string::npos) {
        value.resize(slashPos);
    }
    const size_t atPos = value.rfind('@');
    if (atPos != std::string::npos) {
        value = value.substr(atPos + 1);
    }
    const size_t colonPos = value.find(':');
    if (colonPos != std::string::npos) {
        value.resize(colonPos);
    }
    return ToLowerAsciiCopy(value);
}

std::string ExtractHostFromUrl(const std::string& url) {
    const size_t schemeSep = url.find("://");
    if (schemeSep == std::string::npos) {
        return std::string();
    }
    const size_t hostStart = schemeSep + 3;
    const size_t hostEnd = url.find_first_of("/?#", hostStart);
    const std::string hostPort = url.substr(hostStart, hostEnd - hostStart);
    if (hostPort.empty()) {
        return std::string();
    }
    if (hostPort.front() == '[') {
        const size_t closePos = hostPort.find(']');
        if (closePos == std::string::npos) {
            return std::string();
        }
        return ToLowerAsciiCopy(hostPort.substr(1, closePos - 1));
    }
    const size_t colonPos = hostPort.find(':');
    return ToLowerAsciiCopy(colonPos == std::string::npos ? hostPort : hostPort.substr(0, colonPos));
}

bool IsAllowedJiraAttachmentHost(const std::string& host, const std::string& jiraDomain) {
    if (host.empty() || jiraDomain.empty()) {
        return false;
    }
    if (host == jiraDomain) {
        return true;
    }
    const std::string suffix = "." + jiraDomain;
    if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return true;
    }
    return host == "api.media.atlassian.com";
}

#if defined(__APPLE__) || defined(__linux__)
bool LaunchCommandNoShell(const char* exe, const std::string& arg) {
    if (!exe || arg.empty()) {
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        execlp(exe, exe, arg.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
}
#endif

bool DownloadAttachmentToLocalFile(const std::string& url, const std::string& filename, const std::string& mimeType,
                                   std::string& outFilePath, std::string& outMime, std::string& outError) {
    outFilePath.clear();
    outMime.clear();
    outError.clear();
    if (url.empty()) {
        outError = "Attachment URL is empty.";
        return false;
    }

    TrackerConfig cfg = ConfigManager::Load();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira credentials/domain.";
        return false;
    }
    if (url.rfind("https://", 0) != 0) {
        outError = "Attachment URL must use HTTPS.";
        return false;
    }
    const std::string jiraDomain = NormalizeDomain(cfg.Domain);
    const std::string targetHost = ExtractHostFromUrl(url);
    if (!IsAllowedJiraAttachmentHost(targetHost, jiraDomain)) {
        outError = "Attachment host is not allowlisted.";
        return false;
    }

    cpr::Header headers{{"Accept", "*/*"},
                        {"Authorization", BuildTrackerBasicAuthHeader(cfg)},
                        {"User-Agent", "Smatchet/1.0 Attachment-Downloader"}};
    cpr::Redirect redirect(true, false);

    constexpr size_t kMaxAttachmentDownloadBytes = 50u * 1024u * 1024u;
    bool sizeExceeded = false;
    std::string bodyAccum;
    bodyAccum.reserve(64 * 1024);
    cpr::WriteCallback writeCb{[&](std::string data, intptr_t) -> bool {
        if (bodyAccum.size() + data.size() > kMaxAttachmentDownloadBytes) {
            sizeExceeded = true;
            return false;
        }
        bodyAccum.append(data);
        return true;
    }};
    const auto resp =
        cpr::Get(cpr::Url{url}, headers, redirect, writeCb, cpr::ConnectTimeout{5000}, cpr::Timeout{120000});
    if (sizeExceeded) {
        outError = "Attachment exceeds max allowed size.";
        return false;
    }
    // cpr followed redirects (Redirect(true,false)) but only the INITIAL url was allowlisted above.
    // A tracker 30x to an internal host would otherwise be fetched and written to a local file
    // (limited SSRF). Re-validate the effective url libcurl actually landed on before persisting
    // anything, mirroring the post-redirect host recheck in Whisper/ModelDownloader.cpp.
    const std::string finalHost = ExtractHostFromUrl(resp.url.str());
    if (finalHost.empty() || !IsAllowedJiraAttachmentHost(finalHost, jiraDomain)) {
        // Fail closed: an unparseable/empty effective host is as suspect as a non-allowlisted one.
        outError = "Attachment redirected to a non-allowlisted host.";
        return false;
    }
    if (resp.error.code != cpr::ErrorCode::OK || resp.status_code < 200 || resp.status_code >= 300) {
        outError = "Download failed: HTTP " + std::to_string(static_cast<int>(resp.status_code));
        return false;
    }

    outMime = mimeType;
    try {
        auto it = resp.header.find("Content-Type");
        if (it != resp.header.end() && !it->second.empty()) {
            outMime = it->second;
        }
    } catch (...) {
        LOG_DEBUG("DownloadAttachmentToLocalFile: response header parse failed; using provided mime type.");
    }
    if (outMime.empty()) {
        outMime = "application/octet-stream";
    }

    const std::string extension = ExtensionFromMime(outMime);
    outFilePath = MakeUniqueTempFilePath(filename, extension);
    std::ofstream ofs(outFilePath, std::ios::binary);
    if (!ofs.is_open()) {
        outError = "Failed to open local temp file.";
        outFilePath.clear();
        return false;
    }

    ofs.write(bodyAccum.data(), static_cast<std::streamsize>(bodyAccum.size()));
    if (!ofs.good()) {
        outError = "Failed to write downloaded attachment bytes.";
        ofs.close();
        outFilePath.clear();
        return false;
    }
    ofs.close();
    LOG_INFO("DownloadAttachmentToLocalFile: downloaded %zu bytes mime=%s path=%s", bodyAccum.size(), outMime.c_str(),
             outFilePath.c_str());
    return true;
}

std::string GetTempDir() {
    std::string t = smatchet::env::ReadVar("TEMP");
    if (!t.empty()) {
        return t;
    }
    t = smatchet::env::ReadVar("TMP");
    if (!t.empty()) {
        return t;
    }
    return ".";
}

std::string SanitizeFilename(const std::string& name) {
    std::string out = name;
    std::replace_if(
        out.begin(), out.end(),
        [](char ch) {
            return ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '\"' || ch == '<' ||
                   ch == '>' || ch == '|';
        },
        '_');
    if (out.empty())
        return std::string("attachment");
    return out;
}

std::string MakeUniqueTempFilePath(const std::string& filename, const std::string& extension) {
    // Mix time + thread-id + monotonic counter so concurrent downloads from different threads
    // cannot collide even when time resolution is coarser than the download rate.
    static std::atomic<std::uint64_t> s_counter{0};
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const std::size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const std::uint64_t unique = static_cast<std::uint64_t>(now) ^ (static_cast<std::uint64_t>(tid) << 16) ^
                                 s_counter.fetch_add(1, std::memory_order_relaxed);
    const std::string safe = SanitizeFilename(filename);
    std::string base = safe;

    // If caller didn't include an extension, append our derived one.
    if (base.find_last_of('.') == std::string::npos) {
        base += extension;
    }

    const std::string tempDir = GetTempDir();
    if (tempDir.empty())
        return base;

    // Ensure tempDir ends with a path separator.
    if (tempDir.back() != '/' && tempDir.back() != '\\') {
        return tempDir + "/" + std::to_string(unique) + "_" + base;
    }
    return tempDir + std::to_string(unique) + "_" + base;
}

std::string TrimAppUpdateText(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    while (!text.empty() &&
           (text.front() == '\r' || text.front() == '\n' || text.front() == ' ' || text.front() == '\t')) {
        text.erase(text.begin());
    }
    return text;
}

// SemanticVersion + ParseSemanticVersion/CompareSemanticVersion moved to the cpr-free pure seam
// SemanticVersionPure.{h,cpp} so the untrusted release-tag parse (CPP_CODE_AUDIT.md #17) can be
// fuzzed without this service's heavy deps. Aliased in so the call sites below stay unchanged.
using smatchet::version::CompareSemanticVersion;
using smatchet::version::ParseSemanticVersion;
using smatchet::version::SemanticVersion;

std::string FileNameFromUrl(const std::string& url) {
    const size_t slash = url.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= url.size()) {
        return std::string();
    }
    return url.substr(slash + 1);
}

#if defined(_WIN32)
// Verify the downloaded installer's Authenticode signature chain (no UI, no online revocation
// fetch — the machine may be mid-update on a flaky network; a revoked-but-cached cert still
// fails). Returns the raw WinVerifyTrust status; kInstallerTrustOk (0) means the signature is
// valid and chains to a trusted root. The -A path is converted via CP_ACP to match the GetTempPathA
// + ShellExecuteA path handling around it.
long VerifyInstallerAuthenticodeStatus(const std::string& filePath) {
    const int wideLen = MultiByteToWideChar(CP_ACP, 0, filePath.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) {
        // An unconvertible path can't be verified — fail closed with E_INVALIDARG.
        return static_cast<long>(0x80070057UL);
    }
    std::wstring widePath(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, filePath.c_str(), -1, &widePath[0], wideLen);

    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = widePath.c_str();

    WINTRUST_DATA trustData = {};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &trustData);
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trustData);
    return static_cast<long>(status);
}

bool IsUpdateUnsignedOverrideSet() {
    const std::string value =
        ToLowerAsciiCopy(TrimCopyAsciiWhitespace(smatchet::env::ReadVar("SMATCHET_UPDATE_ALLOW_UNSIGNED")));
    return value == "1" || value == "true" || value == "yes";
}
#endif
} // namespace

AttachmentAppUpdateService::AttachmentAppUpdateService(IAttachmentAppUpdateDeps& deps) : deps_(deps) {}

void AttachmentAppUpdateService::ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments) {
    if (attachments.empty()) {
        return;
    }
    if (deps_.Host().AttachmentCollection) {
        deps_.Host().AttachmentCollection(attachments);
        return;
    }

    const AttachmentDescriptor& first = attachments.front();
    if (!first.Url.empty()) {
        OpenAttachment(first.Url, first.Filename, first.MimeType);
    }
}

void AttachmentAppUpdateService::OpenAttachment(const std::string& url, const std::string& filename,
                                                const std::string& mimeType) {
    if (url.empty()) {
        return;
    }

    // If no file-based path exists, fall back to regular URL opening.
    if (!deps_.Host().AttachmentViewer && !deps_.Host().AttachmentPreview) {
        LOG_INFO("OpenAttachment: no attachment handler, opening URL directly.");
        deps_.OpenUrl(url);
        return;
    }

    std::string outFilePath;
    std::string outMime;
    std::string outError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, outError)) {
        LOG_WARN("OpenAttachment: %s; falling back to URL open.", outError.c_str());
        deps_.OpenUrl(url);
        return;
    }
    if (deps_.Host().AttachmentViewer) {
        LOG_INFO("OpenAttachment: dispatching to host attachment viewer.");
        deps_.Host().AttachmentViewer(outFilePath, outMime, filename);
        return;
    }

    if (deps_.Host().AttachmentPreview && IsSupportedImageMime(outMime)) {
        if (deps_.Host().AttachmentPreview(outFilePath, outMime, filename, url)) {
            LOG_INFO("OpenAttachment: queued in-app image preview.");
            return;
        }
        LOG_WARN("OpenAttachment: in-app preview rejected; falling back to URL open.");
    }

    LOG_INFO("OpenAttachment: opening attachment via URL fallback.");
    deps_.OpenUrl(url);
}

void AttachmentAppUpdateService::OpenAttachmentInSystemViewer(const std::string& url, const std::string& filename,
                                                              const std::string& mimeType) {
    if (url.empty()) {
        return;
    }
    std::string outFilePath;
    std::string outMime;
    std::string outError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, outError)) {
        LOG_WARN("OpenAttachmentInSystemViewer: %s; falling back to URL open.", outError.c_str());
        deps_.OpenUrl(url);
        return;
    }
    bool launchOk = false;
#if defined(_WIN32)
    const HINSTANCE shellResult = ShellExecuteA(nullptr, "open", outFilePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    launchOk = reinterpret_cast<intptr_t>(shellResult) > 32;
    if (!launchOk) {
        LOG_ERROR("OpenAttachmentInSystemViewer: ShellExecute failed path=%s err=%lu",
                  TruncateForLog(outFilePath, 300).c_str(), static_cast<unsigned long>(GetLastError()));
    }
#elif defined(__APPLE__)
    launchOk = LaunchCommandNoShell("open", outFilePath);
    if (!launchOk) {
        LOG_ERROR("OpenAttachmentInSystemViewer: open failed path=%s", TruncateForLog(outFilePath, 300).c_str());
    }
#else
    launchOk = LaunchCommandNoShell("xdg-open", outFilePath);
    if (!launchOk) {
        LOG_ERROR("OpenAttachmentInSystemViewer: xdg-open failed path=%s", TruncateForLog(outFilePath, 300).c_str());
    }
#endif
}

bool AttachmentAppUpdateService::DownloadAttachmentForPreview(const std::string& url, const std::string& filename,
                                                              const std::string& mimeType, std::string* outError) {
    auto fail = [outError](const std::string& errorMessage) {
        if (outError != nullptr) {
            *outError = errorMessage;
        }
        return false;
    };
    if (!deps_.Host().AttachmentPreview || !IsSupportedImageMime(mimeType)) {
        return fail(!deps_.Host().AttachmentPreview ? std::string("Preview handler is unavailable.")
                                                    : std::string("Attachment is not a supported image type."));
    }
    std::string outFilePath;
    std::string outMime;
    std::string downloadError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, downloadError)) {
        LOG_WARN("DownloadAttachmentForPreview: %s", downloadError.c_str());
        return fail(downloadError);
    }
    if (!deps_.Host().AttachmentPreview(outFilePath, outMime, filename, url)) {
        LOG_WARN("DownloadAttachmentForPreview: preview handler rejected file=%s mime=%s", filename.c_str(),
                 outMime.c_str());
        return fail("Preview handler rejected the attachment.");
    }
    if (outError != nullptr) {
        outError->clear();
    }
    return true;
}

std::string AttachmentAppUpdateService::GetAppVersion() const {
#ifdef SMATCHET_APP_VERSION
    return SMATCHET_APP_VERSION;
#else
    return "0.0.0";
#endif
}

std::string AttachmentAppUpdateService::GetGitHubReleaseRepo() const {
#ifdef SMATCHET_GITHUB_RELEASE_REPO
    return SMATCHET_GITHUB_RELEASE_REPO;
#else
    return "alexandrosk0/Smatchet";
#endif
}

AppUpdateInfo AttachmentAppUpdateService::CheckForAppUpdate(bool includePrerelease) const {
    AppUpdateInfo out;
    out.CurrentVersion = GetAppVersion();

    const TrackerConfig cfg = ConfigManager::Load();
    const std::string repo = cfg.UpdateGithubRepo.empty() ? GetGitHubReleaseRepo() : cfg.UpdateGithubRepo;
    if (repo.empty()) {
        out.Error = "No GitHub release repository configured.";
        return out;
    }

    const std::string url = "https://api.github.com/repos/" + repo + "/releases?per_page=10";
    cpr::Header headers{{"Accept", "application/vnd.github+json"},
                        {"User-Agent", "SmatchetUpdater/" + out.CurrentVersion}};

    // Bound the response body before it is buffered: a hostile / misconfigured endpoint could
    // otherwise stream an unbounded body straight into memory (OOM) before ParseBounded ever
    // runs. Abort the transfer once the cap is exceeded (mirrors the capped download at :155 and
    // the Tracker attachment proxy in McpPlugin.cpp).
    // SMATCHET_DEVIATION(rule=duplication; reason=capped cpr::WriteCallback body-writer is deliberately
    // inlined per download site so each cap and error string stays local and tunable, per ADR-0015,
    // rather than folded into a shared helper; owner=security-audit; revisit=2026-09-30)
    constexpr size_t kMaxUpdateCheckBytes = 8u * 1024u * 1024u;
    bool sizeExceeded = false;
    std::string bodyAccum;
    bodyAccum.reserve(64 * 1024);
    cpr::WriteCallback writeCb{[&](std::string data, intptr_t) -> bool {
        if (bodyAccum.size() + data.size() > kMaxUpdateCheckBytes) {
            sizeExceeded = true;
            return false;
        }
        bodyAccum.append(data);
        return true;
    }};

    cpr::Response response = cpr::Get(cpr::Url{url}, headers, cpr::Redirect{true, true}, writeCb,
                                      cpr::ConnectTimeout{5000}, cpr::Timeout{15000});
    if (sizeExceeded) {
        out.Error = "Update check failed: GitHub response exceeds the maximum allowed size.";
        return out;
    }
    if (response.error.code != cpr::ErrorCode::OK) {
        out.Error = "Update check failed: " + response.error.message;
        return out;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        out.Error = "Update check failed: GitHub returned HTTP " + std::to_string(response.status_code);
        return out;
    }

    // The GitHub release listing is a remote response; parse it through the
    // depth/node-bounded helper so a hostile / oversized body can't crash the process.
    // ParseBounded never throws; failure is a non-empty parseErr (stable, input-free).
    std::string parseErr;
    nlohmann::json releases = smatchet::json_safe::ParseBounded(bodyAccum, parseErr);
    if (!parseErr.empty()) {
        out.Error = std::string("Update check failed to parse GitHub response: ") + parseErr;
        return out;
    }
    if (!releases.is_array()) {
        out.Error = "Update check failed: unexpected GitHub response shape.";
        return out;
    }

    const SemanticVersion current = ParseSemanticVersion(out.CurrentVersion);
    for (const auto& release : releases) {
        if (!release.is_object()) {
            continue;
        }
        if (release.value("draft", false)) {
            continue;
        }
        if (!includePrerelease && release.value("prerelease", false)) {
            continue;
        }

        const std::string tag = release.value("tag_name", std::string());
        const SemanticVersion candidate = ParseSemanticVersion(tag);
        if (!candidate.Valid) {
            continue;
        }

        out.Ok = true;
        out.ReleaseTag = tag;
        out.LatestVersion = !tag.empty() && tag.front() == 'v' ? tag.substr(1) : tag;
        out.ReleaseUrl = release.value("html_url", std::string());
        out.ReleaseNotes = TrimAppUpdateText(release.value("body", std::string()));

        if (release.contains("assets") && release["assets"].is_array()) {
            for (const auto& asset : release["assets"]) {
                if (!asset.is_object()) {
                    continue;
                }
                const std::string assetName = asset.value("name", std::string());
                if (assetName.find("-windows-setup.exe") != std::string::npos) {
                    out.InstallerAsset.Name = assetName;
                    out.InstallerAsset.DownloadUrl = asset.value("browser_download_url", std::string());
                    break;
                }
            }
        }

        if (current.Valid && CompareSemanticVersion(candidate, current) <= 0) {
            out.UpdateAvailable = false;
            return out;
        }

        out.UpdateAvailable = !out.InstallerAsset.DownloadUrl.empty();
        if (!out.UpdateAvailable && out.Error.empty()) {
            out.Error = "A newer release exists, but no Windows installer asset was found.";
        }
        return out;
    }

    out.Ok = true;
    out.UpdateAvailable = false;
    return out;
}

bool AttachmentAppUpdateService::DownloadAndLaunchInstallerUpdate(const std::string& downloadUrl,
                                                                  const std::string& assetName, std::string& outError,
                                                                  std::shared_ptr<std::atomic<bool>> cancelFlag) const {
    outError.clear();
    if (downloadUrl.empty()) {
        outError = "Missing installer download URL.";
        return false;
    }
#if defined(_WIN32)
    char tempPathBuf[MAX_PATH] = {};
    const DWORD tempPathLen = GetTempPathA(static_cast<DWORD>(sizeof(tempPathBuf)), tempPathBuf);
    if (tempPathLen == 0 || tempPathLen >= sizeof(tempPathBuf)) {
        outError = "Failed to resolve temp directory.";
        return false;
    }

    const std::string rawFilename = assetName.empty() ? FileNameFromUrl(downloadUrl) : assetName;
    // The asset name is attacker-influenced (GitHub release metadata) and is concatenated onto the
    // temp dir below. Sanitize to a bare basename so a name like "..\\..\\...\\Startup\\x-windows-
    // setup.exe" cannot escape %TEMP% and be ShellExecute-launched from an arbitrary location.
    const std::string filename = smatchet::app_update::SanitizeInstallerFilename(rawFilename);
    const std::string localPath = std::string(tempPathBuf) + filename;
    std::ofstream ofs(localPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        outError = "Failed to create installer download file: " + localPath;
        return false;
    }

    cpr::Header headers{{"Accept", "application/octet-stream"}, {"User-Agent", "SmatchetUpdater/" + GetAppVersion()}};
    cpr::Redirect redirect(true, true);
    bool cancelled = false;
    cpr::WriteCallback writeCb{[&](const std::string& data, intptr_t) -> bool {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            cancelled = true;
            return false;
        }
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        return ofs.good();
    }};
    cpr::Response resp =
        cpr::Get(cpr::Url{downloadUrl}, headers, redirect, writeCb, cpr::ConnectTimeout{5000}, cpr::Timeout{120000});
    ofs.close();
    if (cancelled) {
        std::remove(localPath.c_str());
        outError = "Download cancelled.";
        return false;
    }
    if (resp.error.code != cpr::ErrorCode::OK || resp.status_code < 200 || resp.status_code >= 300) {
        std::remove(localPath.c_str());
        outError = "Failed to download installer.";
        if (!resp.error.message.empty()) {
            outError += " " + resp.error.message;
        } else if (resp.status_code > 0) {
            outError += " HTTP " + std::to_string(resp.status_code);
        }
        return false;
    }

    // GitHub HTTPS authenticates the transport, not the artifact: a compromised release asset (or
    // tampered bytes anywhere upstream) would otherwise run with full user rights. Fail closed on
    // any Authenticode outcome other than a signature chaining to a trusted root; the env override
    // (release-pipeline validation with self-signed certs) forgives everything except a bad digest.
    const long trustStatus = VerifyInstallerAuthenticodeStatus(localPath);
    const bool allowUnsignedOverride = IsUpdateUnsignedOverrideSet();
    std::string trustError;
    if (!smatchet::app_update::ShouldLaunchDownloadedInstaller(trustStatus, allowUnsignedOverride, trustError)) {
        std::remove(localPath.c_str());
        LOG_ERROR("DownloadAndLaunchInstallerUpdate: refusing unverified installer status=0x%08lx asset=%s",
                  static_cast<unsigned long>(trustStatus), TruncateForLog(filename, 200).c_str());
        outError = trustError;
        return false;
    }
    if (trustStatus != smatchet::app_update::kInstallerTrustOk) {
        LOG_WARN("DownloadAndLaunchInstallerUpdate: SMATCHET_UPDATE_ALLOW_UNSIGNED override active — launching "
                 "installer that failed Authenticode verification (status=0x%08lx).",
                 static_cast<unsigned long>(trustStatus));
    }

    const HINSTANCE openResult = ShellExecuteA(nullptr, "open", localPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(openResult) <= 32) {
        outError = "Failed to launch downloaded installer.";
        return false;
    }

    deps_.RequestAppQuit();
    return true;
#else
    (void)assetName;
    (void)downloadUrl;
    (void)cancelFlag;
    outError = "Installer updates are currently supported only on Windows.";
    return false;
#endif
}
