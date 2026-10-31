// RemoteVersionPure — bucket-A coverage for the tracker "updated" stamp parser/comparator
// (Source/Core/include/Sync/RemoteVersionPure.h, docs/plans/active/delta-sync.md § Phase 1).
// Header-only and pure: no AppController, no HTTP.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "Sync/RemoteVersionPure.h"

using smatchet::sync::CompareRemoteUpdated;
using smatchet::sync::IsIncomingRowStale;
using smatchet::sync::ParseRemoteUpdatedMs;
using smatchet::sync::RemoteVersionOrder;

TEST_CASE("ParseRemoteUpdatedMs parses the Jira stamp shape") {
    std::int64_t ms = 0;
    REQUIRE(ParseRemoteUpdatedMs("1970-01-01T00:00:00.000+0000", ms));
    CHECK(ms == 0);
    REQUIRE(ParseRemoteUpdatedMs("1970-01-02T00:00:00.000+0000", ms));
    CHECK(ms == 86400000);
    REQUIRE(ParseRemoteUpdatedMs("2024-05-01T10:00:00.123+0000", ms));
    // 2024-05-01T10:00:00Z == 1714557600 s.
    CHECK(ms == 1714557600123LL);
}

TEST_CASE("ParseRemoteUpdatedMs normalises offsets, Z, and extra fraction digits") {
    std::int64_t utc = 0;
    std::int64_t plusOne = 0;
    std::int64_t zulu = 0;
    std::int64_t micro = 0;
    REQUIRE(ParseRemoteUpdatedMs("2024-05-01T10:00:00.000+0000", utc));
    REQUIRE(ParseRemoteUpdatedMs("2024-05-01T11:00:00.000+01:00", plusOne));
    REQUIRE(ParseRemoteUpdatedMs("2024-05-01T10:00:00Z", zulu));
    REQUIRE(ParseRemoteUpdatedMs("2024-05-01T10:00:00.999999Z", micro));
    CHECK(utc == plusOne);
    CHECK(utc == zulu);
    CHECK(micro == utc + 999);
}

TEST_CASE("ParseRemoteUpdatedMs rejects malformed or zone-less stamps and leaves the output alone") {
    std::int64_t ms = 42;
    CHECK_FALSE(ParseRemoteUpdatedMs("", ms));
    CHECK_FALSE(ParseRemoteUpdatedMs("yesterday", ms));
    CHECK_FALSE(ParseRemoteUpdatedMs("2024-05-01", ms));
    CHECK_FALSE(ParseRemoteUpdatedMs("2024-05-01T10:00:00", ms)); // no zone: ambiguous
    CHECK_FALSE(ParseRemoteUpdatedMs("2024-13-01T10:00:00Z", ms));
    CHECK_FALSE(ParseRemoteUpdatedMs("2024-05-01T25:00:00Z", ms));
    CHECK_FALSE(ParseRemoteUpdatedMs("2024-05-01T10:00:00+99999", ms));
    CHECK_FALSE(ParseRemoteUpdatedMs("2024-05-01T10:00:00Zjunk", ms));
    CHECK(ms == 42);
}

TEST_CASE("CompareRemoteUpdated orders by instant, not by spelling") {
    CHECK(CompareRemoteUpdated("2024-05-01T10:00:00.000+0000", "2024-05-01T10:00:00Z") == RemoteVersionOrder::Same);
    CHECK(CompareRemoteUpdated("2024-05-01T10:00:01.000+0000", "2024-05-01T10:00:00.000+0000") ==
          RemoteVersionOrder::Newer);
    CHECK(CompareRemoteUpdated("2024-05-01T09:59:59.999+0000", "2024-05-01T10:00:00.000+0000") ==
          RemoteVersionOrder::Older);
    // 11:00+01:00 is 10:00Z, so it is OLDER than 10:30Z even though "11:00" > "10:30" as text.
    CHECK(CompareRemoteUpdated("2024-05-01T11:00:00.000+01:00", "2024-05-01T10:30:00.000+0000") ==
          RemoteVersionOrder::Older);
}

TEST_CASE("CompareRemoteUpdated is Unknown when either side cannot be ordered") {
    CHECK(CompareRemoteUpdated("", "2024-05-01T10:00:00Z") == RemoteVersionOrder::Unknown);
    CHECK(CompareRemoteUpdated("2024-05-01T10:00:00Z", "") == RemoteVersionOrder::Unknown);
    CHECK(CompareRemoteUpdated("junk", "junk") == RemoteVersionOrder::Unknown);
}

TEST_CASE("IsIncomingRowStale blocks only a provably older row") {
    CHECK(IsIncomingRowStale("2024-05-01T09:00:00Z", "2024-05-01T10:00:00Z"));
    CHECK_FALSE(IsIncomingRowStale("2024-05-01T10:00:00Z", "2024-05-01T10:00:00Z"));
    CHECK_FALSE(IsIncomingRowStale("2024-05-01T11:00:00Z", "2024-05-01T10:00:00Z"));
    // Unknown never blocks: today's behaviour (the incoming row wins) is preserved.
    CHECK_FALSE(IsIncomingRowStale("", "2024-05-01T10:00:00Z"));
    CHECK_FALSE(IsIncomingRowStale("2024-05-01T10:00:00Z", ""));
}

TEST_CASE("IsIncomingRowStale with a clock treats a resident stamp more than a day ahead as unknown") {
    constexpr std::int64_t nowMs = 1777627200000LL; // 2026-05-01T09:00:00Z
    // A resident stamp a few hours ahead is within the one-day tolerance: it still blocks an older row.
    CHECK(IsIncomingRowStale("2026-05-01T09:00:00Z", "2026-05-01T12:00:00Z", nowMs));
    // Far in the future: not a real tracker stamp, so it never blocks.
    CHECK_FALSE(IsIncomingRowStale("2026-05-01T09:00:00Z", "2999-01-01T00:00:00Z", nowMs));
    CHECK(smatchet::sync::IsStampImplausiblyInFuture("2999-01-01T00:00:00Z", nowMs));
    CHECK_FALSE(smatchet::sync::IsStampImplausiblyInFuture("garbage", nowMs));
    CHECK_FALSE(smatchet::sync::IsStampImplausiblyInFuture("2026-05-01T12:00:00Z", nowMs));
}

TEST_CASE("IsSameRemoteVersion: equal text or one instant in two renderings; unparsable stamps need equal text") {
    using smatchet::sync::IsSameRemoteVersion;
    CHECK(IsSameRemoteVersion("2024-05-01T10:00:00.000+0000", "2024-05-01T10:00:00.000+0000"));
    CHECK(IsSameRemoteVersion("2024-05-01T10:00:00.000+0000", "2024-05-01T10:00:00Z"));
    CHECK(IsSameRemoteVersion("2024-05-01T10:00:00.000+0000", "2024-05-01T11:00:00.000+0100"));
    CHECK_FALSE(IsSameRemoteVersion("2024-05-01T10:00:00.000+0000", "2024-05-01T10:00:00.001+0000"));
    CHECK(IsSameRemoteVersion("opaque-v7", "opaque-v7"));
    CHECK_FALSE(IsSameRemoteVersion("opaque-v7", "opaque-v8"));
    CHECK_FALSE(IsSameRemoteVersion("opaque-v7", ""));
}
