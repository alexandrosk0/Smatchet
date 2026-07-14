#include <doctest/doctest.h>

#include "SmatchetResult.h"

#include <stdexcept>
#include <string>
#include <utility>

// Wrong-state access (value() on disengaged / error() on ok) is a two-stage
// Pillar-3 contract in SmatchetResult.h: assert() in DEBUG, then throw
// std::logic_error in RELEASE (NDEBUG). doctest's CHECK_THROWS_AS can only
// observe the *throw* — under a live assert() (Debug builds, e.g. the
// ninja-msvc-asan preset which compiles -DCMAKE_BUILD_TYPE=Debug WITHOUT
// NDEBUG) the assert's abort() fires FIRST and SIGABRTs the run before any
// exception unwinds. This is correct designed behaviour, not an ASan finding —
// so the throw-path death-tests are gated to NDEBUG builds (where every other
// test preset runs: ninja-test-* / ninja-debug-* use RelWithDebInfo/Release →
// /DNDEBUG). The #1017 copy-assignment death-tests below are NOT gated: they
// throw from a user copy ctor (real exception, no assert), so they exercise the
// UB-prone path genuinely under ASan in both Debug and Release.
#ifdef NDEBUG
#define SMATCHET_CHECK_WRONGSTATE_THROWS(expr, exc) CHECK_THROWS_AS(expr, exc)
#else
#define SMATCHET_CHECK_WRONGSTATE_THROWS(expr, exc) ((void)0) // assert() aborts in Debug
#endif

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
    SMATCHET_CHECK_WRONGSTATE_THROWS(empty.value(), std::logic_error);

    const Optional<int> constEmpty;
    SMATCHET_CHECK_WRONGSTATE_THROWS(constEmpty.value(), std::logic_error);
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
    SMATCHET_CHECK_WRONGSTATE_THROWS(ok.error(), std::logic_error);

    Result<int> err = Result<int>::Err("nope");
    SMATCHET_CHECK_WRONGSTATE_THROWS(err.value(), std::logic_error);
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

// --- #1017: Result copy-assignment exception-safety (the reachable construct-then-throw UB) ---
namespace {

// A payload whose COPY ctor throws on demand but whose MOVE ctor is noexcept — exactly the
// shape of Result<vector/string/json>: a copy can bad_alloc, a move never throws. liveCount
// balances ctor/dtor so a double-destroy (the old Destroy()-then-throwing-copy bug) shows up as
// a non-zero residual after scope exit (and ASan/UBSan flags the actual UB in the sanitizer lane).
struct ThrowOnCopy {
    int id;
    bool armed; // when copied while armed, the copy throws
    static int liveCount;
    ThrowOnCopy(int i, bool arm) : id(i), armed(arm) { ++liveCount; }
    ThrowOnCopy(const ThrowOnCopy& o) : id(o.id), armed(o.armed) {
        if (o.armed) {
            throw std::runtime_error("ThrowOnCopy: copy boom");
        }
        ++liveCount; // only a *successful* copy creates a live object
    }
    ThrowOnCopy(ThrowOnCopy&& o) noexcept : id(o.id), armed(o.armed) {
        ++liveCount;
        o.id = -1;
    }
    ~ThrowOnCopy() { --liveCount; }
};
int ThrowOnCopy::liveCount = 0;

} // namespace

TEST_CASE("Result: copy-assignment is exception-safe when the copy throws (#1017)") {
    ThrowOnCopy::liveCount = 0;
    {
        // target holds id=1 (unarmed); source holds id=2 (armed → its copy throws).
        Result<ThrowOnCopy, std::string> target = Result<ThrowOnCopy, std::string>::Ok(ThrowOnCopy(1, false));
        Result<ThrowOnCopy, std::string> source = Result<ThrowOnCopy, std::string>::Ok(ThrowOnCopy(2, true));

        // The copy in `target = source` throws. Pre-fix this Destroy()'d target FIRST, so the
        // throw left target's storage destroyed-but-ok_ → ~target double-destroyed = UB.
        CHECK_THROWS_AS(target = source, std::runtime_error);

        // Strong guarantee: target is untouched — still its original ok-value.
        CHECK(target.has_value());
        CHECK(target.value().id == 1);
    }
    // Both Results destructed once each; the failed copy left no live object. A double-destroy
    // (the old bug) would drive this negative.
    CHECK(ThrowOnCopy::liveCount == 0);
}

TEST_CASE("Result: copy-assignment onto an error state stays exception-safe (#1017)") {
    ThrowOnCopy::liveCount = 0;
    {
        Result<ThrowOnCopy, std::string> target = Result<ThrowOnCopy, std::string>::Err("orig-err");
        Result<ThrowOnCopy, std::string> source = Result<ThrowOnCopy, std::string>::Ok(ThrowOnCopy(7, true));
        CHECK_THROWS_AS(target = source, std::runtime_error);
        // target unchanged — still the original error.
        CHECK_FALSE(target.has_value());
        CHECK(target.error() == "orig-err");
    }
    CHECK(ThrowOnCopy::liveCount == 0);
}

// --- VoidResult / Unit — the no-payload Result shape (#21 groundwork) ------------

TEST_CASE("VoidResult: VoidOk() is a success with no payload") {
    const VoidResult r = VoidOk();
    CHECK(r.has_value());
    CHECK(static_cast<bool>(r));
    (void)r.value(); // Unit — nothing to inspect, but must not assert/throw on the ok path
}

TEST_CASE("VoidResult: Err carries the failure reason") {
    const VoidResult r = VoidResult::Err("nope");
    CHECK_FALSE(r.has_value());
    CHECK_FALSE(static_cast<bool>(r));
    CHECK(r.error() == "nope");
}

TEST_CASE("VoidResult: copy and move preserve the ok/err state") {
    VoidResult ok = VoidOk();
    const VoidResult okCopy = ok;
    CHECK(okCopy.has_value());
    const VoidResult okMove = std::move(ok);
    CHECK(okMove.has_value());

    VoidResult err = VoidResult::Err("boom");
    const VoidResult errCopy = err;
    CHECK_FALSE(errCopy.has_value());
    CHECK(errCopy.error() == "boom");
    const VoidResult errMove = std::move(err);
    CHECK_FALSE(errMove.has_value());
    CHECK(errMove.error() == "boom");
}

TEST_CASE("VoidResult: reading error() on a success VoidResult is the wrong-state contract") {
    const VoidResult r = VoidOk();
    SMATCHET_CHECK_WRONGSTATE_THROWS(r.error(), std::logic_error);
}
