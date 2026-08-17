#include <doctest/doctest.h>

#include "Commands/Scenarios/ScenarioCaptureSizing.h"

#include <nlohmann/json.hpp>

using nlohmann::json;

TEST_CASE("scenario capture sizing defaults to bucket-C golden dimensions") {
    const smatchet::cmd::ScenarioCaptureSize size = smatchet::cmd::ParseScenarioCaptureSize(json::object());

    CHECK(size.Width == 1920);
    CHECK(size.Height == 1009);
}

TEST_CASE("scenario capture sizing accepts CLI string dimensions") {
    json args;
    args["windowWidth"] = "1600";
    args["windowHeight"] = "900";

    const smatchet::cmd::ScenarioCaptureSize size = smatchet::cmd::ParseScenarioCaptureSize(args);

    CHECK(size.Width == 1600);
    CHECK(size.Height == 900);
}

// The resize a screenshot scenario stages is only a REQUEST the window system may clamp. On the
// bucket-C Windows runner a 1920x1009 request came back as a 1028x749 framebuffer (Win32
// MaxTrackSize caps a top-level window near the virtual desktop size), the scenario still
// reported ok, and the golden diff could only say `dimension mismatch` — which the lane's
// sanctioned mask swallowed, leaving bucket-C green while it compared nothing (#2092). A capture
// at any size other than the one requested is an unusable golden-diff input, never a near-miss.
TEST_CASE("VerifyScenarioCaptureSize rejects every capture that is not exactly the requested size") {
    smatchet::cmd::ScenarioCaptureSize requested;
    requested.Width = 1920;
    requested.Height = 1009;

    SUBCASE("exact match passes with no message") {
        const auto verdict = smatchet::cmd::VerifyScenarioCaptureSize(requested, 1920, 1009);
        CHECK(verdict.Ok);
        CHECK(verdict.Message.empty());
    }
    SUBCASE("the observed runner clamp is rejected and names both sizes") {
        const auto verdict = smatchet::cmd::VerifyScenarioCaptureSize(requested, 1028, 749);
        REQUIRE_FALSE(verdict.Ok);
        CHECK(verdict.Message.find("1028x749") != std::string::npos);
        CHECK(verdict.Message.find("1920x1009") != std::string::npos);
    }
    SUBCASE("an off-by-one in either axis is still a reject — no tolerance band") {
        CHECK_FALSE(smatchet::cmd::VerifyScenarioCaptureSize(requested, 1919, 1009).Ok);
        CHECK_FALSE(smatchet::cmd::VerifyScenarioCaptureSize(requested, 1920, 1008).Ok);
        CHECK_FALSE(smatchet::cmd::VerifyScenarioCaptureSize(requested, 1921, 1010).Ok);
    }
    SUBCASE("a transposed capture is a reject, not a match on area") {
        CHECK_FALSE(smatchet::cmd::VerifyScenarioCaptureSize(requested, 1009, 1920).Ok);
    }
    SUBCASE("no capture recorded is not a pass") {
        for (const int bad : {0, -1}) {
            CHECK_FALSE(smatchet::cmd::VerifyScenarioCaptureSize(requested, bad, 1009).Ok);
            CHECK_FALSE(smatchet::cmd::VerifyScenarioCaptureSize(requested, 1920, bad).Ok);
        }
        // The never-captured verdict must still say what was expected, so a silent
        // no-capture is as attributable as a wrong-sized one.
        const auto verdict = smatchet::cmd::VerifyScenarioCaptureSize(requested, -1, -1);
        CHECK(verdict.Message.find("1920x1009") != std::string::npos);
    }
    SUBCASE("the narrow user-info geometry is judged on its own requested size") {
        smatchet::cmd::ScenarioCaptureSize narrow;
        narrow.Width = 480;
        narrow.Height = 1009;
        CHECK(smatchet::cmd::VerifyScenarioCaptureSize(narrow, 480, 1009).Ok);
        // Width survived the clamp, height did not — the exact bucket-C narrow-lane shape.
        CHECK_FALSE(smatchet::cmd::VerifyScenarioCaptureSize(narrow, 480, 749).Ok);
    }
}

TEST_CASE("scenario capture sizing clamps tiny or malformed dimensions") {
    json args;
    args["windowWidth"] = 12;
    args["windowHeight"] = "not-a-number";

    const smatchet::cmd::ScenarioCaptureSize size = smatchet::cmd::ParseScenarioCaptureSize(args);

    CHECK(size.Width == 320);
    CHECK(size.Height == 1009);
}
