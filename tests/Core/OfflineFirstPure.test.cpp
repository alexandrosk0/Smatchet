#include "OfflineFirstPure.h"
#include "DraftStorePure.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>

using namespace smatchet::offline;

TEST_SUITE("OfflineFirstPure") {

    TEST_CASE("IsOfflineState recognizes TransportDown") {
        CHECK(IsOfflineState(TrackerConnectivityState::TransportDown));
    }

    TEST_CASE("IsOfflineState recognizes ServiceUnavailable") {
        CHECK(IsOfflineState(TrackerConnectivityState::ServiceUnavailable));
    }

    TEST_CASE("IsOfflineState rejects Unknown") { CHECK_FALSE(IsOfflineState(TrackerConnectivityState::Unknown)); }

    TEST_CASE("IsOfflineState rejects AuthenticatedReachable") {
        CHECK_FALSE(IsOfflineState(TrackerConnectivityState::AuthenticatedReachable));
    }

    TEST_CASE("IsOfflineState rejects ReachableAuthOrConfigError") {
        CHECK_FALSE(IsOfflineState(TrackerConnectivityState::ReachableAuthOrConfigError));
    }

    TEST_CASE("ShouldAttemptNetwork blocks while offline") {
        const auto now = Clock::now();
        CHECK_FALSE(ShouldAttemptNetwork(TrackerConnectivityState::TransportDown, now, Clock::time_point()));
        CHECK_FALSE(ShouldAttemptNetwork(TrackerConnectivityState::ServiceUnavailable, now, Clock::time_point()));
    }

    TEST_CASE("ShouldAttemptNetwork blocks while backoff is active") {
        const auto now = Clock::now();
        const auto future = now + std::chrono::seconds(10);
        CHECK_FALSE(ShouldAttemptNetwork(TrackerConnectivityState::AuthenticatedReachable, now, future));
    }

    TEST_CASE("ShouldAttemptNetwork allows when online and backoff passed") {
        const auto now = Clock::now();
        const auto past = now - std::chrono::seconds(10);
        CHECK(ShouldAttemptNetwork(TrackerConnectivityState::AuthenticatedReachable, now, past));
    }

    TEST_CASE("ClassifyFreshness: no cache, no flight") {
        FreshnessInputs in;
        CHECK(ClassifyFreshness(in) == DataFreshness::UnavailableNoCache);
    }

    TEST_CASE("ClassifyFreshness: no cache, in flight") {
        FreshnessInputs in;
        in.InFlight = true;
        CHECK(ClassifyFreshness(in) == DataFreshness::LoadingNoCache);
    }

    TEST_CASE("ClassifyFreshness: has cache, live, no failure") {
        FreshnessInputs in;
        in.HasCache = true;
        in.Live = true;
        CHECK(ClassifyFreshness(in) == DataFreshness::Fresh);
    }

    TEST_CASE("ClassifyFreshness: live data from this session reads as offline once the tracker drops") {
        FreshnessInputs in;
        in.HasCache = true;
        in.Live = true;
        in.Connectivity = TrackerConnectivityState::AuthenticatedReachable;
        CHECK(ClassifyFreshness(in) == DataFreshness::Fresh);
        in.Connectivity = TrackerConnectivityState::TransportDown;
        CHECK(ClassifyFreshness(in) == DataFreshness::CachedOffline);
        in.Connectivity = TrackerConnectivityState::ServiceUnavailable;
        CHECK(ClassifyFreshness(in) == DataFreshness::CachedOffline);
    }

    TEST_CASE("ClassifyFreshness: has cache, in flight") {
        FreshnessInputs in;
        in.HasCache = true;
        in.InFlight = true;
        CHECK(ClassifyFreshness(in) == DataFreshness::Refreshing);
    }

    TEST_CASE("ClassifyFreshness: has cache, offline") {
        FreshnessInputs in;
        in.HasCache = true;
        in.Connectivity = TrackerConnectivityState::TransportDown;
        CHECK(ClassifyFreshness(in) == DataFreshness::CachedOffline);
    }

    TEST_CASE("ClassifyFreshness: has cache, stale") {
        FreshnessInputs in;
        in.HasCache = true;
        in.LastAttemptFailed = true;
        in.Connectivity = TrackerConnectivityState::AuthenticatedReachable;
        CHECK(ClassifyFreshness(in) == DataFreshness::CachedStale);
    }

    TEST_CASE("ShouldRenderContent: LoadingNoCache") {
        CHECK_FALSE(ShouldRenderContent(DataFreshness::LoadingNoCache));
    }

    TEST_CASE("ShouldRenderContent: UnavailableNoCache") {
        CHECK_FALSE(ShouldRenderContent(DataFreshness::UnavailableNoCache));
    }

    TEST_CASE("ShouldRenderContent: Fresh") { CHECK(ShouldRenderContent(DataFreshness::Fresh)); }

    TEST_CASE("ShouldRenderContent: Refreshing") { CHECK(ShouldRenderContent(DataFreshness::Refreshing)); }

    TEST_CASE("ShouldRenderContent: CachedOffline") { CHECK(ShouldRenderContent(DataFreshness::CachedOffline)); }

    TEST_CASE("ShouldRenderContent: CachedStale") { CHECK(ShouldRenderContent(DataFreshness::CachedStale)); }

    TEST_CASE("RouteWrite: read-only preference rejects") {
        CHECK(RouteWrite(TrackerConnectivityState::AuthenticatedReachable, true, true) == WriteRoute::Reject);
    }

    TEST_CASE("RouteWrite: offline and queueable queues") {
        CHECK(RouteWrite(TrackerConnectivityState::TransportDown, true, false) == WriteRoute::QueueImmediately);
    }

    TEST_CASE("RouteWrite: offline and not queueable networks") {
        CHECK(RouteWrite(TrackerConnectivityState::TransportDown, false, false) == WriteRoute::NetworkFirst);
    }

    TEST_CASE("RouteWrite: online goes network") {
        CHECK(RouteWrite(TrackerConnectivityState::AuthenticatedReachable, true, false) == WriteRoute::NetworkFirst);
    }

} // TEST_SUITE

