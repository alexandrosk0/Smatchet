// smatchet::fileio::WriteFileAtomically / FileContentEquals / UniqueTempSuffix / IsTempSiblingOf —
// the binary atomic-replace primitive behind ConfigManager::AtomicWriteTextFile and the attachment and
// icon caches. Real files in a private temp directory; the POSIX-only assertions pin the security
// properties (0600, exclusive no-follow temp) on the platform where they apply.

#include "FileIo.h"

#include "ScopedTempDir.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = ghc::filesystem;
using smatchet::fileio::AtomicWriteOptions;
using smatchet::fileio::FileContentEquals;
using smatchet::fileio::IsTempSiblingOf;
using smatchet::fileio::UniqueTempSuffix;
using smatchet::fileio::WriteFileAtomically;
using smatchet_tests::ScopedTempDir;

namespace {

std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> ListNames(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool Write(const std::string& path, const std::string& bytes, const AtomicWriteOptions& opt = AtomicWriteOptions()) {
    std::string err;
    return WriteFileAtomically(path, bytes.data(), bytes.size(), opt, err);
}

} // namespace

TEST_SUITE("FileIoAtomicWrite") {

    TEST_CASE("round-trips binary content, overwrites, and leaves no temp sibling behind") {
        ScopedTempDir dir;
        const std::string path = dir.File("data.bin");
        const std::string bytes("\x00\x01\xff\x00"
                                "tail",
                                8);
        REQUIRE(Write(path, bytes));
        CHECK(ReadAll(path) == bytes);
        REQUIRE(Write(path, "v2"));
        CHECK(ReadAll(path) == "v2");
        REQUIRE(Write(path, ""));
        CHECK(ReadAll(path).empty());
        CHECK(ListNames(dir.Path()) == std::vector<std::string>{"data.bin"});
    }

    TEST_CASE("a multi-megabyte write lands whole") {
        ScopedTempDir dir;
        std::string big(3 * 1024 * 1024 + 17, 'x');
        for (std::size_t i = 0; i < big.size(); i += 4099) {
            big[i] = static_cast<char>(i & 0xff);
        }
        const std::string path = dir.File("big.bin");
        AtomicWriteOptions opt;
        opt.Durable = true;
        REQUIRE(Write(path, big, opt));
        CHECK(ReadAll(path) == big);
    }

    TEST_CASE("null data is accepted only for an empty write") {
        ScopedTempDir dir;
        std::string err;
        const AtomicWriteOptions opt;
        CHECK(WriteFileAtomically(dir.File("empty"), nullptr, 0, opt, err));
        CHECK(err.empty());
        CHECK_FALSE(WriteFileAtomically(dir.File("bad"), nullptr, 4, opt, err));
        CHECK_FALSE(err.empty());
        CHECK_FALSE(fs::exists(dir.File("bad")));
        CHECK_FALSE(WriteFileAtomically("", "x", 1, opt, err));
        CHECK_FALSE(err.empty());
    }

    TEST_CASE("a missing parent directory fails with a reason and creates nothing") {
        ScopedTempDir dir;
        std::string err;
        const AtomicWriteOptions opt;
        CHECK_FALSE(WriteFileAtomically(dir.File("nope/child.bin"), "x", 1, opt, err));
        CHECK_FALSE(err.empty());
        CHECK(ListNames(dir.Path()).empty());
    }

    TEST_CASE("a failed publish removes the temp and leaves the target untouched") {
        ScopedTempDir dir;
        // The target is a non-empty directory: the rename cannot replace it.
        const std::string target = dir.File("occupied");
        REQUIRE(fs::create_directories(fs::path(target) / "inner"));
        std::string err;
        const AtomicWriteOptions opt;
        CHECK_FALSE(WriteFileAtomically(target, "x", 1, opt, err));
        CHECK_FALSE(err.empty());
        CHECK(ListNames(dir.Path()) == std::vector<std::string>{"occupied"});
        CHECK(fs::is_directory(target));
    }

#if !defined(_WIN32)
    TEST_CASE("POSIX: owner-only by default, umask-governed when OwnerOnly is off") {
        ScopedTempDir dir;
        const mode_t oldMask = ::umask(022);
        const std::string secret = dir.File("secret");
        const std::string shared = dir.File("shared");
        AtomicWriteOptions world;
        world.OwnerOnly = false;
        const bool secretOk = Write(secret, "s");
        const bool sharedOk = Write(shared, "p", world);
        ::umask(oldMask);
        REQUIRE(secretOk);
        REQUIRE(sharedOk);
        struct stat st {};
        REQUIRE(::stat(secret.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == (S_IRUSR | S_IWUSR));
        REQUIRE(::stat(shared.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
    }

    TEST_CASE("POSIX: an overwrite keeps owner-only even when the old file was world-readable") {
        ScopedTempDir dir;
        const std::string path = dir.File("cfg");
        REQUIRE(Write(path, "old"));
        REQUIRE(::chmod(path.c_str(), 0644) == 0);
        REQUIRE(Write(path, "new"));
        struct stat st {};
        REQUIRE(::stat(path.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == (S_IRUSR | S_IWUSR));
    }

    TEST_CASE("POSIX: a symlink planted at the next temp names is never written through") {
        ScopedTempDir dir;
        const std::string path = dir.File("target.bin");
        const std::string decoy = dir.File("decoy.txt");
        {
            std::ofstream d(decoy, std::ios::binary);
            d << "decoy-contents";
        }
        // Predict the next suffixes from the shared counter and plant a symlink at each of the following
        // several names: the exclusive no-follow create must skip every one of them and still publish.
        const std::string probe = UniqueTempSuffix(); // ".part-<pid>-<k>"
        const std::size_t dash = probe.rfind('-');
        REQUIRE(dash != std::string::npos);
        const std::string prefix = probe.substr(0, dash + 1);
        const std::uint64_t k = std::stoull(probe.substr(dash + 1));
        std::vector<std::string> planted;
        for (std::uint64_t i = 1; i <= 3; ++i) {
            const std::string link = path + prefix + std::to_string(k + i);
            REQUIRE(::symlink(decoy.c_str(), link.c_str()) == 0);
            planted.push_back(link);
        }
        REQUIRE(Write(path, "SECRET"));
        CHECK(ReadAll(path) == "SECRET");
        CHECK(ReadAll(decoy) == "decoy-contents");
        for (std::size_t i = 0; i < planted.size(); ++i) {
            struct stat st {};
            CHECK(::lstat(planted[i].c_str(), &st) == 0); // the planted links are left alone
            CHECK(S_ISLNK(st.st_mode));
        }
    }

    TEST_CASE("POSIX: giving up after repeated collisions fails cleanly") {
        ScopedTempDir dir;
        const std::string path = dir.File("target.bin");
        const std::string probe = UniqueTempSuffix();
        const std::size_t dash = probe.rfind('-');
        const std::string prefix = probe.substr(0, dash + 1);
        const std::uint64_t k = std::stoull(probe.substr(dash + 1));
        // Occupy a long run of upcoming names with plain files.
        for (std::uint64_t i = 1; i <= 64; ++i) {
            std::ofstream((path + prefix + std::to_string(k + i)).c_str()) << "x";
        }
        std::string err;
        const AtomicWriteOptions opt;
        CHECK_FALSE(WriteFileAtomically(path, "y", 1, opt, err));
        CHECK_FALSE(err.empty());
        CHECK_FALSE(fs::exists(path));
    }
#endif

    TEST_CASE("UniqueTempSuffix is unique across threads and matches IsTempSiblingOf") {
        static constexpr int kThreads = 8;
        static constexpr int kPerThread = 100;
        std::vector<std::vector<std::string>> perThread(kThreads);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&perThread, t]() {
                for (int i = 0; i < kPerThread; ++i) {
                    perThread[static_cast<std::size_t>(t)].push_back(UniqueTempSuffix());
                }
            });
        }
        for (std::size_t t = 0; t < threads.size(); ++t) {
            threads[t].join();
        }
        std::set<std::string> all;
        for (std::size_t t = 0; t < perThread.size(); ++t) {
            for (std::size_t i = 0; i < perThread[t].size(); ++i) {
                all.insert(perThread[t][i]);
                CHECK(IsTempSiblingOf("spec.pdf" + perThread[t][i], "spec.pdf"));
            }
        }
        CHECK(all.size() == static_cast<std::size_t>(kThreads) * kPerThread);
    }

    TEST_CASE("IsTempSiblingOf applies the exact suffix rule") {
        CHECK(IsTempSiblingOf("spec.pdf.part-12-3", "spec.pdf"));
        CHECK(IsTempSiblingOf("entry.json.part-1-0", "entry.json"));
        CHECK_FALSE(IsTempSiblingOf("spec.pdf", "spec.pdf"));          // the base itself
        CHECK_FALSE(IsTempSiblingOf("spec.part-2.pdf", "spec.pdf"));   // a different file
        CHECK_FALSE(IsTempSiblingOf("spec.pdf.part-2", "spec.pdf"));   // no counter
        CHECK_FALSE(IsTempSiblingOf("spec.pdf.part-2-x", "spec.pdf")); // non-digit counter
        CHECK_FALSE(IsTempSiblingOf("spec.pdf.part--3", "spec.pdf"));  // empty pid
        CHECK_FALSE(IsTempSiblingOf("spec.pdf.part-2-", "spec.pdf"));  // empty counter
        CHECK_FALSE(IsTempSiblingOf("spec.pdf.part-2-3.bak", "spec.pdf"));
        CHECK_FALSE(IsTempSiblingOf("xspec.pdf.part-2-3", "spec.pdf")); // different base
        CHECK_FALSE(IsTempSiblingOf("spec.pdf.tmp", "spec.pdf"));       // a legacy suffix is not a temp
        CHECK_FALSE(IsTempSiblingOf("", "spec.pdf"));
        CHECK(IsTempSiblingOf(".part-1-2", "")); // an empty base degenerates, no crash
    }

    TEST_CASE("FileContentEquals compares exact bytes") {
        ScopedTempDir dir;
        const std::string path = dir.File("f.bin");
        const std::string bytes("abc\0def", 7);
        REQUIRE(Write(path, bytes));
        CHECK(FileContentEquals(path, bytes.data(), bytes.size()));
        CHECK_FALSE(FileContentEquals(path, "abc", 3));              // file is longer
        CHECK_FALSE(FileContentEquals(path, "abc\0def!", 8));        // file is shorter
        CHECK_FALSE(FileContentEquals(path, "abc\0deX", 7));         // last byte differs
        CHECK_FALSE(FileContentEquals(dir.File("missing"), "x", 1)); // no such file
        CHECK_FALSE(FileContentEquals(path, nullptr, 3));            // null with a size
        REQUIRE(Write(dir.File("empty"), ""));
        CHECK(FileContentEquals(dir.File("empty"), nullptr, 0));
        CHECK_FALSE(FileContentEquals(dir.File("empty"), "x", 1));
        CHECK_FALSE(FileContentEquals(path, nullptr, 0)); // non-empty file vs empty data
    }

    TEST_CASE("FileContentEquals spans chunk boundaries") {
        ScopedTempDir dir;
        std::string big(200 * 1024 + 3, 'a');
        const std::string path = dir.File("big.bin");
        REQUIRE(Write(path, big));
        CHECK(FileContentEquals(path, big.data(), big.size()));
        big.back() = 'b';
        CHECK_FALSE(FileContentEquals(path, big.data(), big.size()));
        big.back() = 'a';
        big[64 * 1024] = 'z'; // first byte of the second chunk
        CHECK_FALSE(FileContentEquals(path, big.data(), big.size()));
        CHECK_FALSE(FileContentEquals(path, big.data(), big.size() - 1));
    }

    TEST_CASE("FileContentEquals on a directory is false, not a crash") {
        ScopedTempDir dir;
        CHECK_FALSE(FileContentEquals(dir.Path(), "x", 1));
    }
}
