// AttachmentDiskCache + ResolveAttachment — an attachment downloaded once opens again with no network,
// offline a never-downloaded attachment fails at once instead of waiting out the network, and the cache
// stays inside its folder and prunes back to its cap (offline-first S13). Runs against a private temp
// directory; the download is a fake fetch function.

#include "AttachmentDiskCache.h"

#include "FileIo.h"
#include "ScopedTempDir.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = ghc::filesystem;

using smatchet::attachments::AttachmentDiskCache;
using smatchet::attachments::AttachmentRequest;
using smatchet::attachments::EntryKeyFor;
using smatchet::attachments::EntryKeyForUrl;
using smatchet::attachments::FetchedAttachment;
using smatchet::attachments::LocalAttachment;
using smatchet::attachments::ResolveAttachment;
using smatchet_tests::ScopedTempDir;

namespace {

using FetchResult = Result<FetchedAttachment, TrackerError>;
using LocalResult = Result<LocalAttachment, TrackerError>;

const char* const kUrl = "https://site.atlassian.net/rest/api/3/attachment/content/10001";
const char* const kOfflineText = "Not downloaded yet - available once online";

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

// Ages one file so the grace window no longer protects it.
void SetFileAge(const fs::path& file, std::int64_t secondsAgo) {
    std::error_code ec;
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::seconds(secondsAgo), ec);
}

void WriteText(const fs::path& file, const std::string& text) {
    std::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
    out << text;
}

// Squats a directory on each of the next kWindow temp names of `target`, so every exclusive create of the
// atomic writer collides and a write of `target` fails. Released (directories removed) on destruction.
class TempNameSquatter {
  public:
    explicit TempNameSquatter(const std::string& target) {
        const std::string probe = smatchet::fileio::UniqueTempSuffix(); // ".part-<pid>-<n>"
        const std::string::size_type dash = probe.rfind('-');
        const std::string prefix = target + probe.substr(0, dash + 1);
        const unsigned long long first = std::stoull(probe.substr(dash + 1)) + 1;
        for (unsigned long long n = first; n < first + 64; ++n) {
            const std::string dir = prefix + std::to_string(n);
            std::error_code ec;
            fs::create_directories(dir, ec);
            dirs_.push_back(dir);
        }
    }
    ~TempNameSquatter() {
        std::error_code ec;
        for (std::size_t i = 0; i < dirs_.size(); ++i) {
            fs::remove_all(dirs_[i], ec);
        }
    }
    TempNameSquatter(const TempNameSquatter&) = delete;
    TempNameSquatter& operator=(const TempNameSquatter&) = delete;

