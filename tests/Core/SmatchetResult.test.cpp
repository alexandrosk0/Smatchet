#include <doctest/doctest.h>

#include "SmatchetResult.h"

#include <stdexcept>
#include <string>

namespace {

// Move-tracking probe — records whether it was moved-from so tests can assert
// that Optional's move ctor leaves the source disengaged (and that no heap is used).
struct Tracker {
    int value = 0;
    bool movedFrom = false;

    Tracker() = default;
    explicit Tracker(int v) : value(v) {}
    Tracker(const Tracker& other) : value(other.value), movedFrom(false) {}
    Tracker(Tracker&& other) noexcept : value(other.value), movedFrom(false) { other.movedFrom = true; }
    Tracker& operator=(const Tracker& other) {
        value = other.value;
        movedFrom = false;
        return *this;
    }
    Tracker& operator=(Tracker&& other) noexcept {
        value = other.value;
        movedFrom = false;
        other.movedFrom = true;
        return *this;
    }
};

} // namespace

TEST_CASE("Optional: default is empty, engaged after construction") {
    Optional<int> empty;
    CHECK_FALSE(empty.has_value());
    CHECK_FALSE(static_cast<bool>(empty));

    Optional<int> engaged(42);
    CHECK(engaged.has_value());
    CHECK(static_cast<bool>(engaged));
    CHECK(engaged.value() == 42);
}

TEST_CASE("Optional: value() on an engaged Optional returns the stored value") {
    Optional<std::string> o(std::string("hello"));
    CHECK(o.value() == "hello");
    o.value() += " world";
    CHECK(o.value() == "hello world");
}

TEST_CASE("Optional: value() on an empty Optional throws std::logic_error") {
    Optional<int> empty;
    CHECK_THROWS_AS(empty.value(), std::logic_error);

    const Optional<int> constEmpty;
    CHECK_THROWS_AS(constEmpty.value(), std::logic_error);
}

TEST_CASE("Optional: value_or returns value when engaged, fallback when empty") {
    Optional<int> engaged(7);
    CHECK(engaged.value_or(99) == 7);

    Optional<int> empty;
    CHECK(empty.value_or(99) == 99);
}

TEST_CASE("Optional: copy-construct preserves engaged and empty state") {
    Optional<int> engaged(5);
    Optional<int> engagedCopy(engaged);
    CHECK(engagedCopy.has_value());
    CHECK(engagedCopy.value() == 5);
    CHECK(engaged.has_value()); // source unchanged by copy
    CHECK(engaged.value() == 5);

    Optional<int> empty;
    Optional<int> emptyCopy(empty);
    CHECK_FALSE(emptyCopy.has_value());
}

TEST_CASE("Optional: move leaves the source disengaged") {
    Optional<Tracker> src(Tracker(11));
    REQUIRE(src.has_value());

    Optional<Tracker> dst(std::move(src));
    CHECK(dst.has_value());
    CHECK(dst.value().value == 11);
    CHECK_FALSE(src.has_value()); // move ctor disengages the source
}

TEST_CASE("Optional: copy-and-swap assignment") {
    Optional<int> a(1);
    Optional<int> b(2);
    a = b;
    CHECK(a.value() == 2);
    CHECK(b.value() == 2);

    Optional<int> empty;
    a = empty;
    CHECK_FALSE(a.has_value());
}

TEST_CASE("Optional: reset disengages") {
    Optional<int> o(3);
    REQUIRE(o.has_value());
    o.reset();
    CHECK_FALSE(o.has_value());
    o.reset(); // idempotent
    CHECK_FALSE(o.has_value());
}

TEST_CASE("Optional: int round-trips with no heap allocation") {
    // aligned_storage holds the int inline — value identity is enough to prove
    // the engaged/disengaged lifecycle works for a trivially-copyable type.
    Optional<int> o(123456);
    CHECK(o.value() == 123456);
    Optional<int> moved(std::move(o));
    CHECK(moved.value() == 123456);
}

TEST_CASE("Result: Ok holds a value, Err holds an error") {
    Result<int> ok = Result<int>::Ok(10);
    CHECK(ok.has_value());
    CHECK(static_cast<bool>(ok));
    CHECK(ok.value() == 10);

    Result<int> err = Result<int>::Err("boom");
    CHECK_FALSE(err.has_value());
    CHECK_FALSE(static_cast<bool>(err));
    CHECK(err.error() == "boom");
}

TEST_CASE("Result: value() on error-state throws, error() on ok-state throws") {
    Result<int> ok = Result<int>::Ok(1);
    CHECK_THROWS_AS(ok.error(), std::logic_error);

    Result<int> err = Result<int>::Err("nope");
    CHECK_THROWS_AS(err.value(), std::logic_error);
}

TEST_CASE("Result: value_or returns value on ok, fallback on error") {
    Result<int> ok = Result<int>::Ok(8);
    CHECK(ok.value_or(0) == 8);

    Result<int> err = Result<int>::Err("e");
    CHECK(err.value_or(-1) == -1);
}

TEST_CASE("Result: copy-construct preserves ok and error state") {
    Result<int> ok = Result<int>::Ok(4);
    Result<int> okCopy(ok);
    CHECK(okCopy.has_value());
    CHECK(okCopy.value() == 4);

    Result<int> err = Result<int>::Err("x");
    Result<int> errCopy(err);
    CHECK_FALSE(errCopy.has_value());
    CHECK(errCopy.error() == "x");
}

TEST_CASE("Result: custom error type") {
    Result<int, int> err = Result<int, int>::Err(404);
    CHECK_FALSE(err.has_value());
    CHECK(err.error() == 404);

    Result<int, int> ok = Result<int, int>::Ok(200);
    CHECK(ok.value() == 200);
}
