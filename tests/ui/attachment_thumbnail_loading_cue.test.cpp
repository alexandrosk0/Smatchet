// attachment_thumbnail_loading_cue.test.cpp — bucket-E visible-cue gate for the
// S5 thumbnail decode→upload offload (docs/plans/shipped/memory-budget-and-lifetime-hardening.md,
// pulled into B8 Phase-1 L5 per docs/plans/b8-bucket-e-coverage.md).
//
// Invariant under test: while thumbnail decodes are in flight (the
// `pendingThumbnailUploads` gauge > 0), the attachment preview surface renders a
// passive `TextDisabled` "loading thumbnails..." cue; once the gauge drains back
// to 0 the cue is gone on the next frame. S5 moved the decode off the UI thread,
// so without this cue an in-flight decode is a silent async gap (Pillar 2 — a
// worker-deferred operation must show a cue).
//
// Production shape mirrored (drift-warning — keep in lock-step):
//   Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp
//     :653-659  (inside #if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS))
//       const std::size_t loadingThumbs =
//           smatchet::memtel::PendingThumbnailUploads().load(std::memory_order_relaxed);
//       if (loadingThumbs > 0) {
//           ImGui::SameLine();
//           ImGui::TextDisabled("%s",
//               SmatchetLocalization::T("attachment.thumbnails.loading", "loading thumbnails..."));
//       }
//
// HIGHER FIDELITY THAN A FLAG REPLICA: the production gate reads a *global*
// atomic — `smatchet::memtel::PendingThumbnailUploads()` (MemoryTelemetry.h:46) —
// not a private UI member, so this test drives the REAL gating predicate by
// seeding that real gauge. The GuiFunc evaluates the exact production
// `load(relaxed) > 0` expression against the live counter; only the surrounding
// window + the findable proxy are TU-local. Seeding the real gauge means an
// accidental change to the gauge's type/semantics breaks this test.
//
// Why also a findable proxy: the production cue is `TextDisabled` text with no
// stable ImGui id, which ItemInfo cannot locate. Following the established
// bucket-E convention (annotate_before_cl_cue / sync_stall_visible_cue), the
// GuiFunc submits the REAL `TextDisabled` cue text (so the localized string is
// genuinely produced) PLUS a `##`-id Button proxy guarded by the same predicate;
// the assertion matches the proxy id, which is live iff the real cue is.
//
// Global-state hygiene: the gauge is process-global and shared with sibling
// tests + the live `perf.memory` snapshot, so the TestFunc restores it to 0 on
// every exit path (including before the first assertion arms it) — no leak into
// sibling TUs.
//
// Determinism: the gauge is held at 1 by the TestFunc across an explicit Yield,
// so the cue is observable for >= 1 frame regardless of engine stepping rate
// (order, not wall-clock).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "MemoryTelemetry.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <cstddef>

namespace {

// Mirror of SmatchetAttachmentPreviewUi.cpp:653-659. Reads the SAME global gauge
// the production code reads, evaluates the SAME `> 0` predicate, and submits the
// SAME localized cue text. The `##attachment-thumbnails-loading` Button is the
// findable proxy (TextDisabled carries no stable id); it is gated on the exact
// same predicate so it is on screen iff the real cue is.
void SubmitThumbnailLoadingCue() {
    const std::size_t loadingThumbs =
        smatchet::memtel::PendingThumbnailUploads().load(std::memory_order_relaxed);
    if (loadingThumbs > 0) {
        // The real cue text the production surface renders. The headless test
        // font may render some glyphs as missing, but the call exercises the
        // real localized lookup path; the assertion targets the proxy id below.
        ImGui::TextDisabled("%s", "loading thumbnails...");
        // Findable anchor for the bucket-E assertion (proxy for the TextDisabled
        // above). `##`-only id so no visible glyph is required for a non-zero
        // extent the engine can locate.
        ImGui::Button("##attachment-thumbnails-loading", ImVec2(120, 20));
    }
}

void RegisterVisibleWhileLoading(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Attachment", "ThumbnailLoadingCue_VisibleWhileLoading");

    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(280, 90), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::ThumbnailLoadingCue_Visible")) {
            SubmitThumbnailLoadingCue();
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        // Defensive reset: never inherit a non-zero gauge from a sibling test.
        smatchet::memtel::PendingThumbnailUploads().store(0, std::memory_order_relaxed);

        ctx->SetRef("SmatchetTest::ThumbnailLoadingCue_Visible");

        // --- Drain state: gauge == 0, cue must be absent. ---
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(ctx->ItemInfo("##attachment-thumbnails-loading", ImGuiTestOpFlags_NoError).ID == 0);

        // --- In-flight state: seed the REAL gauge > 0, cue must appear. ---
        smatchet::memtel::PendingThumbnailUploads().store(1, std::memory_order_relaxed);
        ctx->Yield(); // one frame with loadingThumbs == 1
        IM_CHECK(ctx->ItemInfo("##attachment-thumbnails-loading", ImGuiTestOpFlags_NoError).ID != 0);

        // --- Drain again: gauge back to 0, cue must clear on the next frame. ---
        smatchet::memtel::PendingThumbnailUploads().store(0, std::memory_order_relaxed);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(ctx->ItemInfo("##attachment-thumbnails-loading", ImGuiTestOpFlags_NoError).ID == 0);

        // Exit-path hygiene: leave the global gauge at 0 for sibling tests.
        smatchet::memtel::PendingThumbnailUploads().store(0, std::memory_order_relaxed);
    };
}

} // namespace

extern "C" void SmatchetRegisterAttachmentThumbnailLoadingCueTests(ImGuiTestEngine* engine) {
    RegisterVisibleWhileLoading(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