// DraftMap (DraftStorePure.h) — what the user typed and has not sent, kept across a close, a restart and
// a crash (docs/plans/input-ux-w2-drafts-undo.md). Hosted here because a draft that survives an offline
// session is part of Quality Pillar 6, and this file runs on both the Windows suite and the Linux subset.
TEST_SUITE("DraftMap") {
    using smatchet::drafts::DraftEntry;
    using smatchet::drafts::DraftLimits;
    using smatchet::drafts::DraftMap;
    using smatchet::drafts::DraftWriteDue;
    using smatchet::drafts::PutResult;

    TEST_CASE("Put then Get restores the text, the base and the time") {
        DraftMap map;
        CHECK(map.Put("longtext", "PROJ-1|description", "my edit", "the saved text", 1234) == PutResult::Stored);
        DraftEntry out;
        REQUIRE(map.Get("longtext", "PROJ-1|description", out));
        CHECK(out.Text == "my edit");
        CHECK(out.Base == "the saved text");
        CHECK(out.UpdatedUnixMs == 1234);
        CHECK(map.Size() == 1);
    }

    TEST_CASE("Keys are isolated by surface and by scope") {
        DraftMap map;
        map.Put("comments", "site|PROJ-1", "comment on 1", "", 1);
        map.Put("comments", "site|PROJ-2", "comment on 2", "", 2);
        map.Put("worklog.description", "site|PROJ-1", "worklog on 1", "", 3);
        DraftEntry out;
        REQUIRE(map.Get("comments", "site|PROJ-1", out));
        CHECK(out.Text == "comment on 1");
        REQUIRE(map.Get("comments", "site|PROJ-2", out));
        CHECK(out.Text == "comment on 2");
        REQUIRE(map.Get("worklog.description", "site|PROJ-1", out));
        CHECK(out.Text == "worklog on 1");
        CHECK_FALSE(map.Get("comments", "other-site|PROJ-1", out));
        CHECK_FALSE(map.Get("comments", "", out));

        map.Erase("comments", "site|PROJ-1");
        CHECK_FALSE(map.Get("comments", "site|PROJ-1", out));
        CHECK(map.Get("comments", "site|PROJ-2", out));
        CHECK(map.Get("worklog.description", "site|PROJ-1", out));
    }

    TEST_CASE("Empty text erases; the same text again is Unchanged") {
        DraftMap map;
        CHECK(map.Put("assistant", "", "hello", "", 1) == PutResult::Stored);
        CHECK(map.Put("assistant", "", "hello", "", 2) == PutResult::Unchanged);
        CHECK(map.Put("assistant", "", "hello", "other base", 3) == PutResult::Stored);
        CHECK(map.Put("assistant", "", "", "", 4) == PutResult::Erased);
        DraftEntry out;
        CHECK_FALSE(map.Get("assistant", "", out));
        CHECK(map.Size() == 0);
        CHECK(map.TotalBytes() == 0);
    }

    TEST_CASE("EraseIfText keeps a draft that changed since, and EraseSurface clears one surface") {
        DraftMap map;
        map.Put("comments", "PROJ-1", "typed after the post", "", 1);
        CHECK_FALSE(map.EraseIfText("comments", "PROJ-1", "what was posted"));
        DraftEntry out;
        CHECK(map.Get("comments", "PROJ-1", out));
        CHECK(map.EraseIfText("comments", "PROJ-1", "typed after the post"));
        CHECK_FALSE(map.Get("comments", "PROJ-1", out));

        map.Put("offline.conflict", "1", "a", "", 1);
        map.Put("offline.conflict", "2", "b", "", 2);
        map.Put("comments", "PROJ-1", "c", "", 3);
        CHECK(map.EraseSurface("offline.conflict") == 2);
        CHECK(map.Size() == 1);
        CHECK(map.Get("comments", "PROJ-1", out));
    }

    TEST_CASE("An entry over the size bound is rejected and the older draft under its key goes too") {
        DraftLimits limits;
        limits.MaxEntryBytes = 64;
        DraftMap map(limits);
        CHECK(map.Put("comments", "PROJ-1", "short", "", 1) == PutResult::Stored);
        CHECK(map.Put("comments", "PROJ-1", std::string(100, 'x'), "", 2) == PutResult::Rejected);
        DraftEntry out;
        CHECK_FALSE(map.Get("comments", "PROJ-1", out));
        CHECK(map.TotalBytes() == 0);
        // The base counts toward the bound too.
        CHECK(map.Put("longtext", "PROJ-1", "edit", std::string(80, 'b'), 3) == PutResult::Rejected);
    }

    TEST_CASE("A key over its bound, or with no surface, is rejected") {
        DraftLimits limits;
        limits.MaxSurfaceBytes = 8;
        limits.MaxScopeBytes = 8;
        DraftMap map(limits);
        CHECK(map.Put("", "PROJ-1", "text", "", 1) == PutResult::Rejected);
        CHECK(map.Put("toolong-surface", "PROJ-1", "text", "", 1) == PutResult::Rejected);
        CHECK(map.Put("comments", "PROJ-123456", "text", "", 1) == PutResult::Rejected);
        CHECK(map.Size() == 0);
    }

    TEST_CASE("Over the count bound, the least recently updated other draft goes first") {
        DraftLimits limits;
        limits.MaxEntries = 3;
        DraftMap map(limits);
        map.Put("comments", "A", "a", "", 10);
        map.Put("comments", "B", "b", "", 5); // oldest
        map.Put("comments", "C", "c", "", 20);
        map.Put("comments", "D", "d", "", 1); // the new one stays even though its time is the smallest
        DraftEntry out;
        CHECK(map.Size() == 3);
        CHECK_FALSE(map.Get("comments", "B", out));
        CHECK(map.Get("comments", "A", out));
        CHECK(map.Get("comments", "C", out));
        CHECK(map.Get("comments", "D", out));
    }

    TEST_CASE("Over the total-bytes bound, old drafts are dropped until it holds") {
        DraftLimits limits;
        limits.MaxTotalBytes = 100;
        DraftMap map(limits);
        map.Put("s", "1", std::string(40, 'a'), "", 1);
        map.Put("s", "2", std::string(40, 'b'), "", 2);
        map.Put("s", "3", std::string(40, 'c'), "", 3);
        CHECK(map.TotalBytes() <= 100);
        DraftEntry out;
        CHECK_FALSE(map.Get("s", "1", out));
        CHECK(map.Get("s", "2", out));
        CHECK(map.Get("s", "3", out));
    }

    TEST_CASE("The file image round-trips, with the base and with non-ASCII text") {
        DraftMap map;
        map.Put("comments", "site|PROJ-1", "line one\nline \"two\" \xC3\xA9t\xC3\xA9", "", 100);
        map.Put("longtext", "PROJ-1|description", "edited", "saved", 200);
        DraftMap back;
        std::string err;
        REQUIRE(DraftMap::FromJson(map.ToJson(), DraftLimits(), back, err));
        CHECK(err.empty());
        CHECK(back.Size() == 2);
        DraftEntry out;
        REQUIRE(back.Get("comments", "site|PROJ-1", out));
        CHECK(out.Text == "line one\nline \"two\" \xC3\xA9t\xC3\xA9");
        CHECK(out.Base.empty());
        CHECK(out.UpdatedUnixMs == 100);
        REQUIRE(back.Get("longtext", "PROJ-1|description", out));
        CHECK(out.Base == "saved");
    }

    TEST_CASE("Invalid UTF-8 is written as a replacement character instead of throwing") {
        DraftMap map;
        map.Put("assistant", "", std::string("bad \xFF byte"), "", 1);
        std::string json;
        CHECK_NOTHROW(json = map.ToJson());
        DraftMap back;
        std::string err;
        REQUIRE(DraftMap::FromJson(json, DraftLimits(), back, err));
        DraftEntry out;
        REQUIRE(back.Get("assistant", "", out));
        CHECK(out.Text == "bad \xEF\xBF\xBD byte");
    }

    TEST_CASE("FromJson refuses what is not a version 1 drafts file") {
        DraftMap out;
        std::string err;
        CHECK_FALSE(DraftMap::FromJson("not json", DraftLimits(), out, err));
        CHECK_FALSE(err.empty());
        CHECK_FALSE(DraftMap::FromJson("[]", DraftLimits(), out, err));
        CHECK_FALSE(DraftMap::FromJson(R"({"version":2,"drafts":[]})", DraftLimits(), out, err));
        CHECK_FALSE(DraftMap::FromJson(R"({"version":"1","drafts":[]})", DraftLimits(), out, err));
        CHECK_FALSE(DraftMap::FromJson(R"({"version":1})", DraftLimits(), out, err));
        CHECK_FALSE(DraftMap::FromJson(std::string(100000, '[') + std::string(100000, ']'), DraftLimits(), out, err));
        CHECK(out.Size() == 0);
    }

    TEST_CASE("FromJson skips rows that are not well formed and applies the bounds to the rest") {
        DraftLimits limits;
        limits.MaxEntryBytes = 32;
        const std::string json = R"({"version":1,"drafts":[)"
                                 R"({"surface":"comments","scope":"A","text":"kept","updated":5},)"
                                 R"({"surface":"comments","scope":"B"},)"
                                 R"({"surface":7,"scope":"C","text":"x"},)"
                                 R"("not an object",)"
                                 R"({"surface":"comments","scope":"D","text":"far too long for the bound we set"},)"
                                 R"({"surface":"comments","scope":"E","text":"no time"}]})";
        DraftMap out;
        std::string err;
        REQUIRE(DraftMap::FromJson(json, limits, out, err));
        CHECK(out.Size() == 2);
        DraftEntry e;
        REQUIRE(out.Get("comments", "A", e));
        CHECK(e.UpdatedUnixMs == 5);
        REQUIRE(out.Get("comments", "E", e));
        CHECK(e.UpdatedUnixMs == 0);
        CHECK_FALSE(out.Get("comments", "D", e));
    }

    TEST_CASE("MergeUnder fills gaps from disk and keeps what was typed this session") {
        DraftMap session;
        session.Put("comments", "A", "typed now", "", 50);
        DraftMap disk;
        disk.Put("comments", "A", "from disk", "", 99);
        disk.Put("comments", "B", "only on disk", "", 10);
        session.MergeUnder(disk);
        DraftEntry out;
        REQUIRE(session.Get("comments", "A", out));
        CHECK(out.Text == "typed now");
        REQUIRE(session.Get("comments", "B", out));
        CHECK(out.Text == "only on disk");
    }

    TEST_CASE("DraftWriteDue waits for a pause, and for no longer than the cap while typing goes on") {
        using Clock = std::chrono::steady_clock;
        using std::chrono::milliseconds;
        const Clock::time_point t0 = Clock::now();
        CHECK_FALSE(DraftWriteDue(false, t0, t0, t0 + milliseconds(10000)));
        CHECK_FALSE(DraftWriteDue(true, t0, t0, t0 + milliseconds(100)));
        CHECK(DraftWriteDue(true, t0, t0, t0 + milliseconds(750)));
        // Still typing (last change just now), but the first unsaved change is 2 s old.
        CHECK_FALSE(DraftWriteDue(true, t0 + milliseconds(1900), t0, t0 + milliseconds(1950)));
        CHECK(DraftWriteDue(true, t0 + milliseconds(1990), t0, t0 + milliseconds(2000)));
    }
}
