// audit H2 follow-up — AtomicWriteTextFile POSIX hardening (O_NOFOLLOW + fchmod 0600).
//
// AtomicWriteTextFile is the single writer for the secret-bearing user config. On POSIX it now
// opens the temp file through a raw fd with O_NOFOLLOW | O_CREAT | O_TRUNC at mode 0600 and
// fchmods the fd before writing, then atomic-renames. These tests pin the two security
// properties on the platform where they apply (the assertions are POSIX-only — the Windows path
// is DPAPI + MoveFileEx and is not exercised here; the cross-platform round-trip still runs
// everywhere). The write path is filesystem-bound, so it lives in its own test rather than the
// pure-logic ConfigSecretFilePerms decision test.

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

// A unique-ish temp path in the test's working directory (ctest runs in a writable build dir),
// so the test needs no /tmp assumptions and no shared fixtures.
std::string TempBase(const char* tag) {
    return std::string("./atomicwrite_") + tag + ".cfgtest";
}

std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("AtomicWriteTextFile: round-trips content and overwrites (all platforms)") {
    const std::string path = TempBase("roundtrip");
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());

    CHECK(ConfigManager::AtomicWriteTextFile(path, "{\"token\":\"abc\"}"));
    CHECK(ReadAll(path) == "{\"token\":\"abc\"}");

    // Overwrite an existing file (the common config-save case).
    CHECK(ConfigManager::AtomicWriteTextFile(path, "v2"));
    CHECK(ReadAll(path) == "v2");

    // Empty content is a valid write (e.g. a config reset).
    CHECK(ConfigManager::AtomicWriteTextFile(path, ""));
    CHECK(ReadAll(path).empty());

    std::remove(path.c_str());
}

#if !defined(_WIN32)
TEST_CASE("AtomicWriteTextFile: POSIX writes land owner-only (0600)") {
    const std::string path = TempBase("perms");
    std::remove(path.c_str());

    REQUIRE(ConfigManager::AtomicWriteTextFile(path, "{\"github_pat\":\"secret\"}"));

    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    // The secret-bearing config must not be group/world readable — only the owner bits may be set.
    CHECK((st.st_mode & 0777) == (S_IRUSR | S_IWUSR));

    std::remove(path.c_str());
}

TEST_CASE("AtomicWriteTextFile: POSIX O_NOFOLLOW refuses a symlinked temp file") {
    const std::string path = TempBase("nofollow");
    const std::string tmp = path + ".tmp";
    const std::string decoy = TempBase("nofollow_decoy");
    std::remove(path.c_str());
    std::remove(tmp.c_str());
    std::remove(decoy.c_str());

    // A would-be attacker plants a symlink at '<path>.tmp' pointing at another file. O_NOFOLLOW
    // must make the open() fail (ELOOP) so the write — and its 0600 — cannot be diverted.
    {
        std::ofstream d(decoy, std::ios::binary);
        d << "original-decoy-contents";
    }
    REQUIRE(::symlink(decoy.c_str(), tmp.c_str()) == 0);

    // The write must be refused, not followed through the link.
    CHECK_FALSE(ConfigManager::AtomicWriteTextFile(path, "SECRET-SHOULD-NOT-LAND"));

    // The decoy target is untouched, and no real config file was published.
    CHECK(ReadAll(decoy) == "original-decoy-contents");
    struct stat st {};
    CHECK(::lstat(path.c_str(), &st) != 0);  // '<path>' was never created.

    std::remove(tmp.c_str());  // remove the symlink itself.
    std::remove(decoy.c_str());
    std::remove(path.c_str());
}
#endif  // !_WIN32
