// CiFailureClassifierPure — phase-5 of the `coderabbit-react-loop` plan.
// Exercises the pure helpers backing the (phase-6) concrete
// `CiFailureClassifier`. The doctest rig loads
// `tests/fixtures/check_runs_failed_sample.json` +
// `tests/fixtures/check_run_annotations_sample.json` and asserts each row's
// expected category + fingerprint kind against the helpers.
//
// No HTTP / SQLite / ImGui dependencies — the helpers are pure C++14 and
// the fixtures are flat JSON. Always-compiled (helpers compile on both
// AGENTIC=ON and AGENTIC=OFF — the dependency on the AGENTIC gate is the
// `PrCheckRunClassifier.h` interface, not the matchers).

#include "CiFailureClassifierPure.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef SMATCHET_TESTS_REPO_ROOT
#error "SMATCHET_TESTS_REPO_ROOT must be defined to locate tests/fixtures/*"
#endif

using ::smatchet::agentic::ci_pure::CheckRunCategory;
using ::smatchet::agentic::ci_pure::CmakeOrCtestKind;
using ::smatchet::agentic::ci_pure::ConcatenateAnnotations;
using ::smatchet::agentic::ci_pure::FingerprintCmakeOrCtest;
using ::smatchet::agentic::ci_pure::MapCheckRunNameToCategory;
using ::smatchet::agentic::ci_pure::MatchesCmakeConfigError;
using ::smatchet::agentic::ci_pure::MatchesCtestFailure;
using ::smatchet::agentic::ci_pure::MatchesLinkError;
using ::smatchet::agentic::ci_pure::MatchesSanitizerHit;
using ::smatchet::agentic::ci_pure::MatchesTransientFlake;

namespace {

const std::string kFixtureDir = std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures";

nlohmann::json LoadJson(const std::string& relPath) {
    const std::string path = kFixtureDir + "/" + relPath;
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.is_open(), ("Fixture file unreadable: " + path).c_str());
    std::stringstream ss;
    ss << in.rdbuf();
    return nlohmann::json::parse(ss.str());
}

CheckRunCategory CategoryFromName(const std::string& name) {
    if (name == "Unknown")
        return CheckRunCategory::Unknown;
    if (name == "CmakeOrCtest")
        return CheckRunCategory::CmakeOrCtest;
    if (name == "CoverageGate")
        return CheckRunCategory::CoverageGate;
    if (name == "CoverageAdvisory")
        return CheckRunCategory::CoverageAdvisory;
    return CheckRunCategory::Unknown;
}

CmakeOrCtestKind KindFromName(const std::string& name) {
    if (name == "Unknown")
        return CmakeOrCtestKind::Unknown;
    if (name == "CmakeConfigError")
        return CmakeOrCtestKind::CmakeConfigError;
    if (name == "LinkError")
        return CmakeOrCtestKind::LinkError;
    if (name == "CtestFailure")
        return CmakeOrCtestKind::CtestFailure;
    if (name == "SanitizerHit")
        return CmakeOrCtestKind::SanitizerHit;
    if (name == "TransientFlake")
        return CmakeOrCtestKind::TransientFlake;
    return CmakeOrCtestKind::Unknown;
}

} // namespace

