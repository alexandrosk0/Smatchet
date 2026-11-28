// Pure rules of the user-texture destroy state machine (Persistence/UserTextureDestroyPolicy.h).
// ImGui-free: the simulated backend below mirrors the two shipped renderer gates
//   OpenGL3: free when Status == WantDestroy && UnusedFrames > 0
//   DX12:    free when Status == WantDestroy && UnusedFrames >= numFramesInFlight
// so the walk proves a queued texture reaches Destroyed (and FreeNow) for every frames-in-flight value.

#include <doctest/doctest.h>

#include "Persistence/UserTextureDestroyPolicy.h"

using smatchet::persistence::kUserTextureStatus_Destroyed;
using smatchet::persistence::kUserTextureStatus_OK;
using smatchet::persistence::kUserTextureStatus_WantCreate;
using smatchet::persistence::kUserTextureStatus_WantDestroy;
using smatchet::persistence::kUserTextureStatus_WantUpdates;
using smatchet::persistence::NextTextureDestroyStep;
using smatchet::persistence::TextureDestroyAction;
using smatchet::persistence::TextureDestroyStep;

namespace {

struct SimTexture {
    int Status = kUserTextureStatus_OK;
    int UnusedFrames = 0;
    bool TexIdInvalid = false;
    bool HasBackendUserData = true;
};

// Returns the number of ticks until FreeNow, or -1 when it does not happen within `maxTicks`.
// `framesInFlight` models the backend's destroy threshold (1 = GL3's `> 0`).
int TicksUntilFree(SimTexture tex, int framesInFlight, int maxTicks) {
    for (int tick = 1; tick <= maxTicks; ++tick) {
        // Application tick (SmatchetImageTextureCache::TickPendingDestroys), once per frame.
        const TextureDestroyStep step =
            NextTextureDestroyStep(tex.Status, tex.TexIdInvalid, tex.HasBackendUserData, tex.UnusedFrames);
        if (step.Action == TextureDestroyAction::FreeNow) {
            return tick;
        }
        tex.Status = step.NewStatus;
        tex.UnusedFrames = step.NewUnusedFrames;
        // Renderer backend pass for the same frame (RenderDrawData after Render).
        if (tex.Status == kUserTextureStatus_WantDestroy && tex.UnusedFrames >= framesInFlight) {
            tex.Status = kUserTextureStatus_Destroyed;
            tex.TexIdInvalid = true;
            tex.HasBackendUserData = false;
        }
    }
    return -1;
}

} // namespace

TEST_CASE("UserTextureDestroyPolicy: a resident texture is promoted to WantDestroy with UnusedFrames 1") {
    const int statuses[] = {kUserTextureStatus_OK, kUserTextureStatus_WantUpdates};
    for (int i = 0; i < 2; ++i) {
        const TextureDestroyStep step = NextTextureDestroyStep(statuses[i], false, true, 0);
        CHECK(step.Action == TextureDestroyAction::PromoteToWantDestroy);
        CHECK(step.NewStatus == kUserTextureStatus_WantDestroy);
        CHECK(step.NewUnusedFrames == 1);
    }
}

TEST_CASE("UserTextureDestroyPolicy: WantDestroy waits and counts frames") {
    const TextureDestroyStep step = NextTextureDestroyStep(kUserTextureStatus_WantDestroy, false, true, 1);
    CHECK(step.Action == TextureDestroyAction::Wait);
    CHECK(step.NewStatus == kUserTextureStatus_WantDestroy);
    CHECK(step.NewUnusedFrames == 2);
}

TEST_CASE("UserTextureDestroyPolicy: the frame counter saturates instead of overflowing") {
    const TextureDestroyStep step = NextTextureDestroyStep(kUserTextureStatus_WantDestroy, false, true, 0x3fffffff);
    CHECK(step.Action == TextureDestroyAction::Wait);
    CHECK(step.NewUnusedFrames == 0x3fffffff);
}

TEST_CASE("UserTextureDestroyPolicy: Destroyed frees at once whatever the other inputs say") {
    CHECK(NextTextureDestroyStep(kUserTextureStatus_Destroyed, true, false, 0).Action == TextureDestroyAction::FreeNow);
    CHECK(NextTextureDestroyStep(kUserTextureStatus_Destroyed, false, true, 5).Action == TextureDestroyAction::FreeNow);
}

TEST_CASE("UserTextureDestroyPolicy: a texture the backend never saw frees on the first tick") {
    CHECK(NextTextureDestroyStep(kUserTextureStatus_WantCreate, true, false, 0).Action ==
          TextureDestroyAction::FreeNow);
    // The same texture already promoted (e.g. a backend re-Init cleared its handles).
    CHECK(NextTextureDestroyStep(kUserTextureStatus_WantDestroy, true, false, 3).Action ==
          TextureDestroyAction::FreeNow);
}

TEST_CASE("UserTextureDestroyPolicy: WantCreate with a backend object still waits for the backend") {
    // TexID set or backend data present: a GPU object exists, so it must go through WantDestroy.
    CHECK(NextTextureDestroyStep(kUserTextureStatus_WantCreate, false, true, 0).Action ==
          TextureDestroyAction::PromoteToWantDestroy);
    CHECK(NextTextureDestroyStep(kUserTextureStatus_WantCreate, true, true, 0).Action ==
          TextureDestroyAction::PromoteToWantDestroy);
    CHECK(NextTextureDestroyStep(kUserTextureStatus_WantCreate, false, false, 0).Action ==
          TextureDestroyAction::PromoteToWantDestroy);
}

TEST_CASE("UserTextureDestroyPolicy: walk to FreeNow for 1, 2 and 3 frames in flight") {
    // GL3 frees in the render of the promote frame; the next tick sees Destroyed.
    CHECK(TicksUntilFree(SimTexture(), 1, 16) == 2);
    // DX12 with 2 frames in flight needs UnusedFrames 2: promote, wait, then Destroyed.
    CHECK(TicksUntilFree(SimTexture(), 2, 16) == 3);
    // DX12 with 3 frames in flight (Unreal backend).
    CHECK(TicksUntilFree(SimTexture(), 3, 16) == 4);
}

TEST_CASE("UserTextureDestroyPolicy: a never-uploaded texture is freed on tick 1 regardless of frames in flight") {
    SimTexture tex;
    tex.Status = kUserTextureStatus_WantCreate;
    tex.TexIdInvalid = true;
    tex.HasBackendUserData = false;
    CHECK(TicksUntilFree(tex, 1, 4) == 1);
    CHECK(TicksUntilFree(tex, 3, 4) == 1);
}

TEST_CASE("UserTextureDestroyPolicy: the step never lowers UnusedFrames") {
    for (int status = kUserTextureStatus_OK; status <= kUserTextureStatus_WantDestroy; ++status) {
        for (int frames = 0; frames < 5; ++frames) {
            const TextureDestroyStep step = NextTextureDestroyStep(status, false, true, frames);
            if (step.Action != TextureDestroyAction::FreeNow) {
                CHECK(step.NewUnusedFrames >= (status == kUserTextureStatus_WantDestroy ? frames : 1));
            }
        }
    }
}
