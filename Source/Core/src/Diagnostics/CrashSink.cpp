#include "CrashSink.h"

#include "TextRedaction.h"

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = ghc::filesystem;

namespace smatchet {
namespace diagnostics {

namespace {

// Static path buffers — built once in Init, read (never written) by the async
// handler. Sized generously; truncated safely if a path is somehow longer.
constexpr std::size_t kPathCap = 1024;
char g_markerPath[kPathCap] = {0};
char g_dumpPath[kPathCap] = {0};
char g_breadcrumbPath[kPathCap] = {0};
char g_crashDir[kPathCap] = {0};
char g_sessionLogRecordPath[kPathCap] = {0}; // crashes/session_log.txt — points at the active log
bool g_inited = false;
std::string g_crashedLogPath; // crashed session's log path, stashed at Init when a crash is pending

void CopyPath(char* dst, const std::string& s) {
    const std::size_t n = s.size() < (kPathCap - 1) ? s.size() : (kPathCap - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

std::string NowStamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    return std::string(buf);
}

// Keep only the most recent `keep` *.dmp files in the crash dir.
void RotateDumps(const std::string& crashDir, std::size_t keep) {
    std::error_code ec;
    std::vector<fs::path> dumps;
    for (fs::directory_iterator it(crashDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".dmp") {
            dumps.push_back(it->path());
        }
    }
    if (dumps.size() <= keep) {
        return;
    }
    std::sort(dumps.begin(), dumps.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code e1, e2;
        return fs::last_write_time(a, e1) < fs::last_write_time(b, e2);
    });
    for (std::size_t i = 0; i + keep < dumps.size(); ++i) {
        fs::remove(dumps[i], ec);
    }
}

std::string ReadFileText(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return std::string();
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::string();
    }
    std::string out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return out;
}

// Last `maxLines` lines of `text`.
std::string TailLines(const std::string& text, std::size_t maxLines) {
    if (text.empty()) {
        return std::string();
    }
    std::size_t pos = text.size();
    std::size_t lines = 0;
    while (pos > 0) {
        const std::size_t nl = text.rfind('\n', pos - 1);
        if (nl == std::string::npos) {
            return text;
        }
        ++lines;
        if (lines > maxLines) {
            return text.substr(nl + 1);
        }
        pos = nl;
    }
    return text;
}

} // namespace

void CrashSinkInit(const std::string& userDataDir, const std::string& activeLogPath) {
    std::string base = userDataDir;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') {
        base += '/';
    }
    const std::string crashDir = base + "crashes";
    std::error_code ec;
    fs::create_directories(crashDir, ec);

    CopyPath(g_crashDir, crashDir);
    CopyPath(g_markerPath, crashDir + "/pending_crash.marker");
    CopyPath(g_dumpPath, crashDir + "/pending_crash.dmp");
    CopyPath(g_breadcrumbPath, crashDir + "/breadcrumb.txt");
    CopyPath(g_sessionLogRecordPath, crashDir + "/session_log.txt");

    // Stash the PREVIOUS session's recorded log path BEFORE overwriting it — but
    // only when a crash is pending, so the next-launch reporter can tail the
    // crashed log even though it's a per-PID file the new process can't guess.
    if (fs::exists(g_markerPath, ec) && !ec) {
        g_crashedLogPath = ReadFileText(g_sessionLogRecordPath);
    }
    if (!activeLogPath.empty()) {
        std::FILE* f = std::fopen(g_sessionLogRecordPath, "wb");
        if (f != nullptr) {
            std::fwrite(activeLogPath.data(), 1, activeLogPath.size(), f);
            std::fclose(f);
        }
    }

    RotateDumps(crashDir, 5);
    g_inited = true;
}

void CrashSinkBreadcrumb(const char* activity) {
    if (!g_inited || activity == nullptr || g_breadcrumbPath[0] == '\0') {
        return;
    }
    // Normal context — plain stdio is fine. Overwrite with the latest activity.
    std::FILE* f = std::fopen(g_breadcrumbPath, "wb");
    if (f == nullptr) {
        return;
    }
    std::fwrite(activity, 1, std::strlen(activity), f);
    std::fclose(f);
}

void CrashSinkWriteMarkerAsyncSafe(const char* reason) noexcept {
    if (g_markerPath[0] == '\0') {
        return;
    }
    const char* msg = (reason != nullptr) ? reason : "unknown";
    const std::size_t len = std::strlen(msg);
#if defined(_WIN32)
    HANDLE h = CreateFileA(g_markerPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, msg, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
#else
    const int fd = ::open(g_markerPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return;
    }
    ssize_t rc = ::write(fd, msg, len);
    (void)rc;
    ::close(fd);
#endif
}

const char* CrashSinkPendingDumpPath() noexcept { return g_dumpPath; }

bool CrashSinkHasPending() {
    if (g_markerPath[0] == '\0') {
        return false;
    }
    std::error_code ec;
    return fs::exists(g_markerPath, ec) && !ec;
}

CrashInfo CrashSinkConsume() {
    CrashInfo info;
    if (!CrashSinkHasPending()) {
        return info;
    }
    info.Pending = true;
    info.Reason = ReadFileText(g_markerPath);
    if (info.Reason.empty()) {
        info.Reason = "unknown";
    }
    info.Breadcrumb = ReadFileText(g_breadcrumbPath);

    // Tail the crashed session's log (path stashed at Init), redacted.
    if (!g_crashedLogPath.empty()) {
        const std::string raw = ReadFileText(g_crashedLogPath.c_str());
        if (!raw.empty()) {
            info.LogTail = privacy::RedactLogText(TailLines(raw, 200));
        }
    }

    // Archive the pending dump (timestamped) so it survives + is rotated; keep it
    // until a successful submit is out of scope for v1 (dump stays locally).
    std::error_code ec;
    if (g_dumpPath[0] != '\0' && fs::exists(g_dumpPath, ec) && !ec) {
        const std::string archived = std::string(g_crashDir) + "/crash-" + NowStamp() + ".dmp";
        fs::rename(g_dumpPath, archived, ec);
        info.DumpPath = ec ? std::string(g_dumpPath) : archived;
        RotateDumps(g_crashDir, 5);
    }

    // Delete the marker immediately so we never loop on the same crash.
    fs::remove(g_markerPath, ec);
    return info;
}

} // namespace diagnostics
} // namespace smatchet
