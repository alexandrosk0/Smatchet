#include <doctest/doctest.h>

#include "MergeWatchNotifyPure.h"

#include <string>

using MergeWatchNotifyPure::NotifyPlan;
using MergeWatchNotifyPure::PlanMergeWatchNotify;

// The request body arrives over the localhost POST endpoint — attacker-controlled bytes
// (SECURITY_AUDIT.md flagged this listener). These cases pin the full decision surface:
// bounded parse, shape/type validation, the state allow-list, sanitization/truncation, and
// the exact response bodies the daemon's smatchet-notify.sh scripts match on.

TEST_CASE("valid payloads — 200, toast plan, state → toast-type mapping") {
    const NotifyPlan plan = PlanMergeWatchNotify(R"({"pr":123,"state":"CI_FAIL","message":"3 checks red"})");
    REQUIRE(plan.Ok);
    CHECK(plan.HttpStatus == 200);
    CHECK(plan.ResponseBody == "{\"ok\":true}");
    CHECK(plan.Title == "Smatchet watcher \xC2\xB7 PR #123 \xC2\xB7 CI_FAIL");
    CHECK(plan.Message == "3 checks red");
    CHECK(plan.Type == ToastType::Error);

    CHECK(PlanMergeWatchNotify(R"({"pr":1,"state":"PR_CLOSED_OR_MERGED","message":"m"})").Type == ToastType::Success);
    CHECK(PlanMergeWatchNotify(R"({"pr":1,"state":"GH_API_DOWN","message":"m"})").Type == ToastType::Warning);
    CHECK(PlanMergeWatchNotify(R"({"pr":1,"state":"TIMEOUT","message":"m"})").Type == ToastType::Warning);
    CHECK(PlanMergeWatchNotify(R"({"pr":1,"state":"PAGINATION_OVERFLOW","message":"m"})").Type == ToastType::Error);
    CHECK(PlanMergeWatchNotify(R"({"pr":1,"state":"TRIAGE_BUDGET_EXHAUSTED","message":"m"})").Type == ToastType::Error);
}

TEST_CASE("bad JSON — bounded parse rejects, including the nesting bomb") {
    {
        const NotifyPlan plan = PlanMergeWatchNotify("not json {{{");
        CHECK_FALSE(plan.Ok);
        CHECK(plan.HttpStatus == 400);
        CHECK(plan.ResponseBody.find("{\"error\":\"bad JSON: ") == 0);
    }
    {
        // Depth bomb: a bare recursive parse would exhaust the native stack; ParseBounded
        // must reject it as a clean 400 (the audit class this listener was flagged for).
        std::string bomb;
        for (int i = 0; i < 5000; ++i) {
            bomb += '[';
        }
        bomb += '1';
        for (int i = 0; i < 5000; ++i) {
            bomb += ']';
        }
        const NotifyPlan plan = PlanMergeWatchNotify(bomb);
        CHECK_FALSE(plan.Ok);
        CHECK(plan.HttpStatus == 400);
    }
}

TEST_CASE("shape validation — non-object / missing fields / wrong types") {
    for (const char* body : {"[1,2,3]", "42", R"({"pr":1,"state":"CI_FAIL"})", R"({"state":"CI_FAIL","message":"m"})",
                             R"({"pr":1,"message":"m"})"}) {
        const NotifyPlan plan = PlanMergeWatchNotify(body);
        CHECK_FALSE(plan.Ok);
        CHECK(plan.HttpStatus == 400);
        CHECK(plan.ResponseBody == "{\"error\":\"missing required field (pr / state / message)\"}");
    }
    for (const char* body :
         {R"({"pr":"123","state":"CI_FAIL","message":"m"})", R"({"pr":1.5,"state":"CI_FAIL","message":"m"})",
          R"({"pr":1,"state":7,"message":"m"})", R"({"pr":1,"state":"CI_FAIL","message":[]})"}) {
        const NotifyPlan plan = PlanMergeWatchNotify(body);
        CHECK_FALSE(plan.Ok);
        CHECK(plan.ResponseBody == "{\"error\":\"field type mismatch\"}");
    }
}

TEST_CASE("state allow-list — anything off-list is rejected with the stable error body") {
    const NotifyPlan plan = PlanMergeWatchNotify(R"({"pr":1,"state":"EVIL_STATE","message":"m"})");
    CHECK_FALSE(plan.Ok);
    CHECK(plan.HttpStatus == 400);
    CHECK(plan.ResponseBody.find("unknown state") != std::string::npos);
    // Case-sensitive: the daemon sends exact upper-case states; lower-case is off-list.
    CHECK_FALSE(PlanMergeWatchNotify(R"({"pr":1,"state":"ci_fail","message":"m"})").Ok);
}

TEST_CASE("sanitization — control chars neutralised, whitespace kept, 500-byte cap") {
    {
        const NotifyPlan plan =
            PlanMergeWatchNotify("{\"pr\":1,\"state\":\"TIMEOUT\",\"message\":\"a\\u0007b\\tc\\nd\"}");
        REQUIRE(plan.Ok);
        CHECK(plan.Message == "a?b\tc\nd"); // BEL → '?', tab/newline preserved
    }
    {
        const std::string longMsg(600, 'x');
        const NotifyPlan plan =
            PlanMergeWatchNotify(std::string(R"({"pr":1,"state":"TIMEOUT","message":")") + longMsg + "\"}");
        REQUIRE(plan.Ok);
        CHECK(plan.Message.size() == 503); // 500 + "..."
        CHECK(plan.Message.compare(500, 3, "...") == 0);
    }
}
