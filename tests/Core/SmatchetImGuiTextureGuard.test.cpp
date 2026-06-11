// SmatchetImGuiTextureGuard.test.cpp — mobile #1122 dynamic-texture orphan guard.
//
// Unit-tests the pure SmatchetDrawCmdTextureNeedsRebind predicate that the Android render
// loop uses to decide whether a committed draw command points at a texture the renderer's
// pre-command upload pass will leave with an Invalid backend TexID (the GetTexID() assert /
// SIGABRT precondition). Pure / ImGui-free, so it runs on the Windows doctest rig. Pins the
// crash precondition (#1122 rotate -> dock-tab -> tap-Tickets) status-by-status against the
// ImGui 1.92 ImTextureStatus semantics encoded in the header.

#include "Ui/SmatchetImGuiTextureGuard.h"

#include "doctest/doctest.h"

using smatchet::ui::SmatchetDrawCmdTextureNeedsRebind;
using smatchet::ui::kTextureGuardStatus_OK;
using smatchet::ui::kTextureGuardStatus_Destroyed;
using smatchet::ui::kTextureGuardStatus_WantCreate;
using smatchet::ui::kTextureGuardStatus_WantUpdates;
using smatchet::ui::kTextureGuardStatus_WantDestroy;

TEST_CASE("NeedsRebind: a valid backend TexID is always safe, whatever the status") {
    // texIdInvalid == false short-circuits first: the command already resolves a real id, so
    // GetTexID() cannot assert regardless of the pending upload/update status.
    CHECK_FALSE(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_OK, true, false));
    CHECK_FALSE(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_WantUpdates, true, false));
    CHECK_FALSE(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_WantDestroy, false, false));
    CHECK_FALSE(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_Destroyed, false, false));
}

TEST_CASE("NeedsRebind: WantCreate WITH pixels is benign — upload pass validates it this frame") {
    // The headline benign case the broad predicate over-flagged at boot: a not-yet-uploaded
    // atlas texture still carrying its CPU pixels. The backend's per-texture pass (which runs
    // before the command loop) creates + uploads it the SAME frame, so the id is valid by the
    // time GetTexID() reads it. Repointing it would be a needless one-frame glyph wobble.
    CHECK_FALSE(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_WantCreate, true, true));
}

TEST_CASE("NeedsRebind: WantCreate WITHOUT pixels is doomed — nothing left to upload") {
    // If the CPU pixels were already freed, the upload pass cannot create the texture, so its
    // TexID stays Invalid and GetTexID() asserts. Must rebind.
    CHECK(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_WantCreate, false, true));
}

TEST_CASE("NeedsRebind: WantDestroy / Destroyed while Invalid are doomed (the #1122 orphan)") {
    // The actual crash: a mid-frame atlas grow queues the OLD texture for destroy; the upload
    // pass frees its id (-> Invalid) before the command loop reads it. Both the to-be-destroyed
    // and already-destroyed states leave the id Invalid -> rebind, with or without pixels.
    CHECK(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_WantDestroy, true, true));
    CHECK(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_WantDestroy, false, true));
    CHECK(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_Destroyed, true, true));
    CHECK(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_Destroyed, false, true));
}

TEST_CASE("NeedsRebind: any other Invalid status has no same-frame upload path -> rebind") {
    // OK / WantUpdates while Invalid should not occur in practice (a valid id is implied), but
    // the predicate is defensive: no branch makes them valid this frame, so it rebinds rather
    // than let GetTexID() assert.
    CHECK(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_OK, true, true));
    CHECK(SmatchetDrawCmdTextureNeedsRebind(kTextureGuardStatus_WantUpdates, true, true));
}
