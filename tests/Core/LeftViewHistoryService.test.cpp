// LeftViewHistoryService against an in-memory cache and a scripted reader: departures are recorded once, survive a
// reload of the service, get a reason, and never come from a changed query. Tasks run inline, so every case is
// deterministic. See docs/plans/left-view-history-grid.md.

#include "../support/SqliteMemFixture.h"

#include "LeftViewHistoryService.h"
#include "LocalCacheManager.h"
#include "Sync/LeftViewReasonPure.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace smatchet;
using smatchet_tests::SqliteMemFixture;

namespace {

// Only the calls the service makes are scripted; the rest are unreachable here.
class ScriptedReader : public ITrackerIssueReader {
  public:
    std::vector<TrackerFieldChange> history;
    std::vector<CachedTicket> rows;
    bool rowsFail = false;
    bool existsAnswer = true;
    int historyCalls = 0;
    std::string historyRefusesKey; ///< A batch naming this key is refused whole, as Jira does for a hidden issue.

    std::vector<CachedTicket> FetchIssues(bool*, const TrackerConfig*, const ViewsStore*, std::string*, std::string*,
                                          TrackerError*) override {
        return {};
    }
    Result<std::vector<CachedTicket>, TrackerError>
    FetchIssuesForKeys(const TrackerConfig&, const std::vector<std::string>&, const ViewsStore&) override {
        if (rowsFail) {
            return Result<std::vector<CachedTicket>, TrackerError>::Err(TrackerErrorUnknown("down"));
        }
        return Result<std::vector<CachedTicket>, TrackerError>::Ok(rows);
    }
    Result<std::vector<TrackerFieldChange>, TrackerError>
    FetchIssueFieldChanges(const TrackerConfig&, const std::vector<std::string>& keys, const CancelCallback&) override {
        ++historyCalls;
        for (const std::string& key : keys) {
            if (!historyRefusesKey.empty() && key == historyRefusesKey) {
                return Result<std::vector<TrackerFieldChange>, TrackerError>::Err(
                    TrackerErrorInvalidRequest("key in (...) names an issue that cannot be read", 400));
            }
        }
        std::vector<TrackerFieldChange> answered;
        for (const TrackerFieldChange& change : history) {
            if (std::find(keys.begin(), keys.end(), change.issueKey) != keys.end()) {
                answered.push_back(change);
            }
        }
        return Result<std::vector<TrackerFieldChange>, TrackerError>::Ok(answered);
    }
    Result<bool, TrackerError> ProbeIssueExists(const TrackerConfig&, const std::string&) override {
        return Result<bool, TrackerError>::Ok(existsAnswer);
    }
    std::string ResolveDisplayValue(const std::string&, const TrackerField*, const std::string& value) const override {
        return value;
    }
    std::string BuildBrowseUrl(const TrackerConfig&, const std::string& key) const override {
        return "https://tracker.test/browse/" + key;
    }
};

struct Rig {
    SqliteMemFixture fx;
    std::shared_ptr<LocalCacheManager> cache;
    std::shared_ptr<ScriptedReader> reader = std::make_shared<ScriptedReader>();
    std::unique_ptr<LeftViewHistoryService> service;
    std::int64_t now = 1000;
    bool offline = false;
    int reloadNotifications = 0;

    Rig() {
        cache = std::shared_ptr<LocalCacheManager>(&fx.Ref(), [](LocalCacheManager*) {});
        service = MakeService();
    }

    std::unique_ptr<LeftViewHistoryService> MakeService() {
        LeftViewHistoryService::Deps deps;
        deps.GetCache = [this]() { return cache; };
        deps.Launch = [](std::function<void()> task) { task(); };
        deps.PostToUi = [](std::function<void()> task) { task(); };
        deps.ShouldCancel = []() { return false; };
        deps.IsOffline = [this]() { return offline; };
        deps.MaxRows = []() { return 3; };
        deps.MaxReasonKeys = []() { return 2; };
        deps.NowUnixSec = [this]() { return now; };
        deps.ResolveReader = [this](const std::string&, const TrackerConfig&,
                                    std::shared_ptr<ITrackerIssueReader>& out) {
            out = reader;
            return true;
        };
        deps.ResolveFieldLabels = [](const std::string&) { return std::unordered_map<std::string, std::string>(); };
        deps.OnReloadDepartures = [this](const std::vector<LeftViewRecord>& rows) {
            reloadNotifications += static_cast<int>(rows.size());
        };
        deps.LoadViews = [](const TrackerConfig&) { return ViewsStore(); };
        return std::unique_ptr<LeftViewHistoryService>(new LeftViewHistoryService(std::move(deps)));
    }

