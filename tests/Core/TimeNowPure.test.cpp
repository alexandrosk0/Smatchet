#include <doctest/doctest.h>

#include "Commands/Scenarios/UiPerfRowsJson.h"
#include "JiraIssueMappingPure.h"
#include "TimeNowPure.h"
#include "TrackerFieldPayloadPure.h"
#include "TrackerFieldValueUtils.h"

#include <string>
#include <vector>

TEST_CASE("TimeNowPure: NowUnixMs and NowUnixSeconds agree on the same wall clock") {
    const std::int64_t ms = TimeNowPure::NowUnixMs();
    const std::int64_t sec = TimeNowPure::NowUnixSeconds();
    // Both are unix-epoch and taken back to back: the seconds reading sits
    // within one second of the milliseconds reading. Also pin the epoch base —
    // a steady_clock regression would report a tiny since-boot value instead.
    CHECK(ms > 1000000000000LL); // past 2001 in ms — unix epoch, not since-boot
    CHECK(sec > 1000000000LL);
    CHECK(sec >= ms / 1000 - 1);
    CHECK(sec <= ms / 1000 + 1);
}

TEST_CASE("IsTimeDurationField delegates to the one Jira duration-id list") {
    const char* const durationIds[] = {
        "timeoriginalestimate",          "timeestimate",          "timespent",
        "aggregatetimeoriginalestimate", "aggregatetimeestimate", "aggregatetimespent",
    };
    for (const char* id : durationIds) {
        CAPTURE(id);
        CHECK(smatchet::jira::IsJiraDurationSecondsFieldKey(id));
        CHECK(TrackerFieldValueUtils::IsTimeDurationField(id));
    }
    const char* const nonDurationIds[] = {"timetracking", "aggregatetimetracking", "summary", "", "timespent2"};
    for (const char* id : nonDurationIds) {
        CAPTURE(id);
        CHECK_FALSE(smatchet::jira::IsJiraDurationSecondsFieldKey(id));
        CHECK_FALSE(TrackerFieldValueUtils::IsTimeDurationField(id));
    }
}

TEST_CASE("TrackerFieldPayloadPure::IsSprintField: family or gh-sprint schema") {
    TrackerField f;
    CHECK_FALSE(TrackerFieldPayloadPure::IsSprintField(f));

    f.Family = TrackerFieldFamily::Sprint;
    CHECK(TrackerFieldPayloadPure::IsSprintField(f));

    f.Family = TrackerFieldFamily::Unknown;
    f.SchemaCustom = "com.pyxis.greenhopper.jira:gh-sprint";
    CHECK(TrackerFieldPayloadPure::IsSprintField(f));

    f.SchemaCustom = "something-else";
    CHECK_FALSE(TrackerFieldPayloadPure::IsSprintField(f));
}

TEST_CASE("UiPerfRowsToJson: field set and values match the perf-baseline contract") {
    UiPerfRow a;
    a.name = "pane.render";
    a.lastTotalMs = 1.5;
    a.avgPerCallMs = 0.5;
    a.maxMs = 2.25;
    a.calls = 3;
    a.emaAvgMs = 1.25;
    a.p99Ms = 2.0;
    UiPerfRow b;
    b.name = "drawActiveProjectWindow";

    const nlohmann::json rowsJson = UiPerfRowsToJson({a, b});
    REQUIRE(rowsJson.is_array());
    REQUIRE(rowsJson.size() == 2);

    const nlohmann::json& ja = rowsJson[0];
    CHECK(ja.at("name") == "pane.render");
    CHECK(ja.at("lastTotalMs") == 1.5);
    CHECK(ja.at("avgPerCallMs") == 0.5);
    CHECK(ja.at("maxMs") == 2.25);
    CHECK(ja.at("calls") == 3);
    CHECK(ja.at("emaAvgMs") == 1.25);
    CHECK(ja.at("p99Ms") == 2.0);
    // Exactly the seven contract fields — an added or renamed field changes
    // what perf-baseline.sh consumes and must show up here.
    CHECK(ja.size() == 7);
    CHECK(rowsJson[1].at("name") == "drawActiveProjectWindow");
    CHECK(rowsJson[1].at("lastTotalMs") == 0.0);

    CHECK(UiPerfRowsToJson({}).is_array());
    CHECK(UiPerfRowsToJson({}).empty());
}