  private:
    std::vector<std::string> dirs_;
};

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

    TEST_CASE("an old orphaned temp next to the stored file is swept; a young temp and a lookalike are kept") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path(), /*capBytes=*/1u << 20, /*graceSeconds=*/60);
        const LocalResult stored = cache.Store(kUrl, "spec.pdf", Bytes("pdf-bytes", "application/pdf"));
        REQUIRE(stored.has_value());
        const fs::path entryDir = fs::path(dir.Path()) / "attachment_cache" / EntryKeyForUrl(kUrl);
        const fs::path oldTemp = entryDir / "spec.pdf.part-4242-7";
        const fs::path oldMetaTemp = entryDir / "entry.json.part-4242-8";
        const fs::path youngTemp = entryDir / "spec.pdf.part-4242-9";
        const fs::path lookalike = entryDir / "spec.part-2.pdf";
        for (const fs::path& f : {oldTemp, oldMetaTemp, youngTemp, lookalike}) {
            WriteText(f, "junk");
        }
        SetFileAge(oldTemp, 3600);
        SetFileAge(oldMetaTemp, 3600);
        SetFileAge(lookalike, 3600);

        cache.Prune();

        CHECK_FALSE(fs::exists(oldTemp));
        CHECK_FALSE(fs::exists(oldMetaTemp));
        CHECK(fs::exists(youngTemp)); // a writer may still be producing it
        CHECK(fs::exists(lookalike)); // not base + the exact temp suffix: a different file
        LocalAttachment hit;
        REQUIRE(cache.Lookup(kUrl, hit));
        CHECK(ReadAll(hit.FilePath) == "pdf-bytes");
    }

    TEST_CASE("orphaned temps do not count toward an entry's size when the cap is applied") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path(), /*capBytes=*/250, /*graceSeconds=*/60);
        REQUIRE(cache.Store(kUrl, "f", Bytes(std::string(100, 'x'))).has_value());
        const fs::path entryDir = fs::path(dir.Path()) / "attachment_cache" / EntryKeyForUrl(kUrl);
        const fs::path orphan = entryDir / "f.png.part-1-1"; // Bytes() defaults to image/png, so the file is f.png
        REQUIRE(fs::exists(entryDir / "f.png"));
        WriteText(orphan, std::string(100, 'j'));
        SetFileAge(orphan, 3600);
        SetLastUsed(entryDir.string(), 3600);
        SetFileAge(entryDir / "f.png", 3600);
        SetFileAge(entryDir / "entry.json", 3600);

        cache.Prune();

        // 100 bytes of data + ~100 bytes of metadata fit the 250-byte cap only while the 100-byte orphan is not
        // counted.
        LocalAttachment hit;
        CHECK(cache.Lookup(kUrl, hit));
        CHECK_FALSE(fs::exists(orphan));
    }

    TEST_CASE("storing identical bytes again writes nothing, so a blocked replace is not an error") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        const LocalResult first = cache.Store(kUrl, "diagram", Bytes("pixels"));
        REQUIRE(first.has_value());
        const TempNameSquatter squat(first.value().FilePath);
        // Identical bytes: nothing needs replacing, so the blocked write is not an error.
        const LocalResult again = cache.Store(kUrl, "diagram", Bytes("pixels"));
        REQUIRE(again.has_value());
        CHECK(ReadAll(again.value().FilePath) == "pixels");
    }

    TEST_CASE("a live data file named like a temp survives Prune") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path(), /*capBytes=*/1u << 20, /*graceSeconds=*/60);
        const LocalResult stored =
            cache.Store(kUrl, "entry.json.part-1-2", Bytes("pixels", "application/octet-stream"));
        REQUIRE(stored.has_value());
        const fs::path dataFile = stored.value().FilePath;
        REQUIRE(dataFile.filename() == "entry.json.part-1-2");
        SetFileAge(dataFile, 3600);
        cache.Prune();
        CHECK(fs::exists(dataFile));
        LocalAttachment hit;
        REQUIRE(cache.Lookup(kUrl, hit));
        CHECK(ReadAll(hit.FilePath) == "pixels");
    }

    TEST_CASE("sweeping orphaned temps does not refresh the entry's age") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path(), /*capBytes=*/1u << 20, /*graceSeconds=*/60);
        REQUIRE(cache.Store(kUrl, "f", Bytes("x")).has_value());
        const fs::path entryDir = fs::path(dir.Path()) / "attachment_cache" / EntryKeyForUrl(kUrl);
        const fs::path orphan = entryDir / "f.png.part-1-1";
        WriteText(orphan, "junk");
        SetFileAge(orphan, 3600);
        SetLastUsed(entryDir.string(), 3600); // the whole entry is old
        std::error_code ec;
        const fs::file_time_type before = fs::last_write_time(entryDir, ec);
        cache.Prune();
        REQUIRE_FALSE(fs::exists(orphan));
        CHECK(fs::last_write_time(entryDir, ec) == before);
    }

    TEST_CASE("a blocked replace of different bytes fails and leaves the old file") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        const LocalResult first = cache.Store(kUrl, "diagram", Bytes("pixels"));
        REQUIRE(first.has_value());
        const TempNameSquatter squat(first.value().FilePath);
        // Same size, different content: the old size-only rule would have called this the same file.
        const LocalResult other = cache.Store(kUrl, "diagram", Bytes("PIXELS"));
        CHECK_FALSE(other.has_value());
        CHECK(ReadAll(first.value().FilePath) == "pixels");
    }

#if !defined(_WIN32)
    TEST_CASE("POSIX: cache files are owner-only") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        const LocalResult stored = cache.Store(kUrl, "diagram", Bytes("pixels"));
        REQUIRE(stored.has_value());
        const fs::path entryDir = fs::path(stored.value().FilePath).parent_path();
        for (const fs::path& f : {fs::path(stored.value().FilePath), entryDir / "entry.json"}) {
            struct stat st {};
            REQUIRE(::stat(f.string().c_str(), &st) == 0);
            CHECK((st.st_mode & 0777) == (S_IRUSR | S_IWUSR));
        }
    }
