// Bucket-A coverage for NotificationCenterPure (ticket-change-monitor plan, S3 / § Files-to-modify
// #13a) — the STL-only row formatting the Notification Center window draws per ToastHistoryEntry:
// the severity tag + the UTC HH:MM:SS timestamp. Pure (no ImGui), so it is testable without a UI
// session; the window body that consumes these is bucket-E (tests/ui/notification_center.test.cpp).

#include "Ui/NotificationCenterPure.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <string>

TEST_CASE("ToastTypeShortLabel: one stable tag per severity") {
    CHECK(std::string(smatchet::ToastTypeShortLabel(ToastType::Info)) == "INFO");
    CHECK(std::string(smatchet::ToastTypeShortLabel(ToastType::Success)) == "OK");
    CHECK(std::string(smatchet::ToastTypeShortLabel(ToastType::Warning)) == "WARN");
    CHECK(std::string(smatchet::ToastTypeShortLabel(ToastType::Error)) == "ERR");
}

TEST_CASE("FormatClockHMS: zero is midnight UTC") {
    CHECK(smatchet::FormatClockHMS(0) == "00:00:00");
}

TEST_CASE("FormatClockHMS: zero-pads each field") {
    // 01:02:03 = 1*3600 + 2*60 + 3 = 3723
    CHECK(smatchet::FormatClockHMS(3723) == "01:02:03");
}

TEST_CASE("FormatClockHMS: end-of-day boundary") {
    // 23:59:59 = 86399
    CHECK(smatchet::FormatClockHMS(86399) == "23:59:59");
}

TEST_CASE("FormatClockHMS: wraps to time-of-day across whole days") {
    // 86400 (one full day) wraps back to midnight; +3661 = 01:01:01.
    CHECK(smatchet::FormatClockHMS(86400) == "00:00:00");
    CHECK(smatchet::FormatClockHMS(86400 + 3661) == "01:01:01");
}

TEST_CASE("FormatClockHMS: pre-epoch negative input wraps into the day (no malformed string)") {
    // Floored modulo: -1s before epoch is 23:59:59 of the prior day, not "-0:-0:-1".
    const std::string hms = smatchet::FormatClockHMS(-1);
    CHECK(hms.size() == 8);
    CHECK(hms == "23:59:59");
}
