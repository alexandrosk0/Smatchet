#pragma once

// Pure, backend-agnostic predicate behind the mobile #1122 dynamic-texture guard.
// ImGui 1.92 owns texture lifetime via ImTextureData. The GL backend runs a per-texture
// upload/destroy pass over ImDrawData::Textures BEFORE walking draw commands; ImDrawCmd::GetTexID()
// then asserts if a command's texture still has an Invalid backend TexID. A mid-frame atlas grow /
// font re-apply can orphan a command onto a texture the upload pass won't validate this frame — the
// #1122 SIGABRT. This value-only function isolates the "will the upload pass leave TexID Invalid?"
// decision so it is unit-testable without ImGui / a GL context; android_main.cpp static_asserts the
// constants below against the real ImTextureStatus_* enum so the mapping cannot drift.

namespace smatchet {
namespace ui {

// Mirror of the ImGui 1.92 ImTextureStatus enumerators, as plain ints so the header stays
// dependency-free; android_main.cpp pins them to the real values with static_assert.
enum SmatchetTextureGuardStatus {
    kTextureGuardStatus_OK = 0,
    kTextureGuardStatus_Destroyed = 1,
    kTextureGuardStatus_WantCreate = 2,
    kTextureGuardStatus_WantUpdates = 3,
    kTextureGuardStatus_WantDestroy = 4,
};

// Returns true when a committed draw command's texture WILL still resolve an Invalid backend TexID
// once the renderer's pre-command upload pass has run — the command is doomed and must be repointed
// at the live atlas to avoid the GetTexID() assert. Decision (Invalid id assumed):
//   WantCreate && hasPixels        -> upload pass creates it THIS frame         -> safe.
//   WantCreate && !hasPixels       -> pixels freed, upload pass can't run        -> DOOMED.
//   WantDestroy / Destroyed        -> pass destroys / never (re)creates it       -> DOOMED.
//   any other status while Invalid -> no upload path makes it valid this frame   -> DOOMED.
inline bool SmatchetDrawCmdTextureNeedsRebind(int status, bool hasPixels, bool texIdInvalid) {
    if (!texIdInvalid) {
        return false;
    }
    if (status == kTextureGuardStatus_WantCreate && hasPixels) {
        return false;
    }
    return true;
}

} // namespace ui
} // namespace smatchet