#endif

    TEST_CASE("a namespaced entry is served only to its own namespace") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        const std::string nsA = "Jira@site.atlassian.net#aaaaaaaaaaaa";
        const std::string nsB = "Jira@site.atlassian.net#bbbbbbbbbbbb";
        REQUIRE(cache.Store(nsA, kUrl, "diagram", Bytes("account-a")).has_value());

        LocalAttachment hit;
        CHECK(cache.Lookup(nsA, kUrl, hit));
        CHECK(ReadAll(hit.FilePath) == "account-a");
        CHECK_FALSE(cache.Lookup(nsB, kUrl, hit)); // another account on the same site
        CHECK_FALSE(cache.Lookup(kUrl, hit));      // the un-namespaced (legacy) view

        // An entry an older build saved without a namespace is not served to a namespaced lookup.
        REQUIRE(cache.Store(kUrl, "diagram", Bytes("legacy")).has_value());
        CHECK(cache.Lookup(kUrl, hit));
        CHECK(ReadAll(hit.FilePath) == "legacy");
        CHECK(cache.Lookup(nsA, kUrl, hit));
        CHECK(ReadAll(hit.FilePath) == "account-a");
        CHECK(fs::exists(fs::path(dir.Path()) / "attachment_cache" / EntryKeyFor(nsA, kUrl)));
    }

    TEST_CASE("a tampered namespace in entry.json is a miss") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        const std::string nsA = "Jira@site.atlassian.net#aaaaaaaaaaaa";
        REQUIRE(cache.Store(nsA, kUrl, "diagram", Bytes("pixels")).has_value());
        const fs::path meta = fs::path(dir.Path()) / "attachment_cache" / EntryKeyFor(nsA, kUrl) / "entry.json";
        std::ofstream(meta.string(), std::ios::trunc)
            << R"({"url":")" << kUrl << R"(","file":"diagram.png","ns":"Jira@other#zzzzzzzzzzzz"})";
        LocalAttachment hit;
        CHECK_FALSE(cache.Lookup(nsA, kUrl, hit));
    }

    TEST_CASE("ResolveAttachment downloads once per namespace") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        int fetches = 0;
        const auto fetch = [&fetches]() {
            ++fetches;
            return FetchResult::Ok(Bytes("pixels"));
        };
        AttachmentRequest a = Request();
        a.Namespace = "Jira@site.atlassian.net#aaaaaaaaaaaa";
        AttachmentRequest b = Request();
        b.Namespace = "Jira@site.atlassian.net#bbbbbbbbbbbb";
        REQUIRE(ResolveAttachment(cache, a, fetch, NoUncachedWrite).has_value());
        REQUIRE(ResolveAttachment(cache, a, fetch, NoUncachedWrite).has_value());
        CHECK(fetches == 1);
        REQUIRE(ResolveAttachment(cache, b, fetch, NoUncachedWrite).has_value());
        CHECK(fetches == 2);
        // Offline, another account's copy is not available.
        AttachmentRequest c = Request(/*offline=*/true);
        c.Namespace = "Jira@site.atlassian.net#cccccccccccc";
        const LocalResult miss = ResolveAttachment(cache, c, fetch, NoUncachedWrite);
        REQUIRE_FALSE(miss.has_value());
        CHECK(miss.error().Detail == kOfflineText);
    }

    TEST_CASE("PurgeAll removes every entry and nothing else") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        CHECK(cache.PurgeAll() == 0); // nothing to purge yet, no cache folder
        REQUIRE(cache.Store(kUrl, "a", Bytes("1")).has_value());
        REQUIRE(cache.Store("ns", kUrl, "a", Bytes("2")).has_value());
        REQUIRE(cache.Store("https://site.atlassian.net/a/9", "b", Bytes("3")).has_value());
        const fs::path root = fs::path(dir.Path()) / "attachment_cache";
        fs::create_directories(root / "not-an-entry");
        WriteText(root / "stray.txt", "keep");
        fs::create_directories(fs::path(dir.Path()) / "other_folder");

        CHECK(cache.PurgeAll() == 3);
        LocalAttachment hit;
        CHECK_FALSE(cache.Lookup(kUrl, hit));
        CHECK_FALSE(cache.Lookup("ns", kUrl, hit));
        CHECK(fs::exists(root / "not-an-entry"));
        CHECK(fs::exists(root / "stray.txt"));
        CHECK(fs::exists(fs::path(dir.Path()) / "other_folder"));
        // The cache works again after a purge.
        REQUIRE(cache.Store(kUrl, "a", Bytes("4")).has_value());
        CHECK(cache.Lookup(kUrl, hit));
    }

    TEST_CASE("UserDataDir reports the folder the cache was built for") {
        ScopedTempDir dir;
        AttachmentDiskCache cache(dir.Path());
        CHECK(cache.UserDataDir() == dir.Path());
    }
}
