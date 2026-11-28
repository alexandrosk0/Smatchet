#pragma once

// The regular files directly inside a directory with their sizes and last-write times, for the housekeeping
// sweeps of the file caches (icon directory prune, uncached attachment copies). File I/O: worker only.
// Header-only; needs ghc::filesystem.

#include "FileTimeSeconds.h"

#include <ghc/filesystem.hpp>

#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace smatchet {
namespace filetime {

struct ListedFile {
    std::string Name; ///< the file's name within the directory (UTF-8)
    std::uint64_t SizeBytes = 0;
    std::int64_t ModifiedEpochSec = 0; ///< on the file-time clock, comparable with NowFileClockEpochSeconds()
};

/// `Clock::now()` of the file-time clock in whole seconds: the "now" a ListedFile time is compared with.
inline std::int64_t NowFileClockEpochSeconds() { return NowEpochSeconds<ghc::filesystem::file_time_type::clock>(); }

/// The regular files directly inside `dir` (subdirectories and special files are skipped; a missing or
/// unreadable directory gives an empty list). A file whose time cannot be read is reported as modified "now", so
/// an age rule never treats a file of unknown age as old; an unreadable size is 0.
inline std::vector<ListedFile> ListRegularFiles(const std::string& dir) {
    namespace fs = ghc::filesystem;
    std::vector<ListedFile> files;
    std::error_code ec;
    const std::int64_t now = NowFileClockEpochSeconds();
    for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc) {
            continue;
        }
        ListedFile file;
        file.Name = it->path().filename().string();
        const std::uintmax_t size = it->file_size(fileEc);
        file.SizeBytes = fileEc ? 0u : static_cast<std::uint64_t>(size);
        const fs::file_time_type modified = it->last_write_time(fileEc);
        file.ModifiedEpochSec = fileEc ? now : ToEpochSeconds(modified);
        files.push_back(std::move(file));
    }
    return files;
}

} // namespace filetime
} // namespace smatchet
