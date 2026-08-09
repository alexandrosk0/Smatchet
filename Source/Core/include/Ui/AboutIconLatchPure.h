#pragma once

// Failure-latch policy for the About-dialog app icon, split out of SmatchetAboutIcon.cpp so the
// rule is testable without ImGui or a live texture backend.
//
// The rule exists because the two failure modes are NOT alike, and treating them alike was a bug:
// the original latched on any failure, reasoning that "decode + upload are deterministic, so if
// they fail once they fail every frame". That holds for the decode — the same baked bytes fail
// identically forever — but not for the upload, which the texture cache refuses while the renderer
// has yet to advertise ImGuiBackendFlags_RendererHasTextures. That is backend state, not a property
// of the icon, so one unlucky early frame bought a glyph-only icon for the whole session.

namespace smatchet {
namespace ui {

/// Which step of the icon load failed.
enum class IconLoadStep {
    Decode,       ///< ICO -> RGBA. Deterministic in the baked bytes.
    Upload,       ///< RGBA -> GPU texture. Depends on live renderer state.
    NoBytesBaked, ///< No icon was compiled into this build. Never changes.
};

/// Per-process latch state. The owning TU keeps one file-static instance.
struct AboutIconLatch {
    bool decodeFailed = false; ///< Permanent: stop attempting for the rest of the session.
    bool uploadWarned = false; ///< Log-once bookkeeping only; the upload stays retryable.
};

/// What the caller should do about a failure.
struct AboutIconFailureAction {
    bool blockFurtherAttempts = false; ///< Latch permanently.
    bool shouldLog = false;            ///< Emit the warning on this occurrence.
};

/// Records a failure and returns the action for it. `Decode` / `NoBytesBaked` latch permanently and log once. `Upload`
/// never latches — a later frame can succeed once the renderer is ready, or after the cache evicts and is re-asked —
/// and logs only on its first occurrence so a permanent upload failure still costs one line, not one per frame on a
/// draw path.
inline AboutIconFailureAction OnAboutIconFailure(IconLoadStep step, AboutIconLatch& latch) {
    AboutIconFailureAction action;
    if (step == IconLoadStep::Upload) {
        action.blockFurtherAttempts = false;
        action.shouldLog = !latch.uploadWarned;
        latch.uploadWarned = true;
        return action;
    }
    action.blockFurtherAttempts = true;
    action.shouldLog = !latch.decodeFailed;
    latch.decodeFailed = true;
    return action;
}

/// True once a permanent failure has been recorded — the caller should stop attempting.
inline bool AboutIconAttemptsBlocked(const AboutIconLatch& latch) { return latch.decodeFailed; }

} // namespace ui
} // namespace smatchet
