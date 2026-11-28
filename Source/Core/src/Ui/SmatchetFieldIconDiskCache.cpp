#include "SmatchetFieldIconDiskCache.h"

#include "FileIo.h"
#include "ListRegularFiles.h"
#include "Logger.h"
#include "SmatchetFieldIconRender_detail.h"

#include <ghc/filesystem.hpp>

#include <chrono>
#include <exception>
#include <system_error>
#include <utility>
#include <vector>

namespace SmatchetFieldIconDiskCache {

namespace {

namespace fs = ghc::filesystem;

enum class ReadStatus { Ok, Missing, TooLarge, Failed };

// Reads at most kMaxIconFileBytes: it asks for one byte more than the cap, so a file that grows after a
// size check (or a lying size) is still caught. ghc streams take UTF-8 paths on Windows too.
ReadStatus ReadBounded(const std::string& path, IconBytes& out) {
    out.clear();
    fs::ifstream in(fs::path(path), std::ios::binary);
    if (!in.is_open()) {
        std::error_code ec;
        return fs::exists(fs::path(path), ec) ? ReadStatus::Failed : ReadStatus::Missing;
    }
    out.resize(kMaxIconFileBytes + 1);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    const std::size_t got = static_cast<std::size_t>(in.gcount());
    if (in.bad()) {
        out.clear();
        return ReadStatus::Failed;
    }
    if (got > kMaxIconFileBytes) {
        out.clear();
        return ReadStatus::TooLarge;
    }
    out.resize(got);
    return ReadStatus::Ok;
}

IconResolveOutcome Failure(std::string error) {
    IconResolveOutcome outcome;
    outcome.Ok = false;
    outcome.Error = std::move(error);
    return outcome;
}

// Decodes `bytes`; a decoder that throws counts as a failed decode, never an escaped exception.
Result<DecodedIcon> SafeDecode(const DecodeFn& decode, const IconBytes& bytes) {
    try {
        return decode(bytes.data(), bytes.size());
    } catch (const std::exception& ex) {
        return Result<DecodedIcon>::Err(std::string("decoder threw: ") + ex.what());
    }
}

using smatchet::filetime::ToEpochSeconds;

// "Now" on the same clock as file modification times, so the two compare whatever the platform's file epoch.
std::int64_t NowFileClockSeconds() { return smatchet::filetime::NowEpochSeconds<fs::file_time_type::clock>(); }

} // namespace

bool EnsureCacheDir(const std::string& userDataDir, std::string& outDir, std::string& outError) {
    outError.clear();
    outDir.clear();
    if (userDataDir.empty()) {
        outError = "Files base directory is empty.";
        return false;
    }
    const std::string dir = userDataDir + "cache/field_icons/";
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    if (ec) {
        outError = ec.message();
        return false;
    }
    outDir = dir;
    return true;
}

IconResolveOutcome ResolveFileIcon(const std::string& path, const DecodeFn& decode) {
    if (path.empty() || !decode) {
        return Failure("Empty icon path.");
    }
    IconBytes bytes;
    switch (ReadBounded(path, bytes)) {
    case ReadStatus::Missing: {
        IconResolveOutcome missing = Failure("Icon file not found.");
        missing.NotFound = true;
        return missing;
    }
    case ReadStatus::TooLarge:
        return Failure("Icon file is larger than the allowed size.");
    case ReadStatus::Failed:
        return Failure("Icon file could not be read.");
    case ReadStatus::Ok:
        break;
    }
    if (bytes.empty()) {
        return Failure("Icon file is empty.");
    }
    Result<DecodedIcon> decoded = SafeDecode(decode, bytes);
    if (!decoded.has_value()) {
        return Failure(decoded.error());
    }
    IconResolveOutcome outcome;
    outcome.Ok = true;
    outcome.Icon = std::move(decoded.value());
    outcome.Source = IconSource::LocalFile;
    return outcome;
}

IconResolveOutcome ResolveUrlIcon(const std::string& cacheDir, const std::string& url, const FetchFn& fetch,
                                  const DecodeFn& decode) {
    if (cacheDir.empty() || url.empty() || !decode) {
        return Failure("Icon cache is not available.");
    }
    const std::string cachePath = cacheDir + SmatchetFieldIconRender::detail::UrlToCacheFileName(url);
    bool removedBad = false;
    {
        IconBytes cached;
        const ReadStatus status = ReadBounded(cachePath, cached);
        if (status == ReadStatus::Ok && !cached.empty()) {
            Result<DecodedIcon> decoded = SafeDecode(decode, cached);
            if (decoded.has_value()) {
                IconResolveOutcome outcome;
                outcome.Ok = true;
                outcome.Icon = std::move(decoded.value());
                outcome.Source = IconSource::Disk;
                return outcome;
            }
            LOG_WARN("FieldIconDiskCache: cached icon '%s' does not decode (%s); removing it", cachePath.c_str(),
                     decoded.error().c_str());
        }
        // Oversized, empty or undecodable: drop it so the fetch below can replace it. A file that merely could
        // not be opened (an antivirus scan or a viewer holding it) may be perfectly good, so it is left alone.
        // (A readable file that decoded has already returned, so Ok here means empty or undecodable.)
        if (status == ReadStatus::TooLarge || status == ReadStatus::Ok) {
            std::error_code ec;
            removedBad = fs::remove(fs::path(cachePath), ec) && !ec;
        }
    }
    if (!fetch) {
        return Failure("No icon fetcher is configured.");
    }
    Result<IconBytes> fetched = Result<IconBytes>::Err("Icon fetch did not run.");
    try {
        fetched = fetch(url);
    } catch (const std::exception& ex) {
        fetched = Result<IconBytes>::Err(std::string("fetch threw: ") + ex.what());
    }
    if (!fetched.has_value()) {
        IconResolveOutcome outcome = Failure(fetched.error());
        outcome.RemovedBadFile = removedBad;
        return outcome;
    }
    const IconBytes& body = fetched.value();
    if (body.empty() || body.size() > kMaxIconFileBytes) {
        IconResolveOutcome outcome =
            Failure(body.empty() ? "Icon response was empty." : "Icon response is larger than the allowed size.");
        outcome.RemovedBadFile = removedBad;
        return outcome;
    }
    Result<DecodedIcon> decoded = SafeDecode(decode, body);
    if (!decoded.has_value()) {
        IconResolveOutcome outcome = Failure(decoded.error());
        outcome.RemovedBadFile = removedBad;
        return outcome;
    }
    // Cache only bytes that decode. A write failure costs the next launch a re-fetch, nothing more.
    smatchet::fileio::AtomicWriteOptions options;
    options.OwnerOnly = true;
    options.Durable = false;
    std::string writeError;
    if (!smatchet::fileio::WriteFileAtomically(cachePath, body.data(), body.size(), options, writeError)) {
        LOG_WARN("FieldIconDiskCache: could not cache icon '%s': %s", cachePath.c_str(), writeError.c_str());
    }
    IconResolveOutcome outcome;
    outcome.Ok = true;
    outcome.Icon = std::move(decoded.value());
    outcome.Source = IconSource::Network;
    outcome.RemovedBadFile = removedBad;
    return outcome;
}

RevalidateResult RevalidateUrlIcon(const std::string& cacheDir, const std::string& url, const FetchFn& fetch,
                                   const DecodeFn& decode) {
    if (cacheDir.empty() || url.empty() || !fetch || !decode) {
        return RevalidateResult::Fresh;
    }
    try {
        const std::string cachePath = cacheDir + SmatchetFieldIconRender::detail::UrlToCacheFileName(url);
        std::error_code ec;
        const fs::file_time_type modified = fs::last_write_time(fs::path(cachePath), ec);
        if (ec || !IsStale(ToEpochSeconds(modified), NowFileClockSeconds(), kRevalidateAfterSeconds)) {
            return RevalidateResult::Fresh; // no file to refresh, or it is recent
        }
        Result<IconBytes> fetched = fetch(url);
        if (!fetched.has_value() || fetched.value().empty() || fetched.value().size() > kMaxIconFileBytes) {
            return RevalidateResult::Failed;
        }
        const IconBytes& body = fetched.value();
        if (!SafeDecode(decode, body).has_value()) {
            return RevalidateResult::Failed;
        }
        smatchet::fileio::AtomicWriteOptions options;
        options.OwnerOnly = true;
        options.Durable = false;
        std::string writeError;
        if (!smatchet::fileio::WriteFileAtomically(cachePath, body.data(), body.size(), options, writeError)) {
            LOG_WARN("FieldIconDiskCache: could not refresh icon '%s': %s", cachePath.c_str(), writeError.c_str());
            return RevalidateResult::Failed;
        }
        return RevalidateResult::Refreshed;
    } catch (const std::exception& ex) {
        LOG_WARN("FieldIconDiskCache: revalidation failed: %s", ex.what());
        return RevalidateResult::Failed;
    }
}

std::size_t PruneCacheDir(const std::string& cacheDir, std::uint64_t capBytes, std::int64_t tempGraceSeconds) {
    if (cacheDir.empty()) {
        return 0;
    }
    std::size_t removed = 0;
    try {
        std::vector<IconCacheFile> files;
        for (const smatchet::filetime::ListedFile& listed : smatchet::filetime::ListRegularFiles(cacheDir)) {
            IconCacheFile f;
            f.Name = listed.Name;
            f.SizeBytes = listed.SizeBytes;
            f.ModifiedEpochSec = listed.ModifiedEpochSec;
            files.push_back(std::move(f));
        }
        const std::vector<std::string> victims =
            SelectIconPruneVictims(files, capBytes, NowFileClockSeconds(), tempGraceSeconds);
        for (const std::string& name : victims) {
            std::error_code rmEc;
            if (fs::remove(fs::path(cacheDir) / name, rmEc) && !rmEc) {
                ++removed;
            } else if (rmEc) {
                LOG_WARN("FieldIconDiskCache: could not delete '%s': %s", name.c_str(), rmEc.message().c_str());
            }
        }
        if (removed > 0) {
            LOG_INFO("FieldIconDiskCache: removed %zu file(s) from '%s'", removed, cacheDir.c_str());
        }
    } catch (const std::exception& ex) {
        LOG_WARN("FieldIconDiskCache: prune of '%s' failed: %s", cacheDir.c_str(), ex.what());
    }
    return removed;
}

std::size_t PurgeCacheDir(const std::string& cacheDir) { return PruneCacheDir(cacheDir, 0, 0); }

} // namespace SmatchetFieldIconDiskCache
