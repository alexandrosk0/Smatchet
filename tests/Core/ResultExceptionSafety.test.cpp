#include <doctest/doctest.h>

#include "SmatchetResult.h"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// DR31 — exception-safety of Result move-assignment and Optional::operator=.
// A payload whose move constructor can throw is the only trigger: the common
// nothrow case (vector/string/json/TrackerError) is unaffected. These tests use
// a payload with a throw-armed move plus alive-tracking counters so a
// double-destroy shows up as a nonzero doubleDestroys tally (and, under CI ASan,
// as a hard failure) rather than silent UB.

// Wrong-state access is a two-stage Pillar-3 contract (assert in DEBUG, throw
// std::logic_error in RELEASE), so the throw-observing checks are gated to
// NDEBUG builds — matching SmatchetResult.test.cpp. Under a live assert the
// abort fires before any exception unwinds, which is correct designed behaviour.
#ifdef NDEBUG
#define DR31_CHECK_WRONGSTATE_THROWS(expr, exc) CHECK_THROWS_AS(expr, exc)
#else
#define DR31_CHECK_WRONGSTATE_THROWS(expr, exc) ((void)0) // assert aborts in Debug
#endif

namespace {

struct ThrowState {
    static int liveCount;      // net live objects (ctor +1, dtor -1)
    static int doubleDestroys; // dtor entered on an already-dead object
    static bool armMove;       // when set, the next move ctor throws
    static void reset() {
        liveCount = 0;
        doubleDestroys = 0;
        armMove = false;
    }
};
int ThrowState::liveCount = 0;
int ThrowState::doubleDestroys = 0;
bool ThrowState::armMove = false;

// Payload whose MOVE constructor throws on demand. Deliberately not
// nothrow-move-constructible so it exercises the DR31 throwing-move edge.
struct ThrowingMover {
    int value;
    bool alive;

    explicit ThrowingMover(int v) : value(v), alive(true) { ThrowState::liveCount++; }
    ThrowingMover(const ThrowingMover& o) : value(o.value), alive(true) {
        ThrowState::liveCount++;
    }
    ThrowingMover(ThrowingMover&& o) : value(o.value), alive(true) {
        if (ThrowState::armMove) {
            throw std::runtime_error("move boom"); // half-built object is never destroyed
        }
        ThrowState::liveCount++;
    }
    ThrowingMover& operator=(const ThrowingMover&) = default;
    ThrowingMover& operator=(ThrowingMover&&) = default;
    ~ThrowingMover() {
        if (!alive) {
            ThrowState::doubleDestroys++;
        }
        alive = false;
        ThrowState::liveCount--;
    }
};

} // namespace

TEST_CASE("DR31 Result move-assign survives a throwing move without double-destroy") {
    ThrowState::reset();
    {
        Result<ThrowingMover> dst = Result<ThrowingMover>::Ok(ThrowingMover(1));
        Result<ThrowingMover> src = Result<ThrowingMover>::Ok(ThrowingMover(2));

        ThrowState::armMove = true;
        bool threw = false;
        try {
            dst = std::move(src);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        ThrowState::armMove = false;

        CHECK(threw);
        // dst is now valueless: destructible, reports no value, accessors throw
        // (Pillar 3) instead of dereferencing raw storage.
        CHECK_FALSE(dst.has_value());
        CHECK_FALSE(static_cast<bool>(dst));
        DR31_CHECK_WRONGSTATE_THROWS(dst.value(), std::logic_error);
        DR31_CHECK_WRONGSTATE_THROWS(dst.error(), std::logic_error);
        CHECK(ThrowState::doubleDestroys == 0);
    }
    // Leaving scope destroys dst (valueless, no-op) and src (intact). No leak,
    // no double free.
    CHECK(ThrowState::doubleDestroys == 0);
    CHECK(ThrowState::liveCount == 0);
}

TEST_CASE("DR31 Optional::operator= propagates a throwing move instead of terminating") {
    ThrowState::reset();
    {
        Optional<ThrowingMover> a;                   // disengaged
        Optional<ThrowingMover> b(ThrowingMover(5)); // engaged

        ThrowState::armMove = true;
        bool threw = false;
        try {
            a = b; // by-value param copy-built, then swap move-constructs into a and throws
        } catch (const std::runtime_error&) {
            threw = true;
        }
        ThrowState::armMove = false;

        CHECK(threw); // reached us as an exception, not std::terminate (the fix)
        CHECK_FALSE(a.has_value());
        CHECK(b.has_value());
        CHECK(ThrowState::doubleDestroys == 0);
    }
    CHECK(ThrowState::doubleDestroys == 0);
    CHECK(ThrowState::liveCount == 0);
}

TEST_CASE("DR31 payload with a throwing move is not nothrow-assignable") {
    // Sanity anchors for the exception-safety guarantee that the behavioural test
    // above exercises: the payload's move can throw, and an Optional wrapping it is
    // therefore not advertised as nothrow-move-assignable, so the throwing move is
    // allowed to propagate rather than being forced through a noexcept boundary into
    // std::terminate. The positive nothrow-int case is intentionally not asserted:
    // Optional::operator= takes its parameter by value, so the trait also folds in
    // Optional's (non-noexcept) move constructor and cannot isolate operator=' own
    // spec under the project's C++14 build.
    CHECK_FALSE(std::is_nothrow_move_constructible<ThrowingMover>::value);
    CHECK_FALSE(std::is_nothrow_move_assignable<Optional<ThrowingMover>>::value);
}

TEST_CASE("DR31 common nothrow move-assign path is preserved") {
    Result<int> a = Result<int>::Ok(1);
    Result<int> b = Result<int>::Ok(2);
    a = std::move(b);
    CHECK(a.has_value());
    CHECK(a.value() == 2);

    Result<int> e = Result<int>::Err("boom");
    a = std::move(e);
    CHECK_FALSE(a.has_value());
    CHECK(a.error() == "boom");
}
