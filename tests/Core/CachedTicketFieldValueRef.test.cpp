// CachedTicketFieldValueRef.test.cpp — Phase 5 pull-forward of
// docs/plans/shipped/memory-budget-and-lifetime-hardening.md.
//
// Locks the zero-copy CachedTicket::GetFieldValueRef accessor added to avoid the
// per-comparison std::string copies GetFieldValue() made inside the grid's
// stable_sort comparator (Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp).
//
// Header-only POD (CachedTicketTypes.h is SQLite-free) — no .cpp link, pure doctest.

#include "CachedTicketTypes.h"

#include "doctest/doctest.h"

TEST_CASE("GetFieldValueRef returns a zero-copy view of the stored value") {
    CachedTicket t;
    t.fieldValues["status"] = "In Progress";
    const std::string& ref = t.GetFieldValueRef("status");
    CHECK(ref == "In Progress");
    // It is the stored string itself, not a copy — the whole point of the accessor.
    CHECK(&ref == &t.fieldValues.at("status"));
}

TEST_CASE("GetFieldValueRef returns a stable empty string for a missing key") {
    CachedTicket t;
    const std::string& a = t.GetFieldValueRef("nope");
    CHECK(a.empty());
    // Same static empty instance every call — no per-call allocation on the miss path.
    const std::string& b = t.GetFieldValueRef("also-missing");
    CHECK(&a == &b);
}

TEST_CASE("GetFieldValueRef agrees with GetFieldValue") {
    CachedTicket t;
    t.fieldValues["k"] = "v";
    CHECK(t.GetFieldValueRef("k") == t.GetFieldValue("k"));
    CHECK(t.GetFieldValueRef("missing") == t.GetFieldValue("missing"));
}
