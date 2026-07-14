#pragma once

// CliResultFileRead — the shared bounded read of a CLI scenario RESULT file, extracted from the
// two near-identical readers in CliCommandRunner.cpp (in-process) and CliDispatch.cpp (--spawn).
// Both read a JSON result file our own handler/child wrote, then hand it to json_safe::ParseBounded.
// The read itself is the duplicated leaf (DRY Pillar 5); the ParseBounded + per-surface error
// envelope / shutdown differ, so they stay at the call sites.
//
// Defensive OOM bound (command-input-hardening Phase 1.3): the file is first-party, but a corrupt /
// oversized one must not balloon parent memory before ParseBounded's 4 MiB cap rejects it — so the
// read stops past `maxBytes` and reports TooLarge, mirroring the inline instance.json cap (#1747).
// Header-only, C++14, stdlib-only — no link dependency.

#include <cstddef>
#include <cstdio>
#include <string>

namespace smatchet {
namespace cli {

enum class ResultFileReadStatus {
    Ok,         // fully read within the cap; `out` holds the bytes
    OpenFailed, // fopen failed (missing / unreadable path)
    TooLarge,   // exceeded maxBytes; `out` holds the truncated prefix and must not be parsed
};

/// Read `path` (binary) into `out` under a hard byte cap. Returns TooLarge (leaving a truncated
/// prefix in `out`) the moment the accumulated size exceeds `maxBytes`, so a multi-GB file cannot
/// OOM the process before the caller's ParseBounded sees it. `out` is cleared on entry.
inline ResultFileReadStatus ReadResultFileBounded(const std::string& path, std::size_t maxBytes,
                                                  std::string& out) {
    out.clear();
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // fopen: cross-platform — fopen_s is MSVC-only
#endif
    std::FILE* f = std::fopen(path.c_str(), "rb");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (!f) {
        return ResultFileReadStatus::OpenFailed;
    }
    char buf[4096];
    std::size_t n = 0;
    ResultFileReadStatus status = ResultFileReadStatus::Ok;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > maxBytes) {
            status = ResultFileReadStatus::TooLarge;
            break;
        }
    }
    std::fclose(f);
    return status;
}

} // namespace cli
} // namespace smatchet
