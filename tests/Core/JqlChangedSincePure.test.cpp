// Bucket-A coverage for JqlChangedSincePure (ticket-change-monitor plan, S1a) — the pure
// query-window helpers the change monitor uses to narrow each poll to recently-touched
// issues. WrapJqlChangedWithin builds the Jira relative window (and must reposition a
// trailing ORDER BY); IsoSinceFromWindow builds an absolute ISO-8601 since for the
// GitHub/Plane path. Both are deterministic — no clock, no timezone DB.

#include "Sync/JqlChangedSincePure.h"

#include "doctest/doctest.h"

#include <string>

using smatchet::IsoSinceFromWindow;
using smatchet::WrapJqlChangedWithin;

TEST_CASE("WrapJqlChangedWithin: simple base gets parenthesised AND window") {
    CHECK(WrapJqlChangedWithin("project = ABC", 120) == "(project = ABC) AND updated >= -120m");
}

TEST_CASE("WrapJqlChangedWithin: empty/blank base yields the bare window clause") {
    CHECK(WrapJqlChangedWithin("", 30) == "updated >= -30m");
    CHECK(WrapJqlChangedWithin("   ", 30) == "updated >= -30m");
}

TEST_CASE("WrapJqlChangedWithin: minutes clamped to at least 1") {
    CHECK(WrapJqlChangedWithin("a = b", 0) == "(a = b) AND updated >= -1m");
    CHECK(WrapJqlChangedWithin("a = b", -5) == "(a = b) AND updated >= -1m");
}

TEST_CASE("WrapJqlChangedWithin: trailing ORDER BY moves after the window") {
    CHECK(WrapJqlChangedWithin("project = ABC ORDER BY created DESC", 60) ==
          "(project = ABC) AND updated >= -60m ORDER BY created DESC");
}

TEST_CASE("WrapJqlChangedWithin: ORDER BY match is case-insensitive") {
    CHECK(WrapJqlChangedWithin("project = ABC order by created", 60) ==
          "(project = ABC) AND updated >= -60m order by created");
}

TEST_CASE("WrapJqlChangedWithin: 'reorder'/'standby' are not ORDER BY keywords") {
    // "reorder" contains "order" but is not a word-boundary match; left untouched.
    CHECK(WrapJqlChangedWithin("summary ~ reorder", 60) == "(summary ~ reorder) AND updated >= -60m");
}

TEST_CASE("WrapJqlChangedWithin: only the trailing ORDER BY is repositioned") {
    // An "order by" appearing earlier as text plus the real trailing sort: rightmost wins.
    CHECK(WrapJqlChangedWithin("text ~ \"order by\" ORDER BY rank", 60) ==
          "(text ~ \"order by\") AND updated >= -60m ORDER BY rank");
}

TEST_CASE("WrapJqlChangedWithin: a quoted 'order by' with NO real trailing clause is not "
          "mistaken for one") {
    // CPP_CODE_AUDIT.md #33: unlike the "rightmost wins" case above, THIS query has no real
    // trailing ORDER BY at all — the quoted text is the only "order by" substring. The old
    // context-blind scan would have split the query mid-string-literal here (there's no later
    // real match to mask it), producing a malformed changed-since poll query.
    CHECK(WrapJqlChangedWithin("summary ~ \"sort order by date\" AND status = Open", 60) ==
          "(summary ~ \"sort order by date\" AND status = Open) AND updated >= -60m");
    // Single-quoted variant — JQL accepts both quote styles.
    CHECK(WrapJqlChangedWithin("summary ~ 'sort order by date' AND status = Open", 60) ==
          "(summary ~ 'sort order by date' AND status = Open) AND updated >= -60m");
}

TEST_CASE("IsoSinceFromWindow: subtracts the window and formats UTC Z") {
    // 2026-06-20T12:00:00Z == 1781956800; minus 3600s -> 11:00:00Z.
    CHECK(IsoSinceFromWindow(1781956800, 3600) == "2026-06-20T11:00:00Z");
}

TEST_CASE("IsoSinceFromWindow: zero window is the instant itself") {
    CHECK(IsoSinceFromWindow(1781956800, 0) == "2026-06-20T12:00:00Z");
}

TEST_CASE("IsoSinceFromWindow: the Unix epoch formats correctly") {
    CHECK(IsoSinceFromWindow(0, 0) == "1970-01-01T00:00:00Z");
}

TEST_CASE("IsoSinceFromWindow: negative window treated as zero, never goes negative") {
    CHECK(IsoSinceFromWindow(1781956800, -100) == "2026-06-20T12:00:00Z");
    CHECK(IsoSinceFromWindow(10, 100) == "1970-01-01T00:00:00Z");
}

TEST_CASE("IsoSinceFromWindow: crosses a day boundary backward") {
    // 2026-06-20T00:30:00Z == 1781915400; minus 3600s -> 2026-06-19T23:30:00Z.
    CHECK(IsoSinceFromWindow(1781915400, 3600) == "2026-06-19T23:30:00Z");
}
