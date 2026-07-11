#include "Ui/SmatchetCommentsModalGenPure.h"

#include <doctest/doctest.h>

#include <string>

namespace {

using SmatchetCommentsModalGen::AllocGen;
using SmatchetCommentsModalGen::CallbackIsStale;

// Reproduces the pre-#1713 allocator: a per-open counter reset to 0 then pre-incremented, so
// every open yields the same token (1) and the guard can never tell dispatches apart.
int BuggyPerOpenGen() {
    int perOpen = 0; // reset with the modal state on every open
    return ++perOpen;
}

} // namespace

TEST_CASE("comments-modal Gen is strictly monotonic across dispatches (#1713)") {
    int counter = 0; // the file-static s_genCounter analogue — lives across opens

    const int open1 = AllocGen(counter);
    const int refetchAfterPost = AllocGen(counter);
    const int open2 = AllocGen(counter);

    CHECK(open1 == 1);
    CHECK(refetchAfterPost == 2);
    CHECK(open2 == 3);

    // Distinctness is the whole point: no two dispatches share a token.
    CHECK(open1 != refetchAfterPost);
    CHECK(refetchAfterPost != open2);
    CHECK(open1 != open2);
}

TEST_CASE("the buggy per-open counter collapses every dispatch to 1 (regression witness)") {
    // With the old reset-inside-state design, two 'opens' are indistinguishable.
    CHECK(BuggyPerOpenGen() == 1);
    CHECK(BuggyPerOpenGen() == 1);
    CHECK(BuggyPerOpenGen() == BuggyPerOpenGen());
}

TEST_CASE("stale-guard discards an older-generation post-back, keeps the current one (#1713)") {
    const std::string issue = "PROJ-42";

    // Sequence: open (gen 1, slow fetch A) -> post -> re-fetch (gen 2, fetch B).
    // Current modal generation is now 2 (the re-fetch owns the modal).
    const int current = 2;

    // Fetch B (current gen) applies.
    CHECK_FALSE(CallbackIsStale(/*active=*/true, current, /*dispatch=*/2, issue, issue));
    // Fetch A (the slow original open, gen 1) must be discarded — this is the exact overwrite
    // the bug allowed when both carried gen 1.
    CHECK(CallbackIsStale(/*active=*/true, current, /*dispatch=*/1, issue, issue));
}

TEST_CASE("stale-guard also fires on close and issue-switch") {
    const std::string a = "A-1";
    const std::string b = "B-2";

    // Modal closed -> any post-back is stale.
    CHECK(CallbackIsStale(/*active=*/false, 5, 5, a, a));
    // Same gen but the user switched to a different issue -> stale.
    CHECK(CallbackIsStale(/*active=*/true, 5, 5, b, a));
    // Matching active/gen/issue -> not stale.
    CHECK_FALSE(CallbackIsStale(/*active=*/true, 5, 5, a, a));
}

TEST_CASE("end-to-end: with distinct gens the stale original-open fetch loses the race (#1713)") {
    int counter = 0;
    const std::string issue = "PROJ-7";

    // Open -> gen for the original fetch A.
    const int genFetchA = AllocGen(counter);
    // User posts a comment; on success the modal takes a fresh gen for re-fetch B.
    const int genFetchB = AllocGen(counter);

    // Model "current modal generation" as the latest dispatch (fetch B).
    const int currentGen = genFetchB;

    // Fetch B lands first, applies.
    CHECK_FALSE(CallbackIsStale(true, currentGen, genFetchB, issue, issue));
    // Fetch A (older, slow) lands afterward -> discarded, cannot overwrite the just-posted comment.
    CHECK(CallbackIsStale(true, currentGen, genFetchA, issue, issue));

    // Pre-fix witness: had both fetches shared gen 1 (BuggyPerOpenGen), fetch A would satisfy
    // !CallbackIsStale and clobber fetch B's fresh list.
    const int buggyGen = 1;
    CHECK_FALSE(CallbackIsStale(true, buggyGen, buggyGen, issue, issue));
}
