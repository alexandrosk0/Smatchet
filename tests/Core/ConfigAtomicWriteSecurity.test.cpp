// audit H2 follow-up — AtomicWriteTextFile POSIX hardening (O_NOFOLLOW + fchmod 0600).
//
// AtomicWriteTextFile is the single writer for the secret-bearing user config. It delegates to
// smatchet::fileio::WriteFileAtomically: on POSIX a unique temp sibling is created O_EXCL | O_NOFOLLOW
// at mode 0600 and the fd is fchmod'ed before writing, then atomic-renamed. These tests pin the two security
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
#include <dirent.h>
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

TEST_CASE("AtomicWriteTextFile: POSIX a symlink planted at '<path>.tmp' is never written through") {
    const std::string path = TempBase("nofollow");
    const std::string legacyTmp = path + ".tmp";
    const std::string decoy = TempBase("nofollow_decoy");
    std::remove(path.c_str());
    std::remove(legacyTmp.c_str());
    std::remove(decoy.c_str());

    // The writer used to stage through the fixed name '<path>.tmp', so a planted symlink there was the
    // attack. It now stages through a unique exclusive no-follow sibling ('<path>.part-<pid>-<n>'), so
    // the planted link is simply not part of the write: the config lands, and the decoy is untouched.
    {
        std::ofstream d(decoy, std::ios::binary);
        d << "original-decoy-contents";
    }
    REQUIRE(::symlink(decoy.c_str(), legacyTmp.c_str()) == 0);

    CHECK(ConfigManager::AtomicWriteTextFile(path, "NEW-CONFIG"));
    CHECK(ReadAll(path) == "NEW-CONFIG");
    CHECK(ReadAll(decoy) == "original-decoy-contents");
    struct stat st {};
    REQUIRE(::lstat(legacyTmp.c_str(), &st) == 0);
    CHECK(S_ISLNK(st.st_mode)); // left alone: nothing followed it, nothing removed it.

    std::remove(legacyTmp.c_str()); // remove the symlink itself.
    std::remove(decoy.c_str());
    std::remove(path.c_str());
}

TEST_CASE("AtomicWriteTextFile: POSIX leaves no temp sibling behind, success or failure") {
    const std::string path = TempBase("notemp");
    std::remove(path.c_str());
    REQUIRE(ConfigManager::AtomicWriteTextFile(path, "v1"));

    // The directory holds the config and nothing staged next to it ('.tmp' or '.part-*').
    bool strayTemp = false;
    const std::string base = path.substr(path.find_last_of('/') + 1);
    DIR* dir = ::opendir(".");
    REQUIRE(dir != nullptr);
    while (const dirent* ent = ::readdir(dir)) {
        const std::string name = ent->d_name;
        if (name.compare(0, base.size(), base) == 0 && name != base) {
            strayTemp = true;
        }
    }
    ::closedir(dir);
    CHECK_FALSE(strayTemp);

    // A write that cannot publish (target is a non-empty directory) fails, keeps the target, and stages nothing.
    const std::string blocked = TempBase("blocked");
    std::remove(blocked.c_str());
    REQUIRE(::mkdir(blocked.c_str(), 0700) == 0);
    REQUIRE(::mkdir((blocked + "/child").c_str(), 0700) == 0);
    CHECK_FALSE(ConfigManager::AtomicWriteTextFile(blocked, "x"));
    // The failed publish removed its temp: nothing named '<blocked>.part-*' remains next to it.
    const std::string blockedBase = blocked.substr(blocked.find_last_of('/') + 1);
    bool strayAfterFailure = false;
    DIR* cwd = ::opendir(".");
    REQUIRE(cwd != nullptr);
    while (const dirent* ent = ::readdir(cwd)) {
        const std::string name = ent->d_name;
        if (name.compare(0, blockedBase.size(), blockedBase) == 0 && name != blockedBase) {
            strayAfterFailure = true;
        }
    }
    ::closedir(cwd);
    CHECK_FALSE(strayAfterFailure);
    struct stat st {};
    CHECK(::stat(blocked.c_str(), &st) == 0);
    CHECK(S_ISDIR(st.st_mode));

    ::rmdir((blocked + "/child").c_str());
    ::rmdir(blocked.c_str());
    std::remove(path.c_str());
}
#endif  // !_WIN32
