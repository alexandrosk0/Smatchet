// AttachmentDiskCache + ResolveAttachment — an attachment downloaded once opens again with no network,
// offline a never-downloaded attachment fails at once instead of waiting out the network, and the cache
// stays inside its folder and prunes back to its cap (offline-first S13). Runs against a private temp
// directory; the download is a fake fetch function.

#include "AttachmentDiskCache.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = ghc::filesystem;

using smatchet::attachments::AttachmentDiskCache;
using smatchet::attachments::AttachmentRequest;
using smatchet::attachments::EntryKeyForUrl;
using smatchet::attachments::FetchedAttachment;
using smatchet::attachments::LocalAttachment;
using smatchet::attachments::ResolveAttachment;

namespace {

using FetchResult = Result<FetchedAttachment, TrackerError>;
using LocalResult = Result<LocalAttachment, TrackerError>;

const char* const kUrl = "https://site.atlassian.net/rest/api/3/attachment/content/10001";
const char* const kOfflineText = "Not downloaded yet - available once online";

// A private directory, removed with everything in it when the test ends.
class ScopedTempDir {
  public:
    ScopedTempDir() {
        static std::atomic<int> counter{0};
        const long long stamp = static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = (fs::temp_directory_path() /
                 ("smatchet_attachment_cache_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1))))
                    .string();
        fs::create_directories(path_);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    const std::string& Path() const { return path_; }

  private:
    std::string path_;
};

std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

FetchedAttachment Bytes(const std::string& bytes, const std::string& mime = "image/png") {
    FetchedAttachment f;
    f.Bytes = bytes;
    f.Mime = mime;
    return f;
}

AttachmentRequest Request(bool offline = false) {
    AttachmentRequest r;
    r.Url = kUrl;
    r.FileName = "diagram";
    r.Offline = offline;
    r.OfflineMissMessage = kOfflineText;
    return r;
}

LocalResult NoUncachedWrite(const FetchedAttachment&) {
    return LocalResult::Err(TrackerErrorUnknown("uncached write not expected"));
}

void SetLastUsed(const std::string& dir, std::int64_t secondsAgo) {
    const fs::file_time_type when = fs::file_time_type::clock::now() - std::chrono::seconds(secondsAgo);
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        fs::last_write_time(it->path(), when, ec);
    }
    fs::last_write_time(dir, when, ec);
}

} // namespace

TEST_SUITE("AttachmentDiskCache") {

    TEST_CASE("a stored attachment comes back from Lookup, inside the cache folder") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        const LocalResult stored = cache.Store(kUrl, "diagram", Bytes(std::string("\x89PNG\0data", 9)));
        REQUIRE(stored.has_value());
        const fs::path entryDir = fs::path(dir.Path()) / "attachment_cache" / EntryKeyForUrl(kUrl);
        CHECK(fs::exists(entryDir / "diagram.png"));
        CHECK(fs::exists(entryDir / "entry.json"));

        LocalAttachment hit;
        REQUIRE(cache.Lookup(kUrl, hit));
        CHECK(hit.FilePath == stored.value().FilePath);
        CHECK(hit.Mime == "image/png");
        CHECK(ReadAll(hit.FilePath) == std::string("\x89PNG\0data", 9));

        LocalAttachment other;
        CHECK_FALSE(cache.Lookup("https://site.atlassian.net/rest/api/3/attachment/content/2", other));
    }

    TEST_CASE("the second open is served from disk with no download") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        int fetches = 0;
        const auto fetch = [&fetches]() {
            ++fetches;
            return FetchResult::Ok(Bytes("pixels"));
        };
        const LocalResult first = ResolveAttachment(cache, Request(), fetch, NoUncachedWrite);
        REQUIRE(first.has_value());
        CHECK(fetches == 1);

        const LocalResult second = ResolveAttachment(cache, Request(), fetch, NoUncachedWrite);
        REQUIRE(second.has_value());
        CHECK(fetches == 1);
        CHECK(second.value().FilePath == first.value().FilePath);
    }

    TEST_CASE("offline, a saved attachment still opens and a missing one fails at once") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        int fetches = 0;
        const auto fetch = [&fetches]() {
            ++fetches;
            return FetchResult::Ok(Bytes("pixels"));
        };

        const LocalResult miss = ResolveAttachment(cache, Request(/*offline=*/true), fetch, NoUncachedWrite);
        REQUIRE_FALSE(miss.has_value());
        CHECK(miss.error().Kind == TrackerErrorKind::Transport);
        CHECK(miss.error().Detail == kOfflineText);
        CHECK(fetches == 0);

