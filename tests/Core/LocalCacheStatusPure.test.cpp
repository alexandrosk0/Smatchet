// The local cache's status vocabulary (Persistence/LocalCacheStatusPure.h): the recovery record's JSON round trip,
// the rejection of anything that is not a current-version record, and the startup notice wording rules.

#include "Persistence/LocalCacheStatusPure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::persistence::DecodeRecoveryReport;
using smatchet::persistence::DescribeLocalCacheStartupNotice;
using smatchet::persistence::EncodeRecoveryReport;
using smatchet::persistence::LocalCacheNoticeStrings;
using smatchet::persistence::LocalCacheRecoveryReport;
using smatchet::persistence::LocalCacheState;
using smatchet::persistence::LocalCacheStatus;

namespace {

LocalCacheNoticeStrings Strings() {
    LocalCacheNoticeStrings s;
    s.SessionOnly = "open failed at {path}: {error}";
    s.Rebuilt = "rebuilt; copy at {quarantined}";
    s.RebuiltSalvaged = "rebuilt; {rows} recovered; copy at {quarantined}";
    s.RebuiltSalvageIncomplete = "rebuilt; {rows} recovered, some lost; copy at {quarantined}";
    return s;
}

LocalCacheRecoveryReport Rebuilt() {
    LocalCacheRecoveryReport r;
    r.Rebuilt = true;
    r.WhenSec = 1700000000;
    r.QuarantinedPath = "/data/cache.sqlite.corrupt-1700000000";
    r.Reason = "file is not a database";
    r.FromThisLaunch = true;
    return r;
}

LocalCacheStatus Ready(const LocalCacheRecoveryReport& recovery) {
    LocalCacheStatus status;
    status.State = LocalCacheState::Ready;
    status.LastRecovery = recovery;
    return status;
}

} // namespace

TEST_CASE("recovery report: encode then decode round-trips every field, except the launch flag") {
    LocalCacheRecoveryReport in = Rebuilt();
    in.SalvageAttempted = true;
    in.SalvagedRows = 7;
    in.IncompleteTables.push_back("pending_actions");
    in.IncompleteTables.push_back("cache_meta");

    LocalCacheRecoveryReport out;
    REQUIRE(DecodeRecoveryReport(EncodeRecoveryReport(in), out));
    CHECK(out.Rebuilt);
    CHECK(out.WhenSec == in.WhenSec);
    CHECK(out.QuarantinedPath == in.QuarantinedPath);
    CHECK(out.Reason == in.Reason);
    CHECK(out.SalvageAttempted);
    CHECK(out.SalvagedRows == 7);
    REQUIRE(out.IncompleteTables.size() == 2);
    CHECK(out.IncompleteTables[1] == "cache_meta");
    CHECK_FALSE(out.FromThisLaunch); // a stored record is by definition from an earlier launch
}

TEST_CASE("recovery report: anything that is not a current record is rejected and leaves the output alone") {
    LocalCacheRecoveryReport untouched;
    untouched.QuarantinedPath = "keep";
    LocalCacheRecoveryReport out = untouched;

    CHECK_FALSE(DecodeRecoveryReport("", out));
    CHECK_FALSE(DecodeRecoveryReport("not json", out));
    CHECK_FALSE(DecodeRecoveryReport("[1,2]", out));
    CHECK_FALSE(DecodeRecoveryReport("{}", out));
    CHECK_FALSE(DecodeRecoveryReport("{\"v\":2,\"rebuilt\":true}", out)); // a newer shape is not misread
    // Right version, wrong field type.
    CHECK_FALSE(DecodeRecoveryReport("{\"v\":1,\"rebuilt\":\"yes\",\"when\":1,\"quarantined\":\"\",\"reason\":\"\","
                                     "\"salvage_attempted\":false,\"salvaged_rows\":0,\"incomplete\":[]}",
                                     out));
    CHECK_FALSE(DecodeRecoveryReport("{\"v\":1,\"rebuilt\":true,\"when\":1,\"quarantined\":\"\",\"reason\":\"\","
                                     "\"salvage_attempted\":false,\"salvaged_rows\":0,\"incomplete\":[1]}",
                                     out));
    CHECK(out.QuarantinedPath == "keep");

    // Larger than any record we write.
    CHECK_FALSE(DecodeRecoveryReport(std::string(smatchet::persistence::kMaxRecoveryReportBytes + 1, ' '), out));
}

TEST_CASE("startup notice: a session-only cache always says so, with the path and the reason") {
    LocalCacheStatus status;
    status.State = LocalCacheState::SessionOnly;
    status.Path = "/ro/cache.sqlite";
    status.LastError = "unable to open database file";
    CHECK(DescribeLocalCacheStartupNotice(status, Strings()) ==
          "open failed at /ro/cache.sqlite: unable to open database file");
}

TEST_CASE("startup notice: nothing to say for a healthy cache") {
    CHECK(DescribeLocalCacheStartupNotice(Ready(LocalCacheRecoveryReport()), Strings()).empty());
    LocalCacheStatus opening;
    opening.State = LocalCacheState::Opening;
    CHECK(DescribeLocalCacheStartupNotice(opening, Strings()).empty());
}

TEST_CASE("startup notice: a rebuild is announced once, on the launch that did it") {
    LocalCacheRecoveryReport r = Rebuilt();
    CHECK(DescribeLocalCacheStartupNotice(Ready(r), Strings()) ==
          "rebuilt; copy at /data/cache.sqlite.corrupt-1700000000");
    r.FromThisLaunch = false; // a later launch reads the stored record: no repeat toast
    CHECK(DescribeLocalCacheStartupNotice(Ready(r), Strings()).empty());
}

TEST_CASE("startup notice: the rebuild wording follows what the salvage managed") {
    LocalCacheRecoveryReport r = Rebuilt();
    r.SalvageAttempted = true;
    r.SalvagedRows = 12;
    CHECK(DescribeLocalCacheStartupNotice(Ready(r), Strings()) ==
          "rebuilt; 12 recovered; copy at /data/cache.sqlite.corrupt-1700000000");
    r.IncompleteTables.push_back("pending_actions");
    CHECK(DescribeLocalCacheStartupNotice(Ready(r), Strings()) ==
          "rebuilt; 12 recovered, some lost; copy at /data/cache.sqlite.corrupt-1700000000");
}

TEST_CASE("startup notice: a placeholder inside a value is not expanded a second time") {
    LocalCacheStatus status;
    status.State = LocalCacheState::SessionOnly;
    status.Path = "/odd/{error}/cache.sqlite";
    status.LastError = "boom";
    CHECK(DescribeLocalCacheStartupNotice(status, Strings()) == "open failed at /odd/{error}/cache.sqlite: boom");
}

TEST_CASE("startup notice: an unknown placeholder is left as written") {
    LocalCacheStatus status;
    status.State = LocalCacheState::SessionOnly;
    LocalCacheNoticeStrings strings = Strings();
    strings.SessionOnly = "{nope} at {path}";
    status.Path = "/p";
    CHECK(DescribeLocalCacheStartupNotice(status, strings) == "{nope} at /p");
}
