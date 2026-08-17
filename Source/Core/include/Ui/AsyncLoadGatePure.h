// AsyncLoadGatePure.h — pure gating predicates for the keyed single-slot async loads
// introduced by Issue #1925 (Pillar 2: no file I/O on the render thread).
//
// Both async-load sites under Source/Core/src/Ui/ share one shape: at most one read is in
// flight, the worker echoes back the KEY it was asked for, and when the result lands the
// poll must decide whether that payload still matches what the UI is showing. Those two
// decisions are pure string/bool logic — the interesting part of the state machine, and
// the part that silently corrupts the pane if it drifts (a dropped kick leaves a stale
// body on screen; a missing staleness check applies a superseded read).
//
// Extracted here so they are unit-testable without an ImGui frame, a worker pool, or a
// filesystem (tests/Core/AsyncLoadGatePure.test.cpp). Header-only and free of ImGui /
// filesystem includes on purpose: the bucket-A test TU links none of the UI stack.
//
// The two slot helpers at the bottom cover the other half of the same state machine — the
// ORDER in which the in-flight latch is published and cleared around the future. Both throw
// paths (a launch that throws before a future exists, a worker whose exception is rethrown by
// get()) latch the pane dead for the session if the ordering is wrong; they are here, and not
// inline at the call site, so both can be exercised headlessly with a std::promise.

#pragma once

#include <exception>
#include <future>
#include <string>
#include <utility>

namespace smatchet {
namespace async_load {

/// Kick gate for a cache-keyed load — the plan-doc viewer's document read, where the key
/// is the selected file path and `loadedKey` is the path matching the cached body.
/// Kick iff nothing is already in flight, something is selected, and the cached payload is
/// not already the selected key. The in-flight term is what keeps the single future slot
/// safe: move-assigning over a live `std::async` future BLOCKS the render thread until the
/// old task's shared state is released, so a second kick must never overlap the first. A
/// selection made during the in-flight window is not lost — the poll re-runs this gate
/// once the stale result has landed.
inline bool ShouldKickLoad(bool inFlight, const std::string& targetKey, const std::string& loadedKey) {
    if (inFlight) {
        return false;
    }
    if (targetKey.empty()) {
        return false;
    }
    return targetKey != loadedKey;
}

/// Latch gate for a one-shot per-key parse — the attachment preview's image-header read,
/// which runs at most once per downloaded file rather than being re-driven by a selection.
/// `done` is the latch: it is set when a parse LANDS, success or failure, so a failed parse
/// is not retried every frame (and keeps suppressing the thumbnail decode, preserving the
/// ordering the old synchronous parse had). `hasError` short-circuits an entry that already
/// failed upstream — e.g. the download itself errored, so there is no file to read.
inline bool ShouldKickOnce(bool inFlight, bool done, bool hasError, bool keyEmpty) {
    return !inFlight && !done && !hasError && !keyEmpty;
}

/// Staleness check run when a result lands: apply it iff the key the worker echoed back is
/// still the live key. A mismatch means the selection moved (or the entry was re-downloaded
/// to a different path) while the read was in flight, so the payload describes a file the
/// UI is no longer showing and must be dropped.
/// An empty `resultKey` is never current, even against an empty `currentKey` — both kick
/// gates above refuse an empty key, so an empty result key can only be a defaulted or
/// corrupted payload, and "nothing selected" is not something to apply a body to.
inline bool ResultIsCurrent(const std::string& resultKey, const std::string& currentKey) {
    return !resultKey.empty() && resultKey == currentKey;
}

/// Exception-safe launch into the single future slot: the latch and the future are published
/// ONLY once `launcher` has actually returned a future. `std::async` can throw before any
/// future exists (`std::system_error` when threads are exhausted, `std::bad_alloc`), and
/// latching ahead of the launch is unrecoverable — the slot stays invalid, so every later poll
/// early-outs on `!valid()` and nothing ever clears the latch, wedging the pane for the rest of
/// the session (Issue #2056; `LuaConsolePlugin::KickScriptLoad` documents the same hazard).
/// The launch is passed in rather than performed here so the throw path is exercisable without
/// exhausting a real thread pool.
/// Returns false with `outError` set to the exception text when the launch threw; the caller's
/// state is then exactly as it was before the call.
template <typename T, typename Launcher>
bool LaunchIntoSlot(std::future<T>& slot, bool& inFlight, Launcher launcher, std::string& outError) {
    std::future<T> pending;
    try {
        pending = launcher();
    } catch (const std::exception& ex) {
        outError = ex.what();
        return false;
    }
    slot = std::move(pending);
    inFlight = true;
    return true;
}

/// Exception-safe consume of a ready slot: the latch is cleared BEFORE `get()`, because the
/// future is consumed either way. A worker exception is rethrown by `get()`, so clearing after
/// it never runs — the future is spent, the latch stays true, and every later poll early-outs
/// on a load that can never land (Issue #2057; `LuaConsolePlugin::PollScriptLoad` does this in
/// the right order).
/// Returns false with `outError` set to the exception text when the worker threw; `out` is then
/// untouched, but `inFlight` is cleared either way so the caller stays usable.
template <typename T> bool TakeFromSlot(std::future<T>& slot, bool& inFlight, T& out, std::string& outError) {
    inFlight = false;
    try {
        out = slot.get();
    } catch (const std::exception& ex) {
        outError = ex.what();
        return false;
    }
    return true;
}

} // namespace async_load
} // namespace smatchet
