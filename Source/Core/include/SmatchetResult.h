#pragma once

// Project-local Optional<T> + Result<T, E> value types. C++14 hard (Unreal compat)
// bans std::optional / std::variant, so these provide the same "engaged-or-empty" and
// "ok-or-error" shapes over manual aligned-storage with explicit placement-new / dtor.
// Lives in the global namespace to match the sibling Tracker/TrackerError.h precedent
// (also global, no namespace). Header-only — no .cpp.
// Pillar 3 (never crash): value() / error() on the wrong state assert() in debug then
// throw std::logic_error in release. Never silent undefined behaviour.

#include <cassert>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// Optional<T> — holds either nothing or one engaged T. Default-constructs empty.
template <typename T> class Optional {
  public:
    Optional() noexcept : engaged_(false) {}

    Optional(const T& value) : engaged_(false) { Construct(value); }
    Optional(T&& value) : engaged_(false) { Construct(std::move(value)); }

    Optional(const Optional& other) : engaged_(false) {
        if (other.engaged_) {
            Construct(*other.Ptr());
        }
    }

    // Move ctor — steals the engaged value and leaves the source disengaged.
    Optional(Optional&& other) : engaged_(false) {
        if (other.engaged_) {
            Construct(std::move(*other.Ptr()));
            other.reset();
        }
    }

    // Copy-and-swap assignment — handles copy and move via the by-value parameter.
    // noexcept is CONDITIONAL: swap() placement-new-move-constructs T in the
    // engaged/disengaged branches, so an unconditional noexcept would std::terminate
    // if T's move throws (DR31). Advertise noexcept only when T's move cannot throw,
    // matching the common (nothrow) case; a throwing-move T propagates normally.
    Optional& operator=(Optional other) noexcept(std::is_nothrow_move_constructible<T>::value &&
                                                 std::is_nothrow_move_assignable<T>::value) {
        swap(other);
        return *this;
    }

    ~Optional() { reset(); }

    bool has_value() const noexcept { return engaged_; }
    explicit operator bool() const noexcept { return engaged_; }

    T& value() & {
        assert(engaged_ && "Optional::value() on a disengaged Optional");
        if (!engaged_) {
            throw std::logic_error("Optional::value() called on a disengaged Optional");
        }
        return *Ptr();
    }

    const T& value() const& {
        assert(engaged_ && "Optional::value() on a disengaged Optional");
        if (!engaged_) {
            throw std::logic_error("Optional::value() called on a disengaged Optional");
        }
        return *Ptr();
    }

    // Returns the engaged value, or fallback when empty. Never throws.
    T value_or(T fallback) const { return engaged_ ? *Ptr() : std::move(fallback); }

    void reset() noexcept {
        if (engaged_) {
            Ptr()->~T();
            engaged_ = false;
        }
    }

    void swap(Optional& other) {
        if (engaged_ && other.engaged_) {
            using std::swap;
            swap(*Ptr(), *other.Ptr());
        } else if (engaged_ && !other.engaged_) {
            other.Construct(std::move(*Ptr()));
            reset();
        } else if (!engaged_ && other.engaged_) {
            Construct(std::move(*other.Ptr()));
            other.reset();
        }
    }

  private:
    T* Ptr() noexcept { return reinterpret_cast<T*>(&storage_); }
    const T* Ptr() const noexcept { return reinterpret_cast<const T*>(&storage_); }

    template <typename U> void Construct(U&& fromValue) {
        // placement-new into in-object storage — no heap allocation
        ::new (static_cast<void*>(&storage_)) T(std::forward<U>(fromValue));
        engaged_ = true;
    }

    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
    bool engaged_;
};

template <typename T> void swap(Optional<T>& a, Optional<T>& b) { a.swap(b); }

