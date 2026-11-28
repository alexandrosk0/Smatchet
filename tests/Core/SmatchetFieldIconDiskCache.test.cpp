// SmatchetFieldIconDiskCache — the worker-side disk half of the field-icon pipeline, against a private
// temp directory with a fake fetch and a fake decoder (no network, no stb, no ImGui).

#include "SmatchetFieldIconDiskCache.h"

#include "SmatchetFieldIconRender_detail.h"

#include "ScopedTempDir.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = ghc::filesystem;
using namespace SmatchetFieldIconDiskCache;
using smatchet_tests::ScopedTempDir;

namespace {

const char* const kUrl = "https://jira.example.com/images/icons/priorities/high.png";

IconBytes Bytes(const std::string& s) { return IconBytes(s.begin(), s.end()); }

// A "valid image" is any bytes starting with "OK"; its size encodes the decoded dimensions.
Result<DecodedIcon> FakeDecode(const unsigned char* bytes, std::size_t size) {
    if (size < 2 || bytes[0] != 'O' || bytes[1] != 'K') {
        return Result<DecodedIcon>::Err("not an image");
    }
    DecodedIcon icon;
    icon.Width = 2;
    icon.Height = 2;
    icon.Rgba.assign(16, 0x7f);
    return Result<DecodedIcon>::Ok(std::move(icon));
}

struct CountingFetch {
    int Calls = 0;
    Result<IconBytes> Reply = Result<IconBytes>::Ok(Bytes("OKpixels"));
    FetchFn Fn() {
        return [this](const std::string&) {
            ++Calls;
            return Reply;
        };
    }
};

std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void Write(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::vector<std::string> Names(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string CacheFile(const std::string& dir) {
    return dir + SmatchetFieldIconRender::detail::UrlToCacheFileName(kUrl);
}

} // namespace

TEST_SUITE("SmatchetFieldIconDiskCache") {

    TEST_CASE("EnsureCacheDir creates <user data>/cache/field_icons/ and refuses an empty or blocked base") {
        ScopedTempDir dir;
        const std::string base = dir.Path() + "/";
        std::string out, err;
        REQUIRE(EnsureCacheDir(base, out, err));
        CHECK(out == base + "cache/field_icons/");
        CHECK(fs::is_directory(out));
        CHECK(EnsureCacheDir(base, out, err)); // idempotent

        CHECK_FALSE(EnsureCacheDir("", out, err));
        CHECK_FALSE(err.empty());
        CHECK(out.empty());

        Write(dir.File("plainfile"), "x"); // a regular file where the base directory should be
        CHECK_FALSE(EnsureCacheDir(dir.File("plainfile") + "/", out, err));
        CHECK_FALSE(err.empty());
    }

    TEST_CASE("a miss fetches once, decodes, caches under the SHA-256 name; the next call is a disk hit") {
        ScopedTempDir dir;
        std::string cacheDir, err;
        REQUIRE(EnsureCacheDir(dir.Path() + "/", cacheDir, err));
        CountingFetch fetch;

        const IconResolveOutcome first = ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), FakeDecode);
        REQUIRE(first.Ok);
        CHECK(first.Source == IconSource::Network);
        CHECK(first.Icon.Width == 2);
        CHECK(fetch.Calls == 1);
        CHECK(ReadAll(CacheFile(cacheDir)) == "OKpixels");
        CHECK(Names(cacheDir).size() == 1); // no temp sibling left behind

        const IconResolveOutcome second = ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), FakeDecode);
        REQUIRE(second.Ok);
        CHECK(second.Source == IconSource::Disk);
        CHECK(fetch.Calls == 1);
    }

    TEST_CASE("a failed fetch fails the resolve and caches nothing") {
        ScopedTempDir dir;
        std::string cacheDir, err;
        REQUIRE(EnsureCacheDir(dir.Path() + "/", cacheDir, err));
        CountingFetch fetch;
        fetch.Reply = Result<IconBytes>::Err("HTTP status 404");
        const IconResolveOutcome out = ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), FakeDecode);
        CHECK_FALSE(out.Ok);
        CHECK(out.Error == "HTTP status 404");
        CHECK(Names(cacheDir).empty());
    }

    TEST_CASE("fetched bytes that do not decode are never cached") {
        ScopedTempDir dir;
        std::string cacheDir, err;
        REQUIRE(EnsureCacheDir(dir.Path() + "/", cacheDir, err));
        CountingFetch fetch;
        fetch.Reply = Result<IconBytes>::Ok(Bytes("<html>login page</html>"));
        const IconResolveOutcome out = ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), FakeDecode);
        CHECK_FALSE(out.Ok);
        CHECK(Names(cacheDir).empty());
    }

    TEST_CASE("a cached file that no longer decodes is removed and fetched again") {
        ScopedTempDir dir;
        std::string cacheDir, err;
        REQUIRE(EnsureCacheDir(dir.Path() + "/", cacheDir, err));
        Write(CacheFile(cacheDir), "garbage bytes from a crashed write");
        CountingFetch fetch;
        const IconResolveOutcome out = ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), FakeDecode);
        REQUIRE(out.Ok);
        CHECK(out.Source == IconSource::Network);
        CHECK(out.RemovedBadFile);
        CHECK(fetch.Calls == 1);
        CHECK(ReadAll(CacheFile(cacheDir)) == "OKpixels");
    }

    TEST_CASE("an oversized cache file is dropped without reading it into the decoder") {
        ScopedTempDir dir;
        std::string cacheDir, err;
        REQUIRE(EnsureCacheDir(dir.Path() + "/", cacheDir, err));
        Write(CacheFile(cacheDir), "OK" + std::string(kMaxIconFileBytes, 'x')); // cap + 2 bytes
        int decodeCalls = 0;
        const DecodeFn counting = [&decodeCalls](const unsigned char* b, std::size_t n) {
            ++decodeCalls;
            return FakeDecode(b, n);
        };
        CountingFetch fetch;
        const IconResolveOutcome out = ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), counting);
        REQUIRE(out.Ok);
        CHECK(out.RemovedBadFile);
        CHECK(decodeCalls == 1); // only the fetched body was decoded
        CHECK(ReadAll(CacheFile(cacheDir)) == "OKpixels");
    }

    TEST_CASE("an empty or oversized response is refused") {
        ScopedTempDir dir;
        std::string cacheDir, err;
        REQUIRE(EnsureCacheDir(dir.Path() + "/", cacheDir, err));
        CountingFetch fetch;
        fetch.Reply = Result<IconBytes>::Ok(IconBytes());
        CHECK_FALSE(ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), FakeDecode).Ok);
        fetch.Reply = Result<IconBytes>::Ok(IconBytes(kMaxIconFileBytes + 1, 'O'));
        CHECK_FALSE(ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), FakeDecode).Ok);
        CHECK(Names(cacheDir).empty());
    }

    TEST_CASE("a throwing fetch or decoder is a failed resolve, never an escaped exception") {
        ScopedTempDir dir;
        std::string cacheDir, err;
        REQUIRE(EnsureCacheDir(dir.Path() + "/", cacheDir, err));
        const FetchFn throwingFetch = [](const std::string&) -> Result<IconBytes> { throw std::runtime_error("boom"); };
        IconResolveOutcome out;
        CHECK_NOTHROW(out = ResolveUrlIcon(cacheDir, kUrl, throwingFetch, FakeDecode));
        CHECK_FALSE(out.Ok);
        CHECK(out.Error.find("boom") != std::string::npos);

        CountingFetch fetch;
        const DecodeFn throwingDecode = [](const unsigned char*, std::size_t) -> Result<DecodedIcon> {
            throw std::runtime_error("decoder exploded");
        };
        CHECK_NOTHROW(out = ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), throwingDecode));
        CHECK_FALSE(out.Ok);
        CHECK(Names(cacheDir).empty());
    }

    TEST_CASE("missing arguments fail cleanly") {
        CountingFetch fetch;
        CHECK_FALSE(ResolveUrlIcon("", kUrl, fetch.Fn(), FakeDecode).Ok);
        CHECK_FALSE(ResolveUrlIcon("/tmp/x/", "", fetch.Fn(), FakeDecode).Ok);
        CHECK_FALSE(ResolveFileIcon("", FakeDecode).Ok);
        CHECK(fetch.Calls == 0);
    }

    TEST_CASE("ResolveFileIcon: a good file decodes, and a bad or missing one fails without being touched") {
        ScopedTempDir dir;
        const std::string good = dir.File("good.png");
        const std::string bad = dir.File("bad.png");
        const std::string empty = dir.File("empty.png");
        Write(good, "OKfile");
        Write(bad, "not an image");
        Write(empty, "");

        const IconResolveOutcome ok = ResolveFileIcon(good, FakeDecode);
        REQUIRE(ok.Ok);
        CHECK(ok.Source == IconSource::LocalFile);

        CHECK_FALSE(ResolveFileIcon(bad, FakeDecode).Ok);
        CHECK(fs::exists(bad)); // never deleted: it is the user's file
        CHECK_FALSE(ResolveFileIcon(empty, FakeDecode).Ok);
        const IconResolveOutcome missing = ResolveFileIcon(dir.File("nope.png"), FakeDecode);
        CHECK_FALSE(missing.Ok);
        CHECK_FALSE(missing.Error.empty());
        CHECK(missing.NotFound); // an absent optional icon is expected, not a fault
        CHECK_FALSE(ResolveFileIcon(bad, FakeDecode).NotFound);
    }

    TEST_CASE("ResolveFileIcon: the read is bounded at kMaxIconFileBytes") {
        ScopedTempDir dir;
        const std::string atCap = dir.File("atcap.png");
        const std::string overCap = dir.File("overcap.png");
        Write(atCap, "OK" + std::string(kMaxIconFileBytes - 2, 'x'));
        Write(overCap, "OK" + std::string(kMaxIconFileBytes - 1, 'x'));
        CHECK(ResolveFileIcon(atCap, FakeDecode).Ok);
        const IconResolveOutcome over = ResolveFileIcon(overCap, FakeDecode);
        CHECK_FALSE(over.Ok);
        CHECK(fs::exists(overCap));
    }

#if !defined(_WIN32)
    TEST_CASE("POSIX: cached icons are owner-only") {
        ScopedTempDir dir;
        std::string cacheDir, err;
        REQUIRE(EnsureCacheDir(dir.Path() + "/", cacheDir, err));
        CountingFetch fetch;
        REQUIRE(ResolveUrlIcon(cacheDir, kUrl, fetch.Fn(), FakeDecode).Ok);
        struct stat st {};
        REQUIRE(::stat(CacheFile(cacheDir).c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == (S_IRUSR | S_IWUSR));
    }
#endif
}
