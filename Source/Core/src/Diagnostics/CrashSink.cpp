#include "CrashSink.h"

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
bool g_inited = false;

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

} // namespace

void CrashSinkInit(const std::string& userDataDir) {
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