// Result<T, E> — holds either an ok-value T or an error E, never both. A tagged union
// over manual aligned storage (no std::variant). Construct via Ok() / Err().
template <typename T, typename E = std::string> class Result {
  public:
    static Result Ok(T value) {
        Result r;
        r.ConstructValue(std::move(value));
        return r;
    }

    static Result Err(E error) {
        Result r;
        r.ConstructError(std::move(error));
        return r;
    }

    Result(const Result& other) : ok_(other.ok_), valueless_(true) {
        if (other.valueless_) {
            return; // source's storage is empty (a throwing move-assign left it so) — stay valueless
        }
        if (ok_) {
            // placement-new into in-object storage (copying T may itself allocate, e.g. vector/json)
            ::new (static_cast<void*>(&storage_)) T(*other.ValuePtr());
        } else {
            // placement-new into in-object storage (copying E may itself allocate)
            ::new (static_cast<void*>(&storage_)) E(*other.ErrorPtr());
        }
        valueless_ = false; // storage now holds a live member — dtor may destroy it
    }

    Result(Result&& other) : ok_(other.ok_), valueless_(true) {
        if (other.valueless_) {
            return; // source's storage is empty (a throwing move-assign left it so) — stay valueless
        }
        if (ok_) {
            // placement-new into in-object storage — no heap allocation
            ::new (static_cast<void*>(&storage_)) T(std::move(*other.ValuePtr()));
        } else {
            // placement-new into in-object storage — no heap allocation
            ::new (static_cast<void*>(&storage_)) E(std::move(*other.ErrorPtr()));
        }
        valueless_ = false; // storage now holds a live member — dtor may destroy it
    }

    Result& operator=(const Result& other) {
        // Construct-then-commit (strong exception-safety; mirrors Optional's copy-and-swap).
        // The old code Destroy()'d FIRST, then placement-new'd a copy of T/E — but that copy can
        // throw (bad_alloc copying a Result<vector/string/json/...>, the tracker layer's heaviest
        // Result types), leaving storage destroyed yet ok_ set → the next ~Result double-destroys
        // unconstructed storage = UB (#1017). Instead: build the copy in a local FIRST (a throw
        // there leaves *this untouched), then Destroy() and commit via a noexcept move.
        static_assert(std::is_nothrow_move_constructible<T>::value && std::is_nothrow_move_constructible<E>::value,
                      "Result copy-assignment commits via a noexcept move; T and E must be "
                      "nothrow-move-constructible (vector/string/json/TrackerError all are).");
        if (this != &other) {
            if (other.valueless_) {
                Destroy(); // propagate the source's empty state instead of reading dead storage
                ok_ = other.ok_;
                valueless_ = true;
                return *this;
            }
            if (other.ok_) {
                T tmp(*other.ValuePtr()); // may throw — *this still intact (strong guarantee)
                Destroy();
                valueless_ = true; // storage empty until the noexcept commit lands
                ::new (static_cast<void*>(&storage_)) T(std::move(tmp)); // noexcept commit
                ok_ = true;
                valueless_ = false;
            } else {
                E tmp(*other.ErrorPtr()); // may throw — *this still intact
                Destroy();
                valueless_ = true; // storage empty until the noexcept commit lands
                ::new (static_cast<void*>(&storage_)) E(std::move(tmp)); // noexcept commit
                ok_ = false;
                valueless_ = false;
            }
        }
        return *this;
    }

    // Move-assignment (DR31). A move can throw for payloads whose move is not
    // noexcept, so ordering matters: Destroy() first, then mark the storage
    // valueless_ BEFORE the placement-new that may throw. If it throws, the
    // storage stays empty, ok_ is left stale but unread, and ~Result / Destroy()
    // skip the unconstructed storage instead of double-destroying it. On success
    // ok_ is committed and valueless_ cleared. For nothrow-move payloads (the
    // common case: vector/string/json/TrackerError) this window never triggers.
    Result& operator=(Result&& other) {
        if (this != &other) {
            Destroy();
            valueless_ = true;
            if (other.valueless_) {
                ok_ = other.ok_; // source empty — leave *this valueless too, no dereference
                return *this;
            }
            if (other.ok_) {
                // placement-new into in-object storage — no heap allocation
                ::new (static_cast<void*>(&storage_)) T(std::move(*other.ValuePtr()));
            } else {
                // placement-new into in-object storage — no heap allocation
                ::new (static_cast<void*>(&storage_)) E(std::move(*other.ErrorPtr()));
            }
            ok_ = other.ok_;
            valueless_ = false;
        }
        return *this;
    }

    ~Result() { Destroy(); }

    // valueless_ (a throwing move-assign left storage empty, DR31) reports as no-value.
    bool has_value() const noexcept { return ok_ && !valueless_; }
    explicit operator bool() const noexcept { return ok_ && !valueless_; }

    T& value() & {
        assert(has_value() && "Result::value() on an error-state Result");
        if (!has_value()) {
            throw std::logic_error("Result::value() called on an error-state Result");
        }
        return *ValuePtr();
    }

    const T& value() const& {
        assert(has_value() && "Result::value() on an error-state Result");
        if (!has_value()) {
            throw std::logic_error("Result::value() called on an error-state Result");
        }
        return *ValuePtr();
    }

    E& error() & {
        assert(!ok_ && !valueless_ && "Result::error() on an ok-state Result");
        if (ok_ || valueless_) {
            throw std::logic_error("Result::error() called on an ok-state Result");
        }
        return *ErrorPtr();
    }

    const E& error() const& {
        assert(!ok_ && !valueless_ && "Result::error() on an ok-state Result");
        if (ok_ || valueless_) {
            throw std::logic_error("Result::error() called on an ok-state Result");
        }
        return *ErrorPtr();
    }

    // Returns the ok-value, or fallback when in the error state. Never throws.
    T value_or(T fallback) const { return has_value() ? *ValuePtr() : std::move(fallback); }

  private:
    Result() noexcept : ok_(false), valueless_(true) {}

    T* ValuePtr() noexcept { return reinterpret_cast<T*>(&storage_); }
    const T* ValuePtr() const noexcept { return reinterpret_cast<const T*>(&storage_); }
    E* ErrorPtr() noexcept { return reinterpret_cast<E*>(&storage_); }
    const E* ErrorPtr() const noexcept { return reinterpret_cast<const E*>(&storage_); }

    void ConstructValue(T value) {
        // placement-new into in-object storage — no heap allocation
        ::new (static_cast<void*>(&storage_)) T(std::move(value));
        ok_ = true;
        valueless_ = false;
    }

    void ConstructError(E error) {
        // placement-new into in-object storage — no heap allocation
        ::new (static_cast<void*>(&storage_)) E(std::move(error));
        ok_ = false;
        valueless_ = false;
    }

    void Destroy() noexcept {
        if (valueless_) {
            return; // storage holds no live member (post-throw window) — nothing to destroy
        }
        if (ok_) {
            ValuePtr()->~T();
        } else {
            ErrorPtr()->~E();
        }
    }

    typename std::aligned_union<0, T, E>::type storage_;
    bool ok_;
    // True while storage_ holds no live member: pre-construction and after a
    // throwing move/commit. Guards Destroy() against destroying raw storage.
    bool valueless_;
};

