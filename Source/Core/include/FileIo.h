#ifndef SMATCHET_FILE_IO_H
#define SMATCHET_FILE_IO_H

// Layer-0 filesystem primitives shared across subsystems (BACKLOG_CODE_REVIEW.md § C5).
//
// Holds ONLY primitives with a real cross-subsystem consumer. Today that is
// `ScopedFileLock`, hoisted out of `Source/Core/src/Config/ConfigManager_Internal.h`
// (a src-private header no non-Config TU may include) so `Persistence/` can guard the
// audit-trail append with the same cross-process lock the config writers already use.
//
// Deliberately NOT here: `ConfigManager::AtomicWriteTextFile`. It is already a public
// static shared by six TUs, so relocating it would be pure churn — and it is the WRONG
// primitive for an append-only log: whole-file replace would force a read-modify-rewrite
// of the entire file per appended line.
//
// Layer rank 0 (no subdirectory) under the `core-include-dag` gate, so Config/ (1),
// Persistence/ (2) and anything above may depend on it while it depends on nothing
// first-party in return.

#include <string>

namespace smatchet {
namespace fileio {

#if defined(_WIN32)
// UTF-8 to UTF-16 for a path handed to a wide Win32 API. Declared here so the Config layer
// shares this one definition rather than keeping a second copy (dup gate, Pillar 5); the
// declaration is Win32-only because no other platform has a use for it.
std::wstring Utf8ToWide(const std::string& s);
#endif

// Cross-process advisory lock held for the lifetime of the object, taken on a
// `<path>.lock` sidecar rather than on `path` itself. The sidecar indirection is what
// makes the lock survive an atomic temp-then-rename replace of `path`: renaming swaps
// the inode out from under any lock held on the original file, whereas the sidecar's
// identity is stable.
//
// Best-effort by contract: every acquisition failure (permissions, a read-only mount,
// an AV handle) is logged and the object proceeds UNLOCKED rather than throwing or
// blocking forever. Callers must therefore treat it as a contention optimisation layered
// on top of an operation that is already safe in the single-process case, never as the
// sole guarantee of correctness.
//
// NOT reentrant and NOT a substitute for an in-process mutex. On POSIX the lock is
// `flock(2)`, which is per-open-file-description: two ScopedFileLock objects alive in the
// same process on the same path hold two descriptions and the second one BLOCKS. Take the
// subsystem's own mutex first, then this.
//
// `handle_` / `fd_` are typed as `void*` / `int` so consumers need no <windows.h>. On
// Windows, `void*` is layout-compatible with `HANDLE` (itself a `void*` typedef).
class ScopedFileLock {
  public:
    explicit ScopedFileLock(const std::string& path);
    ~ScopedFileLock();
    ScopedFileLock(const ScopedFileLock&) = delete;
    ScopedFileLock& operator=(const ScopedFileLock&) = delete;

    // The sidecar path actually locked (`path + ".lock"`). Exposed so callers can clean it
    // up and so tests can assert the lock was taken against the intended file.
    const std::string& LockPath() const { return lockPath_; }

    // True when the advisory lock is genuinely held. False means the acquisition degraded
    // to the unlocked best-effort path described above.
    bool Held() const;

  private:
    void Acquire();
    void Release();

    std::string lockPath_;
#if defined(_WIN32)
    void* handle_;
#else
    int fd_;
#endif
};

} // namespace fileio
} // namespace smatchet

#endif // SMATCHET_FILE_IO_H
