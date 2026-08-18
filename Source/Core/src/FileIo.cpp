// FileIo — implementation of the layer-0 filesystem primitives declared in FileIo.h.
//
// `ScopedFileLock` was moved here verbatim from ConfigManager_PathUtils.cpp
// (BACKLOG_CODE_REVIEW.md § C5); the Win32 and POSIX bodies are unchanged apart from the
// log prefix and the added `Held()` accessor. The Win32 branch is NOT exercised by the
// Linux portability lane, so keep the two branches structurally parallel.

#include "FileIo.h"

#include "Logger.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace smatchet {
namespace fileio {

#if defined(_WIN32)
namespace {

// Local UTF-8 -> UTF-16 conversion. Deliberately not shared with
// `config_detail::Utf8ToWide`: that one is declared in a src-private Config header, and
// including it here would be a layer-0 -> Config back-edge under the core-include-dag gate.
std::wstring WidenUtf8Path(const std::string& s) {
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1)
        return std::wstring();
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

} // namespace
#endif

ScopedFileLock::ScopedFileLock(const std::string& path)
    : lockPath_(path + ".lock"),
#if defined(_WIN32)
      handle_(INVALID_HANDLE_VALUE)
#else
      fd_(-1)
#endif
{
    Acquire();
}

ScopedFileLock::~ScopedFileLock() { Release(); }

bool ScopedFileLock::Held() const {
#if defined(_WIN32)
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return fd_ >= 0;
#endif
}

void ScopedFileLock::Acquire() {
#if defined(_WIN32)
    const std::wstring wLock = WidenUtf8Path(lockPath_);
    if (wLock.empty())
        return;
    HANDLE h = CreateFileW(wLock.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_WARN("FileIo: CreateFileW failed for lock '%s' err=%lu — proceeding without exclusive access",
                 lockPath_.c_str(), GetLastError());
        return;
    }
    OVERLAPPED ov{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov)) {
        LOG_WARN("FileIo: LockFileEx failed for '%s' err=%lu — proceeding without exclusive access", lockPath_.c_str(),
                 GetLastError());
        CloseHandle(h);
        handle_ = INVALID_HANDLE_VALUE;
        return;
    }
    handle_ = h;
#else
    fd_ = ::open(lockPath_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        LOG_WARN("FileIo: open() failed for lock '%s' errno=%d — proceeding without exclusive access",
                 lockPath_.c_str(), errno);
        return;
    }
    if (::flock(fd_, LOCK_EX) != 0) {
        LOG_WARN("FileIo: flock(LOCK_EX) failed for '%s' errno=%d — proceeding without exclusive access",
                 lockPath_.c_str(), errno);
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

void ScopedFileLock::Release() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov{};
        UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &ov);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

} // namespace fileio
} // namespace smatchet