// Unit / VoidResult — the no-payload Result shape for operations that
// succeed-or-fail with nothing to return (the `bool + std::string& outError`
// pattern). C++14-hard (no std::variant) makes a clean `Result<void>` partial
// specialisation more trouble than it's worth, so "no payload" is modelled as an
// empty `Unit` value instead: `VoidResult` is just `Result<Unit>`. Use `VoidOk()`
// on success and `VoidResult::Err(reason)` on failure, then branch on
// `has_value()` / read `error()` exactly like any other Result. `Unit` is empty,
// trivially-copyable and nothrow-movable, so it satisfies Result's
// nothrow-move requirement with zero storage overhead.
struct Unit {};

using VoidResult = Result<Unit>;

// Convenience: a success VoidResult without spelling `VoidResult::Ok(Unit{})`.
inline VoidResult VoidOk() { return VoidResult::Ok(Unit{}); }

// Unpack a Result<T> into the "async worker → main-thread post-back" capture shape the UI
// workers share: set `ok`, MOVE the payload into `value` on success, or copy the message into
// `err` on failure. The other branch's out-param is left untouched, so pre-seed `value`/`err`
// to empty/default before calling. Consolidates the has_value()/value()/error() unpack so the
// idiom lives in one place instead of being copy-pasted across worker lambdas (DRY Pillar 5).
template <typename T> inline void UnpackResult(Result<T>&& r, bool& ok, T& value, std::string& err) {
    ok = r.has_value();
    if (ok) {
        value = std::move(r.value());
    } else {
        err = r.error();
    }
}
