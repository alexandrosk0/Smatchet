#pragma once

// Pure, backend-agnostic predicate behind the mobile #1122 dynamic-texture guard.
//
// ImGui 1.92 owns texture lifetime through ImTextureData. Every per-frame ImDrawCmd carries a
// TexRef that resolves to an ImTextureData; the renderer backend (here imgui_impl_opengl3) runs a
// per-texture upload/destroy pass over ImDrawData::Textures BEFORE it walks the draw commands, then
// ImDrawCmd::GetTexID() asserts (imgui.h, "Backend must call ImTextureData::SetTexID()...") if the
// command's texture still has an Invalid backend TexID at that point. A mid-frame dynamic-atlas grow
// or font re-apply can leave a command pointing at a texture that the upload pass will NOT make
// valid this frame — that orphan is the #1122 SIGABRT.
//
// This header isolates the "will the upload pass leave this texture's TexID Invalid?" decision into
// a value-only function so it can be unit-tested without ImGui / a GL context. android_main.cpp
// static_asserts the constants below against the real ImTextureStatus_* enum so the mapping cannot
// drift from upstream.

namespace smatchet {
namespace ui {

// Mirror of the ImGui 1.92 ImTextureStatus enumerators this predicate reasons about. Kept as plain
// ints (not the ImGui enum) so the header stays dependency-free; android_main.cpp pins them to the
// real values with static_assert.
enum SmatchetTextureGuardStatus {
    kTextureGuardStatus_OK = 0,
    kTextureGuardStatus_Destroyed = 1,
    kTextureGuardStatus_WantCreate = 2,
    kTextureGuardStatus_WantUpdates = 3,
    kTextureGuardStatus_WantDestroy = 4,
};

// Returns true when a committed draw command pointing at this texture WILL still resolve an Invalid
// backend TexID once the renderer's pre-command upload pass has run — i.e. the command is doomed and
// must be repointed at the live atlas to avoid the GetTexID() assert.
//
//   texIdInvalid == false            -> backend already has a valid id           -> safe.
//   WantCreate && hasPixels          -> upload pass creates it THIS frame         -> safe.
//   WantCreate && !hasPixels         -> pixels were freed, upload pass can't run  -> DOOMED.
//   WantDestroy / Destroyed          -> pass destroys it / never (re)creates it   -> DOOMED.
//   any other status while Invalid   -> no upload path makes it valid this frame  -> DOOMED.
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
