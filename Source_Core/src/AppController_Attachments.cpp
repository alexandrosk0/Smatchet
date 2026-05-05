#include "AppController.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <fstream>
#include <string>
#include <utility>

#include "AttachmentMimeUtils.h"
#include "ConfigManager.h"
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "StringUtil.h"

#include <cpr/cpr.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

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
        value = value.substr(0, slashPos);
    }
    const size_t atPos = value.rfind('@');
    if (atPos != std::string::npos) {
        value = value.substr(atPos + 1);
    }
    const size_t colonPos = value.find(':');
    if (colonPos != std::string::npos) {
        value = value.substr(0, colonPos);
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
#if defined(_WIN32) && defined(_MSC_VER)
    // MSVC: getenv is deprecated (C4996); _dupenv_s is not linked the same way on MinGW.
    auto readEnv = [](const char* name) -> std::string {
        char* buf = nullptr;
        size_t sz = 0;
        if (_dupenv_s(&buf, &sz, name) != 0) {
            if (buf) {
                std::free(buf);
            }
            return {};
        }
        if (!buf || buf[0] == '\0') {
            if (buf) {
                std::free(buf);
            }
            return {};
        }
        std::string out(buf);
        std::free(buf);
        return out;
    };
    std::string t = readEnv("TEMP");
    if (!t.empty()) {
        return t;
    }
    t = readEnv("TMP");
    if (!t.empty()) {
        return t;
    }
    return ".";
#else
    const char* env = std::getenv("TEMP");
    if (env && *env)
        return std::string(env);
    env = std::getenv("TMP");
    if (env && *env)
        return std::string(env);
    return ".";
#endif
}

std::string SanitizeFilename(const std::string& name) {
    std::string out = name;
    for (auto& ch : out) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '\"' || ch == '<' || ch == '>' ||
            ch == '|') {
            ch = '_';
        }
    }
    if (out.empty())
        return std::string("attachment");
    return out;
}

std::string MakeUniqueTempFilePath(const std::string& filename, const std::string& extension) {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
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
        return tempDir + "/" + std::to_string(now) + "_" + base;
    }
    return tempDir + std::to_string(now) + "_" + base;
}
} // namespace

void AppController::ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments) {
    if (attachments.empty()) {
        return;
    }
    if (AttachmentCollectionHandlerCallback) {
        AttachmentCollectionHandlerCallback(attachments);
        return;
    }

    const AttachmentDescriptor& first = attachments.front();
    if (!first.Url.empty()) {
        OpenAttachment(first.Url, first.Filename, first.MimeType);
    }
}

void AppController::OpenAttachment(const std::string& url, const std::string& filename, const std::string& mimeType) {
    if (url.empty()) {
        return;
    }

    // If no file-based path exists, fall back to regular URL opening.
    if (!AttachmentViewerHandlerCallback && !AttachmentPreviewHandlerCallback) {
        LOG_INFO("OpenAttachment: no attachment handler, opening URL directly.");
        OpenUrl(url);
        return;
    }

    std::string outFilePath;
    std::string outMime;
    std::string outError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, outError)) {
        LOG_WARN("OpenAttachment: %s; falling back to URL open.", outError.c_str());
        OpenUrl(url);
        return;
    }
    if (AttachmentViewerHandlerCallback) {
        LOG_INFO("OpenAttachment: dispatching to host attachment viewer.");
        AttachmentViewerHandlerCallback(outFilePath, outMime, filename);
        return;
    }

    if (AttachmentPreviewHandlerCallback && IsSupportedImageMime(outMime)) {
        if (AttachmentPreviewHandlerCallback(outFilePath, outMime, filename, url)) {
            LOG_INFO("OpenAttachment: queued in-app image preview.");
            return;
        }
        LOG_WARN("OpenAttachment: in-app preview rejected; falling back to URL open.");
    }

    LOG_INFO("OpenAttachment: opening attachment via URL fallback.");
    OpenUrl(url);
}

void AppController::OpenAttachmentInSystemViewer(const std::string& url, const std::string& filename,
                                                 const std::string& mimeType) {
    if (url.empty()) {
        return;
    }
    std::string outFilePath;
    std::string outMime;
    std::string outError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, outError)) {
        LOG_WARN("OpenAttachmentInSystemViewer: %s; falling back to URL open.", outError.c_str());
        OpenUrl(url);
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

bool AppController::DownloadAttachmentForPreview(const std::string& url, const std::string& filename,
                                                 const std::string& mimeType, std::string* outError) {
    auto fail = [outError](const std::string& errorMessage) {
        if (outError != nullptr) {
            *outError = errorMessage;
        }
        return false;
    };
    if (!AttachmentPreviewHandlerCallback || !IsSupportedImageMime(mimeType)) {
        return fail(!AttachmentPreviewHandlerCallback ? std::string("Preview handler is unavailable.")
                                                      : std::string("Attachment is not a supported image type."));
    }
    std::string outFilePath;
    std::string outMime;
    std::string downloadError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, downloadError)) {
        LOG_WARN("DownloadAttachmentForPreview: %s", downloadError.c_str());
        return fail(downloadError);
    }
    if (!AttachmentPreviewHandlerCallback(outFilePath, outMime, filename, url)) {
        LOG_WARN("DownloadAttachmentForPreview: preview handler rejected file=%s mime=%s", filename.c_str(),
                 outMime.c_str());
        return fail("Preview handler rejected the attachment.");
    }
    if (outError != nullptr) {
        outError->clear();
    }
    return true;
}






