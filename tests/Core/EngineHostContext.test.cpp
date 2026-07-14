// EngineHostContext — landing seam for the Unreal host-context JSON snapshot.
// Process-global state, so each case sets-then-asserts and uses RELATIVE revision
// deltas (the global revision is monotonic across the whole test binary).

#include <doctest/doctest.h>

#include "Diagnostics/EngineHostContext.h"

#include <string>

using smatchet::hostctx::GetSnapshotJson;
using smatchet::hostctx::Revision;
using smatchet::hostctx::SetSnapshotJson;

TEST_CASE("An accepted snapshot is stored and bumps the revision") {
    const std::uint64_t before = Revision();
    SetSnapshotJson(R"({"engine":"5.6","map":"Main"})");
    CHECK(GetSnapshotJson() == R"({"engine":"5.6","map":"Main"})");
    CHECK(Revision() > before);
}

TEST_CASE("An empty snapshot clears the stored value") {
    SetSnapshotJson(R"({"x":1})");
    SetSnapshotJson("");
    CHECK(GetSnapshotJson().empty());
}

TEST_CASE("An oversized snapshot is rejected and leaves state untouched") {
    SetSnapshotJson(R"({"keep":"me"})");
    const std::uint64_t rev = Revision();
    const std::string kept = GetSnapshotJson();

    // > the internal 128 KiB cap.
    const std::string huge(200u * 1024u, 'x');
    SetSnapshotJson(huge);

    CHECK(GetSnapshotJson() == kept); // unchanged
    CHECK(Revision() == rev);         // revision not bumped by a rejected push
}

TEST_CASE("A payload exactly at the 128 KiB cap is accepted") {
    const std::string atCap(128u * 1024u, 'a');
    const std::uint64_t before = Revision();
    SetSnapshotJson(atCap);
    CHECK(GetSnapshotJson().size() == 128u * 1024u);
    CHECK(Revision() > before);
}