        REQUIRE(ResolveAttachment(cache, Request(), fetch, NoUncachedWrite).has_value());
        const LocalResult hit = ResolveAttachment(cache, Request(/*offline=*/true), fetch, NoUncachedWrite);
        REQUIRE(hit.has_value());
        CHECK(ReadAll(hit.value().FilePath) == "pixels");
        CHECK(fetches == 1);
    }

    TEST_CASE("an unreachable tracker reads as unavailable offline; other failures keep their kind") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        const LocalResult unreachable = ResolveAttachment(
            cache, Request(), []() { return FetchResult::Err(TrackerErrorTransport("HTTP 0 (Couldn't connect)")); },
            NoUncachedWrite);
        REQUIRE_FALSE(unreachable.has_value());
        CHECK(unreachable.error().Kind == TrackerErrorKind::Transport);
        CHECK(unreachable.error().Detail == kOfflineText);

        const LocalResult refused = ResolveAttachment(
            cache, Request(), []() { return FetchResult::Err(TrackerErrorAuth("HTTP 401", 401)); }, NoUncachedWrite);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().Kind == TrackerErrorKind::Auth);
    }

    TEST_CASE("a download that throws is a failure, not a crash") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        LocalResult result = LocalResult::Err(TrackerErrorUnknown("unset"));
        CHECK_NOTHROW(result = ResolveAttachment(
                          cache, Request(), []() -> FetchResult { throw std::runtime_error("socket exploded"); },
                          NoUncachedWrite));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().Detail.find("socket exploded") != std::string::npos);
    }

    TEST_CASE("when the cache cannot be written the attachment still opens from the uncached copy") {
        ScopedTempDir dir;
        // The "user data folder" is a regular file, so no cache directory can be created under it.
        const std::string notADir = dir.Path() + "/plain-file";
        std::ofstream(notADir) << "x";
        AttachmentDiskCache cache(notADir);
        int uncachedWrites = 0;
        const LocalResult result = ResolveAttachment(
            cache, Request(), []() { return FetchResult::Ok(Bytes("pixels")); },
            [&uncachedWrites](const FetchedAttachment& fetched) {
                ++uncachedWrites;
                LocalAttachment local;
                local.FilePath = "uncached-copy";
                local.Mime = fetched.Mime;
                return LocalResult::Ok(local);
            });
        REQUIRE(result.has_value());
        CHECK(uncachedWrites == 1);
        CHECK(result.value().FilePath == "uncached-copy");
    }

    TEST_CASE("a tampered entry.json never points outside its entry") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        REQUIRE(cache.Store(kUrl, "diagram", Bytes("pixels")).has_value());
        const fs::path meta = fs::path(dir.Path()) / "attachment_cache" / EntryKeyForUrl(kUrl) / "entry.json";

        std::ofstream(meta.string(), std::ios::trunc) << R"({"url":")" << kUrl << R"(","file":"../../escape.png"})";
        LocalAttachment out;
        CHECK_FALSE(cache.Lookup(kUrl, out));

        // An entry recorded for another URL (a hash collision) is not a hit either.
        std::ofstream(meta.string(), std::ios::trunc) << R"({"url":"https://elsewhere/1","file":"diagram.png"})";
        CHECK_FALSE(cache.Lookup(kUrl, out));
    }

    TEST_CASE("pruning evicts the least recently used entries down to the cap") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path(), /*capBytes=*/200, /*graceSeconds=*/60);
        const std::vector<std::string> urls = {"https://site.atlassian.net/a/1", "https://site.atlassian.net/a/2",
                                               "https://site.atlassian.net/a/3"};
        const fs::path root = fs::path(dir.Path()) / "attachment_cache";
        for (std::size_t i = 0; i < urls.size(); ++i) {
            REQUIRE(cache.Store(urls[i], "f", Bytes(std::string(100, 'x'))).has_value());
            // Oldest first: a/1 was used longest ago.
            SetLastUsed((root / EntryKeyForUrl(urls[i])).string(), static_cast<std::int64_t>(3600 - i * 100));
        }
        fs::create_directories(root / "not-an-entry"); // foreign folders are never deleted

        cache.Prune();
        LocalAttachment out;
        CHECK_FALSE(cache.Lookup(urls[0], out));
        CHECK_FALSE(cache.Lookup(urls[1], out));
        CHECK(cache.Lookup(urls[2], out));
        CHECK(fs::exists(root / "not-an-entry"));
    }

    TEST_CASE("pruning never evicts an attachment used within the grace window") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path(), /*capBytes=*/10, /*graceSeconds=*/3600);
        REQUIRE(cache.Store(kUrl, "f", Bytes(std::string(100, 'x'))).has_value());
        cache.Prune();
        LocalAttachment out;
        CHECK(cache.Lookup(kUrl, out));
    }

    TEST_CASE("an interrupted download is cleaned up once it is old") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path(), /*capBytes=*/1u << 20, /*graceSeconds=*/60);
        const fs::path partial = fs::path(dir.Path()) / "attachment_cache" / EntryKeyForUrl("https://x/partial");
        fs::create_directories(partial);
        std::ofstream((partial / "f.bin.part-1").string()) << "half";
        SetLastUsed(partial.string(), 3600);
        cache.Prune();
        CHECK_FALSE(fs::exists(partial));
    }

    TEST_CASE("concurrent stores, lookups and prunes of one attachment stay consistent") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&cache]() {
                for (int i = 0; i < 20; ++i) {
                    (void)cache.Store(kUrl, "diagram", Bytes("pixels"));
                    LocalAttachment out;
                    (void)cache.Lookup(kUrl, out);
                    cache.Prune();
                }
            });
        }
        for (std::thread& t : threads) {
            t.join();
        }
        LocalAttachment out;
        REQUIRE(cache.Lookup(kUrl, out));
        CHECK(ReadAll(out.FilePath) == "pixels");
    }
}
