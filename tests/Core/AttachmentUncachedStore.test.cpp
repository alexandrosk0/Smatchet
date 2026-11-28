// AttachmentUncachedStore — where an attachment goes when the cache cannot hold it: always under
// <user data>/cache/attachment_tmp/, never the working directory or the system temp folder, owner-only, and
// swept once a day old.

#include "AttachmentUncachedStore.h"

#include "ScopedTempDir.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = ghc::filesystem;
using namespace smatchet::attachments;
using smatchet_tests::ScopedTempDir;

namespace {

std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

FetchedAttachment Fetched(const std::string& bytes, const std::string& mime = "image/png") {
    FetchedAttachment f;
    f.Bytes = bytes;
    f.Mime = mime;
    return f;
}

void SetAge(const fs::path& file, std::int64_t secondsAgo) {
    std::error_code ec;
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::seconds(secondsAgo), ec);
}

} // namespace

TEST_SUITE("AttachmentUncachedStore") {

    TEST_CASE("ResolveUncachedAttachmentDir: under the user data folder, empty when there is none") {
        CHECK(ResolveUncachedAttachmentDir("/home/u/.smatchet/") == "/home/u/.smatchet/cache/attachment_tmp/");
        CHECK(ResolveUncachedAttachmentDir("/home/u/.smatchet") == "/home/u/.smatchet/cache/attachment_tmp/");
        CHECK(ResolveUncachedAttachmentDir("C:\\Users\\u\\Smatchet\\") ==
              "C:\\Users\\u\\Smatchet\\cache/attachment_tmp/");
        // No fallback to ".", the system temp folder or anything else.
        CHECK(ResolveUncachedAttachmentDir("").empty());
    }

    TEST_CASE("SelectExpiredUncachedFiles: age boundary, future times and ordering") {
        const std::int64_t now = 1000000;
        const std::vector<EntryFileInfo> files = {
            {"b_old", now - kUncachedMaxAgeSeconds},       // exactly one day: expired
            {"a_older", now - 5 * kUncachedMaxAgeSeconds}, // expired
            {"young", now - kUncachedMaxAgeSeconds + 1},   // one second short: kept
            {"future", now + 500},                         // clock moved back: never expired
        };
        CHECK(SelectExpiredUncachedFiles(files, now, kUncachedMaxAgeSeconds) ==
              std::vector<std::string>{"a_older", "b_old"});
        CHECK(SelectExpiredUncachedFiles(std::vector<EntryFileInfo>(), now, kUncachedMaxAgeSeconds).empty());
    }

    TEST_CASE("WriteUncachedAttachment writes under cache/attachment_tmp with a sanitized unique name") {
        ScopedTempDir dir;
        const std::string base = dir.Path() + "/";
        const Result<LocalAttachment, TrackerError> a = WriteUncachedAttachment(base, "report", Fetched("one"));
        const Result<LocalAttachment, TrackerError> b = WriteUncachedAttachment(base, "report", Fetched("two"));
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(a.value().FilePath != b.value().FilePath); // same name twice, two files
        CHECK(ReadAll(a.value().FilePath) == "one");
        CHECK(ReadAll(b.value().FilePath) == "two");
        CHECK(a.value().Mime == "image/png");
        const fs::path expectedDir = fs::path(dir.Path()) / "cache" / "attachment_tmp";
        CHECK(fs::path(a.value().FilePath).parent_path() == expectedDir);
        CHECK(fs::path(a.value().FilePath).extension() == ".png"); // derived from the MIME type

        // A hostile name cannot leave the folder.
        const Result<LocalAttachment, TrackerError> evil =
            WriteUncachedAttachment(base, "../../etc/passwd", Fetched("x", "text/plain"));
        REQUIRE(evil.has_value());
        CHECK(fs::path(evil.value().FilePath).parent_path() == expectedDir);
    }

    TEST_CASE("WriteUncachedAttachment without a user data folder fails instead of using the working directory") {
        const Result<LocalAttachment, TrackerError> r = WriteUncachedAttachment("", "report", Fetched("x"));
        REQUIRE_FALSE(r.has_value());
        CHECK_FALSE(r.error().Detail.empty());
    }

    TEST_CASE("WriteUncachedAttachment fails when the folder cannot be created") {
        ScopedTempDir dir;
        std::ofstream(dir.File("plain-file")) << "x"; // the "user data folder" is a regular file
        const Result<LocalAttachment, TrackerError> r =
            WriteUncachedAttachment(dir.File("plain-file") + "/", "report", Fetched("x"));
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("a write sweeps copies older than a day and keeps the rest") {
        ScopedTempDir dir;
        const std::string base = dir.Path() + "/";
        const fs::path tmp = fs::path(dir.Path()) / "cache" / "attachment_tmp";
        fs::create_directories(tmp);
        std::ofstream((tmp / "old.png").string()) << "old";
        std::ofstream((tmp / "recent.png").string()) << "recent";
        SetAge(tmp / "old.png", 2 * kUncachedMaxAgeSeconds);
        SetAge(tmp / "recent.png", 3600);
        const Result<LocalAttachment, TrackerError> r = WriteUncachedAttachment(base, "new", Fetched("fresh"));
        REQUIRE(r.has_value());
        CHECK_FALSE(fs::exists(tmp / "old.png"));
        CHECK(fs::exists(tmp / "recent.png"));
        CHECK(fs::exists(r.value().FilePath));
    }

    TEST_CASE("SweepUncachedAttachments counts removals and tolerates a missing folder") {
        ScopedTempDir dir;
        CHECK(SweepUncachedAttachments(dir.File("nope") + "/") == 0);
        const fs::path tmp = fs::path(dir.Path()) / "t";
        fs::create_directories(tmp);
        std::ofstream((tmp / "a").string()) << "a";
        std::ofstream((tmp / "b").string()) << "b";
        fs::create_directories(tmp / "subdir"); // directories are not copies: left alone
        SetAge(tmp / "a", 2 * kUncachedMaxAgeSeconds);
        SetAge(tmp / "b", 2 * kUncachedMaxAgeSeconds);
        CHECK(SweepUncachedAttachments(tmp.string()) == 2);
        CHECK(fs::exists(tmp / "subdir"));
    }

#if !defined(_WIN32)
    TEST_CASE("POSIX: the copy is owner-only") {
        ScopedTempDir dir;
        const Result<LocalAttachment, TrackerError> r =
            WriteUncachedAttachment(dir.Path() + "/", "report", Fetched("secret"));
        REQUIRE(r.has_value());
        struct stat st {};
        REQUIRE(::stat(r.value().FilePath.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == (S_IRUSR | S_IWUSR));
    }
#endif
}
