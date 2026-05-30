// AiAssistantInputSeedDecision — unit gate for the direction-aware re-seed
// decision used by the Smatchet AI Assistant chat-input mirror. The original
// "if (!seeded || diverged) re-seed model -> buf" logic silently clobbered
// between-frame Whisper splices into `s_inputCharBuf`. The fix (now factored
// into `ShouldSeedAssistantInputFromModel`) only re-seeds when the model has
// MORE content than the buf, so buf-only mutations (the dictation splice
// path) survive the next frame's mirror pass.
//
// See Source/Core/include/AiAssistantInputSeedDecision.h for the helper +
// rationale. The TU is gated to compile in both whisper ON/OFF builds since
// the helper has no whisper-specific dependency.

#include <doctest/doctest.h>

#include "AiAssistantInputSeedDecision.h"

#include <initializer_list>

using smatchet::ai::ShouldSeedAssistantInputFromModel;

TEST_CASE("ShouldSeedAssistantInputFromModel: first frame always seeds") {
    // seeded=false → unconditional seed, regardless of sizes (Lua glue may
    // have written into the model field BEFORE the panel ever rendered).
    CHECK(ShouldSeedAssistantInputFromModel(/*seeded=*/false, /*bufLen=*/0,
                                            /*modelSize=*/0, /*bytesDiffer=*/false));
    CHECK(ShouldSeedAssistantInputFromModel(false, 0, 5, true));
    CHECK(ShouldSeedAssistantInputFromModel(false, 5, 0, true));
    CHECK(ShouldSeedAssistantInputFromModel(false, 5, 5, false));
}

TEST_CASE("ShouldSeedAssistantInputFromModel: after seed, model-larger triggers re-seed") {
    // The Lua-poke-model-with-longer-text path. Model grew while panel was
    // closed / inactive; first re-render must adopt the model.
    CHECK(ShouldSeedAssistantInputFromModel(/*seeded=*/true, /*bufLen=*/0,
                                            /*modelSize=*/10, /*bytesDiffer=*/true));
    CHECK(ShouldSeedAssistantInputFromModel(true, 3, 10, true));
}

TEST_CASE("ShouldSeedAssistantInputFromModel: after seed, equal sizes do NOT trigger re-seed") {
    // Steady state — buf and model agree byte-for-byte after the per-frame
    // buf -> model mirror. No work to do.
    CHECK_FALSE(ShouldSeedAssistantInputFromModel(/*seeded=*/true, /*bufLen=*/5,
                                                  /*modelSize=*/5, /*bytesDiffer=*/false));
    // Even if bytesDiffer is true (e.g. mid-frame transient where Lua poked
    // same-length-but-different content), the size-based gate refuses to
    // re-seed — that's the deliberate trade-off the helper documents.
    CHECK_FALSE(ShouldSeedAssistantInputFromModel(true, 5, 5, true));
}

TEST_CASE("ShouldSeedAssistantInputFromModel: after seed, buf-larger does NOT re-seed (the regression)") {
    // THIS is the regression gate. The Whisper dictation router splices N
    // bytes directly into `s_inputCharBuf`, so bufLen = N while modelSize
    // is still the previous frame's value (smaller). The next frame MUST
    // NOT re-seed model -> buf, or the spliced bytes are clobbered before
    // ImGui even draws them.
    CHECK_FALSE(ShouldSeedAssistantInputFromModel(/*seeded=*/true, /*bufLen=*/10,
                                                  /*modelSize=*/0, /*bytesDiffer=*/true));
    CHECK_FALSE(ShouldSeedAssistantInputFromModel(true, 10, 5, true));
    CHECK_FALSE(ShouldSeedAssistantInputFromModel(true, 100, 99, true));
}

TEST_CASE("ShouldSeedAssistantInputFromModel: bytesDiffer flag is irrelevant once seeded") {
    // The decision is purely size-based after the first frame. Asserting
    // this so future maintainers don't smuggle bytesDiffer back in and
    // re-introduce the Whisper-splice clobber bug.
    for (bool bd : {false, true}) {
        CHECK(ShouldSeedAssistantInputFromModel(true, 0, 10, bd));   // model bigger -> seed
        CHECK_FALSE(ShouldSeedAssistantInputFromModel(true, 5, 5, bd));  // equal -> no seed
        CHECK_FALSE(ShouldSeedAssistantInputFromModel(true, 10, 0, bd)); // buf bigger -> no seed
    }
}
