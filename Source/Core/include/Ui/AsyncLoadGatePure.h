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

#pragma once

#include <string>

namespace smatchet {
namespace async_load {

/// Kick gate for a cache-keyed load — the plan-doc viewer's document read, where the key
/// is the selected file path and `loadedKey` is the path matching the cached body.
///
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
///
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
///
/// An empty `resultKey` is never current, even against an empty `currentKey` — both kick
/// gates above refuse an empty key, so an empty result key can only be a defaulted or
/// corrupted payload, and "nothing selected" is not something to apply a body to.
inline bool ResultIsCurrent(const std::string& resultKey, const std::string& currentKey) {
    return !resultKey.empty() && resultKey == currentKey;
}

} // namespace async_load
} // namespace smatchet