    LeftViewCapture Capture(const std::string& query = "status != Done") {
        LeftViewCapture capture;
        capture.backendKey = "Jira@x";
        capture.paneId = "main";
        capture.viewId = "v1";
        capture.viewName = "My view";
        capture.query = query;
        capture.reader = reader;
        return capture;
    }
};

LeftViewDeparture Row(const std::string& key, const std::string& status) {
    LeftViewDeparture departure;
    departure.issueKey = key;
    departure.fieldValues["summary"] = key + " summary";
    departure.fieldValues["status"] = status;
    return departure;
}

CachedTicket Fresh(const std::string& key, const std::string& status) {
    CachedTicket ticket;
    ticket.id = key;
    ticket.fieldValues["status"] = status;
    return ticket;
}

} // namespace

TEST_SUITE("LeftViewHistoryService") {

    TEST_CASE("the first listing is a baseline; the next records what left, with its reason") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B"}, {}, {});
        CHECK(rig.service->Rows().empty());

        rig.reader->rows.push_back(Fresh("B", "Done"));
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {Row("B", "Open")});
        REQUIRE(rig.service->Rows().size() == 1);
        const LeftViewRecord& row = rig.service->Rows()[0];
        CHECK(row.issueKey == "B");
        CHECK(row.summary == "B summary");
        CHECK(row.viewName == "My view");
        CHECK(row.reasonKind == LeftViewReasonKind::QueryFieldChanged);
        CHECK(row.reason == "Status: Open -> Done");
        CHECK(row.browseUrl == "https://tracker.test/browse/B");
        CHECK(rig.reloadNotifications == 1);
    }

    TEST_CASE("a changed query is a new baseline, not a mass departure") {
        Rig rig;
        rig.service->RecordListing(rig.Capture("old"), {"A", "B"}, {}, {});
        rig.service->RecordListing(rig.Capture("new"), {"C"}, {}, {});
        CHECK(rig.service->Rows().empty());
    }

    TEST_CASE("history wins over the row diff and names the author") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B"}, {}, {});
        TrackerFieldChange change;
        change.issueKey = "B";
        change.fieldId = "status";
        change.fieldLabel = "status";
        change.fromValue = "Open";
        change.toValue = "Done";
        change.author = "Alice";
        rig.reader->history.push_back(change);
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {Row("B", "Open")});
        REQUIRE(rig.service->Rows().size() == 1);
        CHECK(rig.service->Rows()[0].author == "Alice");
    }

    TEST_CASE("monitor and reload report one departure once") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B"}, {}, {});
        LeftViewDeparture departure = Row("B", "Open");
        rig.service->RecordMonitorDepartures(rig.Capture(), {departure});
        CHECK(rig.service->Rows().size() == 1);
        rig.service->RecordMonitorDepartures(rig.Capture(), {departure}); // a second pane on the same view
        CHECK(rig.service->Rows().size() == 1);
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {departure}); // the next reload
        CHECK(rig.service->Rows().size() == 1);
    }

    TEST_CASE("a row the monitor added is recorded when it leaves before the next reload") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {});
        rig.service->RecordListingAdditions("Jira@x", "v1", "status != Done", {"B"});
        rig.service->RecordMonitorDepartures(rig.Capture(), {Row("B", "Open")});
        REQUIRE(rig.service->Rows().size() == 1);
        CHECK(rig.service->Rows()[0].issueKey == "B");

        // Under another query the additions say nothing: the listing stays as it was.
        rig.service->RecordListingAdditions("Jira@x", "v1", "other", {"C"});
        rig.service->RecordMonitorDepartures(rig.Capture(), {Row("C", "Open")});
        CHECK(rig.service->Rows().size() == 1);
    }

    TEST_CASE("only a complete listing moves when the view was last listed") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B", "C"}, {}, {});
        rig.now = 2000;
        rig.service->RecordListingAdditions("Jira@x", "v1", "status != Done", {"D"});
        rig.service->RecordMonitorDepartures(rig.Capture(), {Row("B", "Open")});
        rig.now = 3000;
        rig.service->RecordMonitorDepartures(rig.Capture(), {Row("C", "Open")});
        REQUIRE(rig.service->Rows().size() == 2);
        CHECK(rig.service->Rows()[0].issueKey == "C");
        CHECK(rig.service->Rows()[0].listedAtUnixSec == 1000);
        CHECK(rig.service->Rows()[0].leftAtUnixSec == 3000);

        rig.now = 4000;
        rig.service->RecordListing(rig.Capture(), {"A", "D"}, {}, {}); // a full answer: same rows, new time
        rig.now = 5000;
        rig.service->RecordMonitorDepartures(rig.Capture(), {Row("D", "Open")});
        CHECK(rig.service->Rows()[0].issueKey == "D");
        CHECK(rig.service->Rows()[0].listedAtUnixSec == 4000);
    }

    TEST_CASE("no tracker request while offline: the row settles as unavailable") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B"}, {}, {});
        rig.offline = true;
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {Row("B", "Open")});
        REQUIRE(rig.service->Rows().size() == 1);
        CHECK(rig.service->Rows()[0].reasonKind == LeftViewReasonKind::Unavailable);
        CHECK(rig.reader->historyCalls == 0);
    }

    TEST_CASE("the monitor records nothing the view's last reload did not answer") {
        Rig rig;
        rig.service->RecordMonitorDepartures(rig.Capture(), {Row("B", "Open")}); // no listing yet
        CHECK(rig.service->Rows().empty());
        rig.service->RecordListing(rig.Capture("real"), {"A", "B"}, {}, {});
        rig.service->RecordMonitorDepartures(rig.Capture("stale"), {Row("B", "Open")});
        CHECK(rig.service->Rows().empty());
    }

    TEST_CASE("a deleted issue needs no lookup, and a 404 on reload is reported as deleted") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B", "C"}, {}, {});
        LeftViewDeparture gone = Row("B", "Open");
        gone.deleted = true;
        const int callsBefore = rig.reader->historyCalls;
        rig.service->RecordMonitorDepartures(rig.Capture(), {gone});
        REQUIRE(rig.service->Rows().size() == 1);
        CHECK(rig.service->Rows()[0].kind == LeftViewKind::Deleted);
        CHECK(rig.service->Rows()[0].browseUrl.empty());
        CHECK(rig.reader->historyCalls == callsBefore);

        rig.reader->existsAnswer = false;
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {Row("C", "Open")});
        REQUIRE(rig.service->Rows().size() == 2);
        CHECK(rig.service->Rows()[0].issueKey == "C");
        CHECK(rig.service->Rows()[0].kind == LeftViewKind::Deleted);
    }

    TEST_CASE("an unreachable tracker keeps the row and says so") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B"}, {}, {});
        rig.reader->rowsFail = true;
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {Row("B", "Open")});
        REQUIRE(rig.service->Rows().size() == 1);
        CHECK(rig.service->Rows()[0].reasonKind == LeftViewReasonKind::Unavailable);
    }

    TEST_CASE("an unavailable reason is asked again once the site answers a reload") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B"}, {}, {});
        rig.reader->rowsFail = true;
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {Row("B", "Open")});
        REQUIRE(rig.service->Rows().size() == 1);
        REQUIRE(rig.service->Rows()[0].reasonKind == LeftViewReasonKind::Unavailable);

        rig.reader->rowsFail = false;
        rig.reader->rows.push_back(Fresh("B", "Done"));
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {});
        REQUIRE(rig.service->Rows().size() == 1);
        CHECK(rig.service->Rows()[0].reasonKind == LeftViewReasonKind::QueryFieldChanged);
        CHECK(rig.service->Rows()[0].reason == "Status: Open -> Done");
        const std::vector<LeftViewRecord> stored = rig.cache->LoadLeftViewRows(10);
        REQUIRE(stored.size() == 1);
        CHECK(stored[0].reasonKind == LeftViewReasonKind::QueryFieldChanged);
    }

    TEST_CASE("no retry while offline, and none for an issue the tracker answered without") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B", "C"}, {}, {});
        rig.reader->rowsFail = true;
        rig.service->RecordListing(rig.Capture(), {"A", "C"}, {}, {Row("B", "Open")});
        rig.offline = true;
        const int callsOffline = rig.reader->historyCalls;
        rig.service->RecordListing(rig.Capture(), {"A", "C"}, {}, {});
        CHECK(rig.reader->historyCalls == callsOffline);
        CHECK(rig.service->Rows()[0].reasonKind == LeftViewReasonKind::Unavailable);

        rig.offline = false;
        rig.reader->rowsFail = false; // answers without C: not visible, a settled answer
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {Row("C", "Open")});
        const LeftViewRecord* c = nullptr;
        for (const LeftViewRecord& row : rig.service->Rows()) {
            c = row.issueKey == "C" ? &row : c;
        }
        REQUIRE(c != nullptr);
        CHECK(c->reasonKind == LeftViewReasonKind::NotVisible);
        const int callsSettled = rig.reader->historyCalls;
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {});
        CHECK(rig.reader->historyCalls == callsSettled);
    }

    TEST_CASE("a hidden issue in the batch does not cost the others their history") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B", "C"}, {}, {});
        TrackerFieldChange change;
        change.issueKey = "B";
        change.fieldId = "status";
        change.fieldLabel = "status";
        change.fromValue = "Open";
        change.toValue = "Done";
        change.author = "Alice";
        rig.reader->history.push_back(change);
        rig.reader->historyRefusesKey = "C";
        rig.service->RecordListing(rig.Capture(), {"A"}, {}, {Row("B", "Open"), Row("C", "Open")});
        const LeftViewRecord* b = nullptr;
        for (const LeftViewRecord& row : rig.service->Rows()) {
            b = row.issueKey == "B" ? &row : b;
        }
        REQUIRE(b != nullptr);
        CHECK(b->author == "Alice");
        CHECK(b->reasonKind == LeftViewReasonKind::QueryFieldChanged);
    }

    TEST_CASE("lookups stop at the limit, and the rest say so") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B", "C", "D"}, {}, {});
        rig.service->RecordListing(rig.Capture(), {"D"}, {}, {});
        REQUIRE(rig.service->Rows().size() == 3);
        int notChecked = 0;
        for (const LeftViewRecord& row : rig.service->Rows()) {
            notChecked += row.reasonKind == LeftViewReasonKind::NotChecked ? 1 : 0;
        }
        CHECK(notChecked == 1); // limit is 2 lookups per batch
    }

    TEST_CASE("history survives a restart, newest first, and the row cap holds") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B", "C", "D", "E"}, {}, {});
        for (const char* left : {"B", "C", "D", "E"}) {
            ++rig.now;
            rig.service->RecordMonitorDepartures(rig.Capture(), {Row(left, "Open")});
        }
        CHECK(rig.service->Rows().size() == 3);

        std::unique_ptr<LeftViewHistoryService> restarted = rig.MakeService();
        restarted->Load();
        REQUIRE(restarted->Rows().size() == 3);
        CHECK(restarted->Rows()[0].issueKey == "E");
        CHECK(restarted->Rows()[2].issueKey == "C");
        CHECK(restarted->Rows()[0].summary == "E summary");
    }

    TEST_CASE("the listing survives a restart, so an overnight departure is found") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B"}, {}, {});
        std::unique_ptr<LeftViewHistoryService> restarted = rig.MakeService();
        restarted->Load();
        restarted->RecordListing(rig.Capture(), {"A"}, {}, {});
        REQUIRE(restarted->Rows().size() == 1);
        CHECK(restarted->Rows()[0].issueKey == "B");
    }

    TEST_CASE("dismiss and clear reach the table") {
        Rig rig;
        rig.service->RecordListing(rig.Capture(), {"A", "B", "C"}, {}, {});
        rig.service->RecordMonitorDepartures(rig.Capture(), {Row("B", "Open")});
        rig.service->RecordMonitorDepartures(rig.Capture(), {Row("C", "Open")});
        REQUIRE(rig.service->Rows().size() == 2);
        rig.service->Dismiss(rig.service->Rows()[0].id);
        CHECK(rig.service->Rows().size() == 1);
        CHECK(rig.cache->LoadLeftViewRows(10).size() == 1);
        rig.service->ClearAll();
        CHECK(rig.service->Rows().empty());
        CHECK(rig.cache->LoadLeftViewRows(10).empty());
    }

    TEST_CASE("a row whose lookup never finished gets a settled reason") {
        LeftViewRecord row;
        row.reasonKind = LeftViewReasonKind::Pending;
        SettleInterruptedLookup(row);
        CHECK(row.reasonKind == LeftViewReasonKind::NotChecked);
        CHECK(row.reason.find("not checked") != std::string::npos);
    }

    TEST_CASE("details: live values when connected, no request while offline, failure keeps the row") {
        Rig rig;
        LeftViewRecord record;
        record.id = 5;
        record.issueKey = "B";
        rig.reader->rows.push_back(Fresh("B", "Done"));
        rig.service->RequestDetails(record, TrackerConfig());
        CHECK(rig.service->Details().recordId == 5);
        CHECK(rig.service->Details().liveFields.at("status") == "Done");

        rig.offline = true;
        rig.service->RequestDetails(record, TrackerConfig());
        CHECK(rig.service->Details().status == LeftViewDetails::Status::Offline);
        CHECK(rig.service->Details().liveFields.empty());

        rig.offline = false;
        rig.reader->rowsFail = true;
        rig.service->RequestDetails(record, TrackerConfig());
        CHECK(rig.service->Details().status == LeftViewDetails::Status::Failed);
        CHECK_FALSE(rig.service->Details().error.empty());

        LeftViewRecord deleted = record;
        deleted.kind = LeftViewKind::Deleted;
        rig.service->RequestDetails(deleted, TrackerConfig());
        CHECK(rig.service->Details().status == LeftViewDetails::Status::Idle);
    }
}
