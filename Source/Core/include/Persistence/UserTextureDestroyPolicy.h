#pragma once

// Frame-safe destroy state machine for ImGui user textures, the ImTextureData objects registered with
// RegisterUserTexture. Value-only and ImGui-free, so the Linux doctest subset covers it.
// SmatchetImageTextureCache.cpp pins the status ints below to the real ImTextureStatus enum with a
// static_assert (pattern: Ui/SmatchetImGuiTextureGuard.cpp).
//
// Why the app must drive this: ImGui's own per-frame texture pass (ImFontAtlasUpdateNewFrame) walks
// only the font atlas texture list. A user texture with WantDestroyNextFrame set is never promoted to
// ImTextureStatus_WantDestroy and never gets UnusedFrames incremented, so a backend that gates the
// free on `UnusedFrames > 0` (OpenGL3) or `UnusedFrames >= numFramesInFlight` (DX12) never frees it:
// the GPU texture, the SRV descriptor and the CPU pixels leak, and the texture never reaches
// ImTextureStatus_Destroyed, so the cache can never unregister it. The rules below mirror what ImGui
// does for atlas textures (imgui_draw.cpp, "Update texture status").
//
// Lifecycle driven once per frame by SmatchetImageTextureCache::TickPendingDestroys():
//   1. eviction sets WantDestroyNextFrame only (the texture may still be referenced by the frame
//      being built; the status is left alone so the draw-list assert on WantDestroyNextFrame is the
//      only visible change).
//   2. next tick: status -> WantDestroy and UnusedFrames -> 1 (PromoteToWantDestroy).
//   3. while WantDestroy: UnusedFrames++ each tick (Wait); the backend frees the GPU object once its
//      own threshold is met and sets Destroyed with TexID invalid and no backend user data.
//   4. Destroyed: FreeNow (unregister + delete).
// A texture that never reached the backend (WantCreate, invalid TexID, no backend user data) has no
// GPU object and no in-flight copy: FreeNow on the first tick.

namespace smatchet {
namespace persistence {

/// Mirror of the ImGui 1.92 ImTextureStatus enumerators as plain ints (pinned by a static_assert in
/// SmatchetImageTextureCache.cpp so this header never needs imgui.h).
enum UserTextureStatus {
    kUserTextureStatus_OK = 0,
    kUserTextureStatus_Destroyed = 1,
    kUserTextureStatus_WantCreate = 2,
    kUserTextureStatus_WantUpdates = 3,
    kUserTextureStatus_WantDestroy = 4,
};

enum class TextureDestroyAction {
    Wait,                 ///< Keep the texture queued; apply NewStatus / NewUnusedFrames.
    PromoteToWantDestroy, ///< Apply NewStatus (= WantDestroy) and NewUnusedFrames (= 1); keep queued.
    FreeNow               ///< Unregister and delete; the backend holds nothing for it.
};

struct TextureDestroyStep {
    TextureDestroyAction Action = TextureDestroyAction::Wait;
    int NewStatus = kUserTextureStatus_OK;
    int NewUnusedFrames = 0;
};

/**
 * Decide this tick's step for a texture whose destroy was requested (WantDestroyNextFrame already
 * set). `texIdInvalid` is `tex->TexID == ImTextureID_Invalid`; `hasBackendUserData` is
 * `tex->BackendUserData != nullptr`. Pure; never throws.
 */
inline TextureDestroyStep NextTextureDestroyStep(int status, bool texIdInvalid, bool hasBackendUserData,
                                                 int unusedFrames) {
    TextureDestroyStep step;
    step.NewStatus = status;
    step.NewUnusedFrames = unusedFrames;

    if (status == kUserTextureStatus_Destroyed) {
        step.Action = TextureDestroyAction::FreeNow;
        return step;
    }
    // The backend never created (or already dropped) a GPU object: nothing to wait for.
    const bool backendHoldsNothing = texIdInvalid && !hasBackendUserData;
    if (backendHoldsNothing && (status == kUserTextureStatus_WantCreate || status == kUserTextureStatus_WantDestroy)) {
        step.Action = TextureDestroyAction::FreeNow;
        return step;
    }
    if (status == kUserTextureStatus_WantDestroy) {
        step.Action = TextureDestroyAction::Wait;
        // Saturate: the counter is a plain int the backend only compares; never overflow it.
        step.NewUnusedFrames = unusedFrames < 0x3fffffff ? unusedFrames + 1 : unusedFrames;
        return step;
    }
    // OK / WantUpdates / WantCreate with a live backend object: ask the backend to destroy it.
    step.Action = TextureDestroyAction::PromoteToWantDestroy;
    step.NewStatus = kUserTextureStatus_WantDestroy;
    step.NewUnusedFrames = 1;
    return step;
}

} // namespace persistence
} // namespace smatchet