TEST_SUITE("CiFailureClassifierPure") {

    // -------------------------------------------------------------------
    // Name table — one TEST_CASE per known check-run name, plus unknown.
    // -------------------------------------------------------------------

    TEST_CASE("MapCheckRunNameToCategory: 'Windows + MSYS2 UCRT64' -> CmakeOrCtest") {
        CHECK(MapCheckRunNameToCategory("Windows + MSYS2 UCRT64") == CheckRunCategory::CmakeOrCtest);
    }

    TEST_CASE("MapCheckRunNameToCategory: WHISPER=OFF variant -> CmakeOrCtest") {
        CHECK(MapCheckRunNameToCategory("Windows + MSYS2 UCRT64 (SMATCHET_WITH_WHISPER=OFF)") ==
              CheckRunCategory::CmakeOrCtest);
    }

    TEST_CASE("MapCheckRunNameToCategory: AGENTIC=OFF variant -> CmakeOrCtest") {
        CHECK(MapCheckRunNameToCategory("Windows + MSYS2 UCRT64 (SMATCHET_WITH_AGENTIC=OFF)") ==
              CheckRunCategory::CmakeOrCtest);
    }

    TEST_CASE("MapCheckRunNameToCategory: 'Test-delta gate' -> CoverageGate") {
        CHECK(MapCheckRunNameToCategory("Test-delta gate") == CheckRunCategory::CoverageGate);
    }

    TEST_CASE("MapCheckRunNameToCategory: 'Coverage (windows-2022 + OpenCppCoverage)' -> CoverageAdvisory") {
        CHECK(MapCheckRunNameToCategory("Coverage (windows-2022 + OpenCppCoverage)") ==
              CheckRunCategory::CoverageAdvisory);
    }

    TEST_CASE("MapCheckRunNameToCategory: unknown name -> Unknown") {
        CHECK(MapCheckRunNameToCategory("Future build matrix variant") == CheckRunCategory::Unknown);
        CHECK(MapCheckRunNameToCategory("CodeQL Analysis") == CheckRunCategory::Unknown);
    }

    TEST_CASE("MapCheckRunNameToCategory: empty name -> Unknown") {
        CHECK(MapCheckRunNameToCategory("") == CheckRunCategory::Unknown);
    }

    TEST_CASE("MapCheckRunNameToCategory: case-sensitive exact match") {
        // The table uses case-sensitive exact match; a lowercase variant
        // must NOT match.
        CHECK(MapCheckRunNameToCategory("windows + msys2 ucrt64") == CheckRunCategory::Unknown);
        CHECK(MapCheckRunNameToCategory("Windows + MSYS2 UCRT64 ") == CheckRunCategory::Unknown); // trailing space
    }

    // -------------------------------------------------------------------
    // Fingerprint matchers — happy + negative cases.
    // -------------------------------------------------------------------

    TEST_CASE("MatchesCmakeConfigError: positive cases") {
        CHECK(MatchesCmakeConfigError("CMake Error at CMakeLists.txt:142"));
        CHECK(MatchesCmakeConfigError("CMake Configure Step Failed."));
        CHECK(MatchesCmakeConfigError("target_link_libraries called with non-existent target"));
        CHECK(MatchesCmakeConfigError(R"(  FetchContent_Declare(cpr GIT_REPOSITORY ...))"));
        CHECK(MatchesCmakeConfigError("MissingPackage not found by find_package"));
    }

    TEST_CASE("MatchesCmakeConfigError: negative cases") {
        CHECK_FALSE(MatchesCmakeConfigError(""));
        CHECK_FALSE(MatchesCmakeConfigError("clean build, no errors"));
        CHECK_FALSE(MatchesCmakeConfigError("undefined reference to foo"));
    }

    TEST_CASE("MatchesLinkError: positive cases") {
        CHECK(MatchesLinkError("undefined reference to `bar()'"));
        CHECK(MatchesLinkError("multiple definition of `foo'"));
        CHECK(MatchesLinkError("ld: error: cannot find -lwhisper"));
        CHECK(MatchesLinkError("lld-link: error: undefined symbol"));
        CHECK(MatchesLinkError("LNK2019: unresolved external symbol"));
        CHECK(MatchesLinkError("LNK2001: unresolved external symbol"));
    }

    TEST_CASE("MatchesLinkError: negative cases") {
        CHECK_FALSE(MatchesLinkError(""));
        CHECK_FALSE(MatchesLinkError("CMake Error at CMakeLists.txt"));
        CHECK_FALSE(MatchesLinkError("warning: unused variable"));
    }

    TEST_CASE("MatchesCtestFailure: positive cases") {
        CHECK(MatchesCtestFailure("FAILED: smatchet_tests/CMakeFiles/smatchet_tests.dir/build.make"));
        CHECK(MatchesCtestFailure("The following tests FAILED:\n        7 - smatchet_tests (Failed)"));
        CHECK(MatchesCtestFailure("Errors while running CTest"));
        CHECK(MatchesCtestFailure("doctest::String mismatch in TEST_CASE"));
        CHECK(MatchesCtestFailure("FAILED:  smatchet/tests/smatchet_tests"));
    }

    TEST_CASE("MatchesCtestFailure: negative cases") {
        CHECK_FALSE(MatchesCtestFailure(""));
        CHECK_FALSE(MatchesCtestFailure("100% tests passed"));
        CHECK_FALSE(MatchesCtestFailure("all green"));
    }

    TEST_CASE("MatchesSanitizerHit: positive cases") {
        CHECK(MatchesSanitizerHit("==12345==ERROR: AddressSanitizer: heap-use-after-free"));
        CHECK(MatchesSanitizerHit("UndefinedBehaviorSanitizer: SEGV on unknown address"));
        CHECK(MatchesSanitizerHit("ThreadSanitizer: data race on 0x7ffe"));
        CHECK(MatchesSanitizerHit("MemorySanitizer: use-of-uninitialized-value"));
        CHECK(MatchesSanitizerHit("LeakSanitizer: detected memory leaks"));
        CHECK(MatchesSanitizerHit("==9999==ERROR: something happened"));
    }

    TEST_CASE("MatchesSanitizerHit: negative cases") {
        CHECK_FALSE(MatchesSanitizerHit(""));
        CHECK_FALSE(MatchesSanitizerHit("AddressSanitizer not enabled in this build"));
        CHECK_FALSE(MatchesSanitizerHit("FAILED: smatchet_tests"));
    }

    TEST_CASE("MatchesTransientFlake: positive cases") {
        CHECK(MatchesTransientFlake("Operation timed out after 60001 milliseconds"));
        CHECK(MatchesTransientFlake("Could not resolve host: mirror.msys2.org"));
        CHECK(MatchesTransientFlake("could not connect to host github.com:443"));
        CHECK(MatchesTransientFlake("Temporary failure resolving 'mirror.msys2.org'"));
        CHECK(MatchesTransientFlake("curl: (28) Operation timed out"));
        CHECK(MatchesTransientFlake("FetchContent: download failed for cpr; will retry"));
    }

    TEST_CASE("MatchesTransientFlake: paired MSYS2 mirror + connection reset") {
        // Both substrings must appear for the pair to match.
        CHECK(MatchesTransientFlake("downloading from http://mirror.msys2.org/foo: connection reset by peer"));
        // Only mirror.msys2.org alone → not a transient.
        CHECK_FALSE(MatchesTransientFlake("downloading from http://mirror.msys2.org/foo: success"));
        // Only connection-reset alone → not a transient (that's an
        // ambiguous failure that could be a real bug; defer to other
        // fingerprints).
        CHECK_FALSE(MatchesTransientFlake("HTTP connection reset by peer on github.com"));
    }

    TEST_CASE("MatchesTransientFlake: negative cases") {
        CHECK_FALSE(MatchesTransientFlake(""));
        CHECK_FALSE(MatchesTransientFlake("All good, no flakes here"));
        CHECK_FALSE(MatchesTransientFlake("CMake Error at CMakeLists.txt"));
    }

    // -------------------------------------------------------------------
    // FingerprintCmakeOrCtest — priority order.
    // -------------------------------------------------------------------

    TEST_CASE("FingerprintCmakeOrCtest: transient beats link error") {
        // A flake masquerading as a link error: cpr download failed for
        // the dep so the eventual link sees undefined references. Must
        // classify as TransientFlake, not LinkError.
        std::string ann = "undefined reference to `cpr::Session::Get()'\n"
                          "curl: (28) Operation timed out after 60001 milliseconds";
        CHECK(FingerprintCmakeOrCtest(ann, "") == CmakeOrCtestKind::TransientFlake);
    }

    TEST_CASE("FingerprintCmakeOrCtest: transient beats cmake error") {
        std::string ann = "CMake Error at FetchContent download step\n"
                          "FetchContent: download failed for cpr; will retry";
        CHECK(FingerprintCmakeOrCtest(ann, "") == CmakeOrCtestKind::TransientFlake);
    }

    TEST_CASE("FingerprintCmakeOrCtest: sanitizer beats ctest failure") {
        // A sanitizer hit surfaces via FAILED: too; sanitizer beats
        // generic ctest failure so the dispatch routes to
        // debug-detective rather than test-rig.
        std::string ann = "FAILED: smatchet_tests\n"
                          "==12345==ERROR: AddressSanitizer: heap-use-after-free";
        CHECK(FingerprintCmakeOrCtest(ann, "") == CmakeOrCtestKind::SanitizerHit);
    }

    TEST_CASE("FingerprintCmakeOrCtest: ctest beats link error in priority") {
        std::string ann = "undefined reference to `foo'\n"
                          "FAILED: smatchet_tests";
        CHECK(FingerprintCmakeOrCtest(ann, "") == CmakeOrCtestKind::CtestFailure);
    }

    TEST_CASE("FingerprintCmakeOrCtest: link beats cmake config in priority") {
        std::string ann = "CMake Error at CMakeLists.txt:142\n"
                          "lld-link: error: undefined reference to foo";
        CHECK(FingerprintCmakeOrCtest(ann, "") == CmakeOrCtestKind::LinkError);
    }

    TEST_CASE("FingerprintCmakeOrCtest: lone cmake-config matches") {
        CHECK(FingerprintCmakeOrCtest("CMake Error at CMakeLists.txt:42", "") == CmakeOrCtestKind::CmakeConfigError);
    }

    TEST_CASE("FingerprintCmakeOrCtest: no fingerprint -> Unknown") {
        CHECK(FingerprintCmakeOrCtest("", "") == CmakeOrCtestKind::Unknown);
        CHECK(FingerprintCmakeOrCtest("clean run, no errors", "100% tests passed") == CmakeOrCtestKind::Unknown);
    }

    TEST_CASE("FingerprintCmakeOrCtest: fingerprint can come from logTail alone") {
        // Empty annotations, but log tail carries the evidence.
        CHECK(FingerprintCmakeOrCtest("", "==12345==ERROR: AddressSanitizer: heap-use-after-free") ==
              CmakeOrCtestKind::SanitizerHit);
    }

    TEST_CASE("FingerprintCmakeOrCtest: transient in logTail beats real failure in annotations") {
        // Same priority rule across both inputs — transient wins
        // regardless of which input it came from.
        CHECK(FingerprintCmakeOrCtest("undefined reference to foo", "curl: (28) Operation timed out") ==
              CmakeOrCtestKind::TransientFlake);
    }

    // -------------------------------------------------------------------
    // ConcatenateAnnotations — cap behaviour.
    // -------------------------------------------------------------------

    TEST_CASE("ConcatenateAnnotations: empty input -> empty output") {
        CHECK(ConcatenateAnnotations({}).empty());
    }

    TEST_CASE("ConcatenateAnnotations: single message passes through") {
        const std::vector<std::string> in = {"hello"};
        CHECK(ConcatenateAnnotations(in) == "hello");
    }

    TEST_CASE("ConcatenateAnnotations: multiple messages joined with newlines") {
        const std::vector<std::string> in = {"a", "b", "c"};
        CHECK(ConcatenateAnnotations(in) == "a\nb\nc");
    }

    TEST_CASE("ConcatenateAnnotations: cap truncates from the start") {
        // 100 messages of "1234567890" each = 10 bytes + newline = 11 bytes.
        // Total ~1099 bytes. Cap at 200 → output ends with the tail
        // content (last ~18 messages) so the actual error survives.
        std::vector<std::string> msgs;
        for (int i = 0; i < 100; ++i) {
            msgs.push_back("1234567890");
        }
        const std::size_t cap = 200;
        const std::string out = ConcatenateAnnotations(msgs, cap);
        REQUIRE(out.size() == cap);
        // The very last bytes of the concatenated whole are the literal
        // "1234567890" tail of the last message (no trailing newline);
        // therefore the capped output must also end with that suffix.
        const std::string tail = "1234567890";
        REQUIRE(out.size() >= tail.size());
        CHECK(out.substr(out.size() - tail.size()) == tail);
    }

    TEST_CASE("ConcatenateAnnotations: cap larger than data is a no-op") {
        const std::vector<std::string> in = {"short", "stuff"};
        const std::string out = ConcatenateAnnotations(in, 64 * 1024);
        CHECK(out == "short\nstuff");
    }

    // -------------------------------------------------------------------
    // Fixture round-trip — each entry's name + annotations + log_tail
    // → expected category + expected kind.
    // -------------------------------------------------------------------

    TEST_CASE("Fixture round-trip: check-run names map to expected categories") {
        const nlohmann::json runs = LoadJson("check_runs_failed_sample.json");
        REQUIRE(runs.is_array());
        REQUIRE(runs.size() >= 8);

        for (const auto& row : runs) {
            const std::int64_t id = row.at("id").get<std::int64_t>();
            const std::string name = row.at("name").get<std::string>();
            const std::string expectedCatName = row.at("expected_category").get<std::string>();
            const CheckRunCategory expected = CategoryFromName(expectedCatName);
            const CheckRunCategory actual = MapCheckRunNameToCategory(name);
            const std::string msg = "id=" + std::to_string(id) + " name=" + name + " expected category " +
                                    expectedCatName;
            CHECK_MESSAGE(actual == expected, msg.c_str());
        }
    }

    TEST_CASE("Fixture round-trip: CmakeOrCtest entries fingerprint to expected kind") {
        const nlohmann::json runs = LoadJson("check_runs_failed_sample.json");
        const nlohmann::json annos = LoadJson("check_run_annotations_sample.json");
        REQUIRE(annos.is_object());

        bool sawCmakeOrCtestRow = false;
        for (const auto& row : runs) {
            const std::int64_t id = row.at("id").get<std::int64_t>();
            const std::string name = row.at("name").get<std::string>();
            if (MapCheckRunNameToCategory(name) != CheckRunCategory::CmakeOrCtest) {
                continue;
            }
            sawCmakeOrCtestRow = true;

            const std::string idKey = std::to_string(id);
            REQUIRE_MESSAGE(annos.contains(idKey), ("annotations fixture missing key " + idKey).c_str());
            const auto& entry = annos.at(idKey);
            const auto& annArr = entry.at("annotations");
            std::vector<std::string> annotationMessages;
            for (const auto& a : annArr) {
                annotationMessages.push_back(a.get<std::string>());
            }
            const std::string annConcat = ConcatenateAnnotations(annotationMessages);
            const std::string logTail = entry.at("log_tail").get<std::string>();

            const std::string expectedKindName = row.at("expected_kind").get<std::string>();
            const CmakeOrCtestKind expected = KindFromName(expectedKindName);
            const CmakeOrCtestKind actual = FingerprintCmakeOrCtest(annConcat, logTail);

            const std::string msg = "id=" + idKey + " name=" + name + " expected kind " + expectedKindName;
            CHECK_MESSAGE(actual == expected, msg.c_str());
        }
        CHECK_MESSAGE(sawCmakeOrCtestRow, "fixture must include at least one CmakeOrCtest row");
    }

    TEST_CASE("Fixture round-trip: non-CmakeOrCtest rows do not depend on fingerprints") {
        // CoverageGate / CoverageAdvisory / Unknown rows: their dispatch
        // route is determined by category alone; assert the row's
        // expected_kind is `Unknown` (the helpers don't speak coverage).
        const nlohmann::json runs = LoadJson("check_runs_failed_sample.json");
        for (const auto& row : runs) {
            const std::string name = row.at("name").get<std::string>();
            const CheckRunCategory cat = MapCheckRunNameToCategory(name);
            if (cat == CheckRunCategory::CmakeOrCtest) {
                continue;
            }
            const std::string expectedKindName = row.at("expected_kind").get<std::string>();
            CHECK_MESSAGE(expectedKindName == "Unknown",
                          ("row name=" + name + " expected_kind must be 'Unknown' for non-CmakeOrCtest").c_str());
        }
    }

} // TEST_SUITE
