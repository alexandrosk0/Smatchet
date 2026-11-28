// FileIo — implementation of the layer-0 filesystem primitives declared in FileIo.h.
//
// `ScopedFileLock` was moved here verbatim from ConfigManager_PathUtils.cpp
// (BACKLOG_CODE_REVIEW.md § C5); the Win32 and POSIX bodies are unchanged apart from the
// log prefix and the added `Held()` accessor. The Win32 branch is NOT exercised by the
// Linux portability lane, so keep the two branches structurally parallel.

#include "FileIo.h"

#include "Logger.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

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
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace smatchet {
namespace fileio {

#if defined(_WIN32)
// The single definition in the codebase; `config_detail::Utf8ToWide` now delegates here.
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1)
        return std::wstring();
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

namespace {

constexpr const char* kTempMarker = ".part-";
// A write retries with a fresh suffix when the exclusive create finds the name taken (a stale temp whose
// pid was reused, or a name planted by another local user); more than this many collisions is hostile.
constexpr int kMaxTempCollisions = 8;
// Chunk size for streaming compares; large enough to amortise syscalls, small enough to allocate per call.
constexpr std::size_t kCompareChunkBytes = 64 * 1024;

std::uint64_t CurrentProcessId() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

bool AllDigits(const std::string& s, std::size_t from, std::size_t to) {
    if (from >= to) {
        return false;
    }
    for (std::size_t i = from; i < to; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

#if defined(_WIN32)
std::string Win32ErrorText(const char* what) {
    return std::string(what) + " failed, err=" + std::to_string(static_cast<unsigned long>(GetLastError()));
}

// Writes `size` bytes, honouring WriteFile's DWORD length limit.
bool WriteAllHandle(HANDLE h, const char* data, std::size_t size) {
    constexpr std::size_t kMaxChunk = 1u << 30;
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>(size < kMaxChunk ? size : kMaxChunk);
        DWORD written = 0;
        if (!WriteFile(h, data, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

bool WriteFileAtomicallyImpl(const std::string& path, const void* data, std::size_t size,
                             const AtomicWriteOptions& options, std::string& errOut) {
    const std::wstring wDst = Utf8ToWide(path);
    if (wDst.empty()) {
        errOut = "path is empty or not valid UTF-8";
        return false;
    }
    std::string tmp;
    std::wstring wTmp;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < kMaxTempCollisions && h == INVALID_HANDLE_VALUE; ++attempt) {
        tmp = path + UniqueTempSuffix();
        wTmp = Utf8ToWide(tmp);
        if (wTmp.empty()) {
            errOut = "temp path is not valid UTF-8";
            return false;
        }
        h = CreateFileW(wTmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            errOut = Win32ErrorText("CreateFileW(temp)");
            return false;
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        errOut = "could not create a unique temp file";
        return false;
    }
    bool ok = WriteAllHandle(h, static_cast<const char*>(data), size);
    if (!ok) {
        errOut = Win32ErrorText("WriteFile");
    } else if (options.Durable && !FlushFileBuffers(h)) {
        LOG_WARN("FileIo: FlushFileBuffers on '%s' failed err=%lu", tmp.c_str(),
                 static_cast<unsigned long>(GetLastError()));
    }
    if (!CloseHandle(h) && ok) {
        ok = false;
        errOut = Win32ErrorText("CloseHandle");
    }
    if (!ok) {
        DeleteFileW(wTmp.c_str());
        return false;
    }
    DWORD flags = MOVEFILE_REPLACE_EXISTING;
    if (options.Durable) {
        flags |= MOVEFILE_WRITE_THROUGH;
    }
    if (!MoveFileExW(wTmp.c_str(), wDst.c_str(), flags)) {
        errOut = Win32ErrorText("MoveFileExW");
        DeleteFileW(wTmp.c_str());
        return false;
    }
    return true;
}
#else
// write(2) can return short or be interrupted, so it loops; the exclusive open needs no EINTR retry.
bool WriteAllFd(int fd, const char* data, std::size_t size, int& errnoOut) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // interrupted before any byte moved — retry the same chunk.
            }
            errnoOut = errno;
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

void SyncParentDirectory(const std::string& path) {
    const std::string::size_type slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".")
                            : (slash == 0)               ? std::string("/")
                                                         : path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        LOG_WARN("FileIo: open parent dir '%s' for fsync failed errno=%d", dir.c_str(), errno);
        return;
    }
    if (::fsync(dfd) != 0) {
        LOG_WARN("FileIo: fsync on parent dir '%s' failed errno=%d", dir.c_str(), errno);
    }
    ::close(dfd);
}

bool WriteFileAtomicallyImpl(const std::string& path, const void* data, std::size_t size,
                             const AtomicWriteOptions& options, std::string& errOut) {
    std::string tmp;
    int fd = -1;
    for (int attempt = 0; attempt < kMaxTempCollisions && fd < 0; ++attempt) {
        tmp = path + UniqueTempSuffix();
        // O_EXCL: never reuse or truncate an existing name. O_NOFOLLOW: a symlink planted at the name is
        // refused instead of followed (with O_EXCL it already fails EEXIST; kept as defence in depth).
        const mode_t mode = options.OwnerOnly ? (S_IRUSR | S_IWUSR) : (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
        if (fd < 0 && errno != EEXIST) {
            errOut = "open(temp) failed, errno=" + std::to_string(errno);
            return false;
        }
    }
    if (fd < 0) {
        errOut = "could not create a unique temp file";
        return false;
    }
    // O_CREAT's mode is masked by umask; fchmod pins 0600 on the fd before any byte is written.
    if (options.OwnerOnly && ::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        LOG_WARN("FileIo: fchmod 0600 on '%s' failed errno=%d; file may be group/world-readable", tmp.c_str(), errno);
    }
    int writeErrno = 0;
    const bool writeOk = WriteAllFd(fd, static_cast<const char*>(data), size, writeErrno);
    if (writeOk && options.Durable && ::fsync(fd) != 0) {
        LOG_WARN("FileIo: fsync on '%s' failed errno=%d", tmp.c_str(), errno);
    }
    const int closeRc = ::close(fd);
    const int closeErrno = errno;
    if (!writeOk || closeRc != 0) {
        // Remove the temp before building the message: a bad_alloc there must not strand it.
        ::unlink(tmp.c_str());
        errOut = !writeOk ? ("write failed, errno=" + std::to_string(writeErrno))
                          : ("close failed, errno=" + std::to_string(closeErrno));
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        const int renameErrno = errno;
        ::unlink(tmp.c_str());
        errOut = "rename failed, errno=" + std::to_string(renameErrno);
        return false;
    }
    if (options.Durable) {
        // fsync(fd) persisted the data; the directory entry that now points at it needs its own sync.
        // Best-effort: a miss is logged and never undoes the atomic publish.
        SyncParentDirectory(path);
    }
    return true;
}
#endif

} // namespace

std::string UniqueTempSuffix() {
    static std::atomic<std::uint64_t> counter(0);
    const std::uint64_t k = counter.fetch_add(1, std::memory_order_relaxed);
    return std::string(kTempMarker) + std::to_string(CurrentProcessId()) + "-" + std::to_string(k);
}

bool IsTempSiblingOf(const std::string& name, const std::string& base) {
    const std::string marker(kTempMarker);
    if (name.size() <= base.size() + marker.size() || name.compare(0, base.size(), base) != 0 ||
        name.compare(base.size(), marker.size(), marker) != 0) {
        return false;
    }
    const std::size_t pidFrom = base.size() + marker.size();
    const std::size_t dash = name.find('-', pidFrom);
    return dash != std::string::npos && AllDigits(name, pidFrom, dash) && AllDigits(name, dash + 1, name.size());
}

bool WriteFileAtomically(const std::string& path, const void* data, std::size_t size, const AtomicWriteOptions& options,
                         std::string& errOut) {
    // Building the failure text can itself throw (bad_alloc); the contract is "never throws", so a failed
    // assignment leaves the reason empty rather than escaping.
    const auto assignErrorNoThrow = [](std::string& out, const char* prefix, const char* detail) noexcept {
        try {
            out = prefix;
            out += detail;
        } catch (...) {
            out.clear(); // a std::string clear never throws; the reason is simply lost
        }
    };
    errOut.clear();
    if (path.empty()) {
        errOut = "empty path";
        return false;
    }
    if (data == nullptr && size != 0) {
        errOut = "null data with non-zero size";
        return false;
    }
    try {
        if (WriteFileAtomicallyImpl(path, data, size, options, errOut)) {
            return true;
        }
    } catch (const std::exception& e) {
        assignErrorNoThrow(errOut, "exception: ", e.what());
    } catch (...) {
        assignErrorNoThrow(errOut, "unknown exception", "");
    }
    LOG_ERROR("FileIo: atomic write of '%s' failed: %s", path.c_str(), errOut.c_str());
    return false;
}

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const {
        if (f != nullptr) {
            std::fclose(f);
        }
    }
};

std::FILE* OpenForRead(const std::string& path) {
#if defined(_WIN32)
    const std::wstring w = Utf8ToWide(path);
    return w.empty() ? nullptr : _wfopen(w.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

} // namespace

bool FileContentEquals(const std::string& path, const void* data, std::size_t size) {
    if (data == nullptr && size != 0) {
        return false;
    }
    try {
        const std::unique_ptr<std::FILE, FileCloser> f(OpenForRead(path));
        if (!f) {
            return false;
        }
        std::vector<char> buf(kCompareChunkBytes);
        const char* expect = static_cast<const char*>(data);
        std::size_t remaining = size;
        while (remaining > 0) {
            const std::size_t want = remaining < buf.size() ? remaining : buf.size();
            if (std::fread(buf.data(), 1, want, f.get()) != want || std::memcmp(buf.data(), expect, want) != 0) {
                return false;
            }
            expect += want;
            remaining -= want;
        }
        // The file must end exactly here: one more byte means it is longer than `data`.
        return std::fread(buf.data(), 1, 1, f.get()) == 0 && std::feof(f.get()) != 0;
    } catch (const std::exception& e) {
        LOG_WARN("FileIo: compare of '%s' failed: %s", path.c_str(), e.what());
        return false;
    }
}

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
    const std::wstring wLock = Utf8ToWide(lockPath_);
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
