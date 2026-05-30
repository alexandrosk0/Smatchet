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

TEST_CASE("scenario capture sizing clamps tiny or malformed dimensions") {
    json args;
    args["windowWidth"] = 12;
    args["windowHeight"] = "not-a-number";

    const smatchet::cmd::ScenarioCaptureSize size = smatchet::cmd::ParseScenarioCaptureSize(args);

    CHECK(size.Width == 320);
    CHECK(size.Height == 1009);
}
