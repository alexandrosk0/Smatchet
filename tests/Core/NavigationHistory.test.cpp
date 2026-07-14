// NavigationHistory — back/forward JQL navigation stack. Pure value type (no
// ImGui / AppController / cpr / SQLite); previously had zero automated coverage.

#include <doctest/doctest.h>

#include "Ui/NavigationHistory.h"

#include <string>

namespace {
NavigationEntry E(const std::string& jql) {
    NavigationEntry e;
    e.Jql = jql;
    return e;
}
} // namespace

TEST_CASE("NavigationHistory starts empty and cannot move") {
    NavigationHistory h;
    CHECK_FALSE(h.CanGoBack());
    CHECK_FALSE(h.CanGoForward());
    CHECK(h.Current() == nullptr);
    CHECK(h.GoBack() == nullptr);
    CHECK(h.GoForward() == nullptr);
}

TEST_CASE("Push makes the entry current; back/forward walk the stack") {
    NavigationHistory h;
    h.Push(E("a"));
    REQUIRE(h.Current() != nullptr);
    CHECK(h.Current()->Jql == "a");
    CHECK_FALSE(h.CanGoBack());

    h.Push(E("b"));
    CHECK(h.Current()->Jql == "b");
    CHECK(h.CanGoBack());
    CHECK_FALSE(h.CanGoForward());

    const NavigationEntry* back = h.GoBack();
    REQUIRE(back != nullptr);
    CHECK(back->Jql == "a");
    CHECK(h.CanGoForward());

    const NavigationEntry* fwd = h.GoForward();
    REQUIRE(fwd != nullptr);
    CHECK(fwd->Jql == "b");
}

TEST_CASE("Pushing a consecutive duplicate is a no-op") {
    NavigationHistory h;
    h.Push(E("same"));
    h.Push(E("same"));
    CHECK_FALSE(h.CanGoBack()); // still a single entry
    CHECK(h.Current()->Jql == "same");
}

TEST_CASE("Pushing after GoBack truncates the forward history") {
    NavigationHistory h;
    h.Push(E("a"));
    h.Push(E("b"));
    h.Push(E("c"));
    h.GoBack(); // now at "b", forward = {c}
    CHECK(h.CanGoForward());
    h.Push(E("d")); // truncates "c"
    CHECK_FALSE(h.CanGoForward());
    CHECK(h.Current()->Jql == "d");
    CHECK(h.GoBack()->Jql == "b");
}

TEST_CASE("Clear empties the history") {
    NavigationHistory h;
    h.Push(E("a"));
    h.Push(E("b"));
    h.Clear();
    CHECK_FALSE(h.CanGoBack());
    CHECK_FALSE(h.CanGoForward());
    CHECK(h.Current() == nullptr);
}

TEST_CASE("History is capped and drops the oldest entries") {
    NavigationHistory h;
    // Push well past the internal 200-entry cap with unique values.
    for (int i = 0; i < 300; ++i) {
        h.Push(E("q" + std::to_string(i)));
    }
    CHECK(h.Current()->Jql == "q299");
    // Walk all the way back; the reachable count is bounded by the cap, and the
    // oldest ("q0") must have been evicted.
    int steps = 0;
    while (h.CanGoBack()) {
        h.GoBack();
        ++steps;
    }
    CHECK(steps <= 200);
    CHECK(h.Current()->Jql != "q0");
}
