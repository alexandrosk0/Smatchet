#include <doctest/doctest.h>

#include "Commands/Scenarios/SpawnOutLogBasename.h"

#include <string>

using smatchet::cmd::MakeConfineSafeSpawnOutLogBasename;

namespace {

// A confine-safe basename carries no directory components and no traversal segment, so the
// child's ConfinePathUnderSubdir can anchor it under <userData>/ui-tests/ without rejecting it.
bool IsConfineSafe(const std::string& s) {
    return s.find('/') == std::string::npos && s.find('\\') == std::string::npos && s.find("..") == std::string::npos;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

TEST_CASE("MakeConfineSafeSpawnOutLogBasename collapses `..` traversal to the leaf" *
          doctest::test_suite("[security]")) {
    const std::string out = MakeConfineSafeSpawnOutLogBasename("../../etc/passwd");
    CHECK(EndsWith(out, "-passwd"));
    CHECK(IsConfineSafe(out));
}

TEST_CASE("MakeConfineSafeSpawnOutLogBasename strips an absolute path to its leaf" *
          doctest::test_suite("[security]")) {
    const std::string out = MakeConfineSafeSpawnOutLogBasename("C:/Windows/system32/x.txt");
    CHECK(EndsWith(out, "-x.txt"));
    CHECK(IsConfineSafe(out));
}

TEST_CASE("MakeConfineSafeSpawnOutLogBasename falls back to outlog.txt for no usable leaf" *
          doctest::test_suite("[security]")) {
    CHECK(EndsWith(MakeConfineSafeSpawnOutLogBasename("."), "-outlog.txt"));
    CHECK(EndsWith(MakeConfineSafeSpawnOutLogBasename(".."), "-outlog.txt"));
    CHECK(EndsWith(MakeConfineSafeSpawnOutLogBasename(""), "-outlog.txt"));
}

TEST_CASE("MakeConfineSafeSpawnOutLogBasename falls back when the leaf embeds `..`" *
          doctest::test_suite("[security]")) {
    // `foo..bar.txt` survives filename() (it is a single component, not a traversal segment) but
    // still contains `..`, violating the confine-safe contract and tripping child-side `..`
    // rejection — so it must fall back to the fixed safe name, not pass through.
    const std::string out = MakeConfineSafeSpawnOutLogBasename("foo..bar.txt");
    CHECK(EndsWith(out, "-outlog.txt"));
    CHECK(IsConfineSafe(out));
}

TEST_CASE("MakeConfineSafeSpawnOutLogBasename preserves a normal leaf filename") {
    const std::string out = MakeConfineSafeSpawnOutLogBasename("jira-fixture-outlog.txt");
    CHECK(EndsWith(out, "-jira-fixture-outlog.txt"));
    CHECK(IsConfineSafe(out));
}

TEST_CASE("MakeConfineSafeSpawnOutLogBasename always carries the spawn- prefix") {
    CHECK(StartsWith(MakeConfineSafeSpawnOutLogBasename("anything.txt"), "spawn-"));
    CHECK(StartsWith(MakeConfineSafeSpawnOutLogBasename("../escape"), "spawn-"));
}

TEST_CASE("MakeConfineSafeSpawnOutLogBasename returns a unique basename per call (entropy)" *
          doctest::test_suite("[security]")) {
    // Two calls with the SAME input must differ — a coarse millisecond stamp would collide when
    // both run in the same tick; entropy guarantees distinct names so concurrent spawns sharing
    // the confinement base never clobber each other.
    const std::string a = MakeConfineSafeSpawnOutLogBasename("outlog.txt");
    const std::string b = MakeConfineSafeSpawnOutLogBasename("outlog.txt");
    CHECK(a != b);
}
