// PathConfinement.test.cpp — unit coverage for the MCP file-command path
// confinement leaf (Source/Core/include/Commands/PathConfinement.h). The header is
// what bounds perf.dump / scenario.run / ui_test.run / whisper.transcribe-once
// file args so a caller reachable from the MCP command surface cannot read or write
// outside a dedicated <userData> subdir. These asserts pin the traversal
// rejections (empty / absolute / ".."), the happy path, and — the reason this file
// exists — the symlinked-subdir escape that the security-audit re-review flagged:
// a <userData>/whisper-import -> /tmp/outside symlink must NOT be accepted as the
// confinement base. Header-only; no extra .cpp source on the rig line.

#include "Commands/PathConfinement.h"

#include <doctest/doctest.h>

#include <ghc/filesystem.hpp>

#include <string>
#include <system_error>

namespace {

namespace fs = ghc::filesystem;
using smatchet::cmd::ConfinePathUnderBase;
using smatchet::cmd::ConfinePathUnderSubdir;

// A scratch directory under the system temp dir, wiped at construction so a prior
// run's leftovers can't mask a regression. The fixed name is fine for a single
// doctest process (Date/random helpers are intentionally avoided).
fs::path MakeScratchBase(const char* name) {
    fs::path base = fs::temp_directory_path() / "smatchet-confine-test" / name;
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

// True when `resolved` sits inside `base` (no `..` escape), the containment
// invariant every success path must satisfy.
bool ContainedUnder(const std::string& resolved, const fs::path& base) {
    std::error_code ec;
    fs::path canonBase = fs::weakly_canonical(base, ec);
    if (ec)
        canonBase = base.lexically_normal();
    const fs::path rel = fs::path(resolved).lexically_relative(canonBase);
    return !rel.empty() && rel.begin()->string() != "..";
}

} // namespace

TEST_CASE("PathConfinement · ConfinePathUnderBase rejects traversal and accepts a relative path") {
    const fs::path base = MakeScratchBase("base");
    std::string resolved, err;

    SUBCASE("empty candidate is rejected") {
        CHECK_FALSE(ConfinePathUnderBase(base.string(), "", resolved, err));
        CHECK_FALSE(err.empty());
    }
    SUBCASE("absolute candidate is rejected") {
        CHECK_FALSE(ConfinePathUnderBase(base.string(), (base / "x.json").string(), resolved, err));
        CHECK_FALSE(err.empty());
    }
    SUBCASE("'..' traversal is rejected") {
        CHECK_FALSE(ConfinePathUnderBase(base.string(), "../escape.json", resolved, err));
        CHECK_FALSE(err.empty());
    }
    SUBCASE("a valid relative path resolves inside the base") {
        REQUIRE(ConfinePathUnderBase(base.string(), "sub/ok.json", resolved, err));
        CHECK(err.empty());
        CHECK(ContainedUnder(resolved, base));
    }
}

TEST_CASE("PathConfinement · ConfinePathUnderSubdir creates and confines under the subdir") {
    const fs::path base = MakeScratchBase("subbase");
    std::string resolved, err;
    REQUIRE(ConfinePathUnderSubdir(base.string(), "perf", "snapshot.json", resolved, err));
    CHECK(err.empty());
    CHECK(fs::is_directory(base / "perf"));
    CHECK(ContainedUnder(resolved, base / "perf"));

    // The traversal rejections still apply through the subdir wrapper.
    std::string r2, e2;
    CHECK_FALSE(ConfinePathUnderSubdir(base.string(), "perf", "../../escape.json", r2, e2));
    CHECK_FALSE(e2.empty());
}

TEST_CASE("PathConfinement · ConfinePathUnderSubdir validates the subdir argument") {
    const fs::path base = MakeScratchBase("subdirval");
    std::string resolved, err;

    SUBCASE("empty subdir is rejected") {
        CHECK_FALSE(ConfinePathUnderSubdir(base.string(), "", "x.json", resolved, err));
        CHECK_FALSE(err.empty());
    }
    SUBCASE("'..' in the subdir is rejected before any mkdir") {
        CHECK_FALSE(ConfinePathUnderSubdir(base.string(), "../escape", "x.json", resolved, err));
        CHECK_FALSE(err.empty());
        CHECK_FALSE(fs::exists(base.parent_path() / "escape")); // nothing was created outside base
    }
    SUBCASE("'.' subdir (no dedicated dir) is rejected") {
        CHECK_FALSE(ConfinePathUnderSubdir(base.string(), ".", "x.json", resolved, err));
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("PathConfinement · ConfinePathUnderSubdir rejects a symlinked subdir (escape regression)") {
    const fs::path base = MakeScratchBase("symbase");
    const fs::path outside = MakeScratchBase("outside");
    std::error_code ec;
    // <base>/whisper-import -> <outside>. If the platform refuses symlink creation
    // (an unprivileged Windows runner), skip — POSIX CI still exercises the reject.
    fs::create_directory_symlink(outside, base / "whisper-import", ec);
    if (ec) {
        MESSAGE("symlink creation unsupported in this environment; skipping the symlink-escape assertion");
        return;
    }
    std::string resolved, err;
    CHECK_FALSE(ConfinePathUnderSubdir(base.string(), "whisper-import", "audio.wav", resolved, err));
    CHECK_FALSE(err.empty());
}
