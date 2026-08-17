// WindowExpand.test.cpp — bucket-A coverage of the pure per-window expand core
// (SmatchetWindowExpand_detail.cpp), the state machine behind the Expand /
// Minimize control that sits left of every secondary window's close X:
//
//   * expand stores the pre-expand placement; minimize arms the one-frame replay
//     that puts the window back exactly where it was.
//   * expanding a SECOND window minimizes the first — each keeps its own saved
//     slot, so both land back where they started.
//   * ConsumeRestore is consume-once (the replay must not repeat every frame).
//   * a minimize+expand within one frame keeps the ORIGINAL home rect — the
//     placement captured while a restore is still armed is the fullscreen
//     override, not a slot worth returning to.
//   * SelfHeal drops an expansion whose window stopped submitting (closed while
//     expanded), so nothing stays pinned to a window that no longer draws.
//
// Pure data-level tests — no ImGui, no UiDrawSession, no UI loop.

#include "SmatchetWindowExpand.h"

#include <doctest/doctest.h>

#include <algorithm>

namespace {

using SmatchetWindowExpand::WindowExpandSaved;
using SmatchetWindowExpand::WindowExpandState;
namespace detail = SmatchetWindowExpand::detail;

WindowExpandSaved MakePlacement(unsigned int dockId, float x, float y, float w, float h) {
    WindowExpandSaved saved;
    saved.DockId = dockId;
    saved.PosX = x;
    saved.PosY = y;
    saved.SizeX = w;
    saved.SizeY = h;
    return saved;
}

bool RestoreArmed(const WindowExpandState& s, unsigned int id) {
    return std::find(s.RestorePending.begin(), s.RestorePending.end(), id) != s.RestorePending.end();
}

const unsigned int kWindowA = 0x1111u;
const unsigned int kWindowB = 0x2222u;
const unsigned int kDockNode = 0x00c0ffeeu;
const unsigned int kOtherNode = 0x00badf00u;

} // namespace

TEST_CASE("WindowExpand: expanding stores the pre-expand placement and focuses once") {
    WindowExpandState s;
    detail::ApplyToggle(s, kWindowA, MakePlacement(kDockNode, 10.0f, 20.0f, 300.0f, 400.0f));

    CHECK(s.ExpandedId == kWindowA);
    CHECK(s.FocusOnExpand);
    REQUIRE(s.Saved.count(kWindowA) == 1);
    CHECK(s.Saved[kWindowA].DockId == kDockNode);
    CHECK(s.Saved[kWindowA].SizeX == doctest::Approx(300.0f));
    // Expanding must not arm its own restore — that only happens on minimize.
    CHECK_FALSE(RestoreArmed(s, kWindowA));
}

TEST_CASE("WindowExpand: minimize arms the restore and replays the saved placement once") {
    WindowExpandState s;
    detail::ApplyToggle(s, kWindowA, MakePlacement(kDockNode, 10.0f, 20.0f, 300.0f, 400.0f));
    detail::ApplyToggle(s, kWindowA, MakePlacement(0u, 0.0f, 0.0f, 0.0f, 0.0f));

    CHECK(s.ExpandedId == 0u);
    CHECK_FALSE(s.FocusOnExpand);
    REQUIRE(RestoreArmed(s, kWindowA));

    WindowExpandSaved out;
    REQUIRE(detail::ConsumeRestore(s, kWindowA, out));
    CHECK(out.DockId == kDockNode);
    CHECK(out.PosX == doctest::Approx(10.0f));
    CHECK(out.PosY == doctest::Approx(20.0f));
    CHECK(out.SizeX == doctest::Approx(300.0f));
    CHECK(out.SizeY == doctest::Approx(400.0f));

    // Consume-once: a second frame must not re-pin the window to the replayed rect.
    CHECK_FALSE(detail::ConsumeRestore(s, kWindowA, out));
    CHECK(s.Saved.count(kWindowA) == 0);
    CHECK(s.RestorePending.empty());
}

TEST_CASE("WindowExpand: a floating window round-trips through DockId 0") {
    WindowExpandState s;
    detail::ApplyToggle(s, kWindowA, MakePlacement(0u, 64.0f, 96.0f, 640.0f, 480.0f));
    detail::ApplyToggle(s, kWindowA, MakePlacement(0u, 0.0f, 0.0f, 0.0f, 0.0f));

    WindowExpandSaved out;
    REQUIRE(detail::ConsumeRestore(s, kWindowA, out));
    CHECK(out.DockId == 0u);
    CHECK(out.PosX == doctest::Approx(64.0f));
    CHECK(out.SizeY == doctest::Approx(480.0f));
}

TEST_CASE("WindowExpand: expanding a second window minimizes the first into its own slot") {
    WindowExpandState s;
    detail::ApplyToggle(s, kWindowA, MakePlacement(kDockNode, 10.0f, 20.0f, 300.0f, 400.0f));
    detail::ApplyToggle(s, kWindowB, MakePlacement(0u, 55.0f, 66.0f, 700.0f, 500.0f));

    CHECK(s.ExpandedId == kWindowB);
    CHECK(RestoreArmed(s, kWindowA));
    CHECK_FALSE(RestoreArmed(s, kWindowB));

    WindowExpandSaved out;
    REQUIRE(detail::ConsumeRestore(s, kWindowA, out));
    CHECK(out.DockId == kDockNode);
    CHECK(out.SizeX == doctest::Approx(300.0f));
    // B is still expanded, so its slot survives untouched.
    REQUIRE(s.Saved.count(kWindowB) == 1);
    CHECK(s.Saved[kWindowB].PosX == doctest::Approx(55.0f));
}

TEST_CASE("WindowExpand: ConsumeRestore ignores an id with nothing armed") {
    WindowExpandState s;
    WindowExpandSaved out;
    CHECK_FALSE(detail::ConsumeRestore(s, kWindowA, out));

    // Armed but with no stored slot (self-heal on a window that never finished
    // expanding): the latch is still dropped so it cannot leak into later frames.
    s.RestorePending.push_back(kWindowA);
    CHECK_FALSE(detail::ConsumeRestore(s, kWindowA, out));
    CHECK(s.RestorePending.empty());
}

TEST_CASE("WindowExpand: a zero id is inert") {
    WindowExpandState s;
    detail::ApplyToggle(s, 0u, MakePlacement(kDockNode, 1.0f, 2.0f, 3.0f, 4.0f));
    CHECK(s.ExpandedId == 0u);
    CHECK(s.Saved.empty());
    CHECK(s.RestorePending.empty());
}

TEST_CASE("WindowExpand: SelfHeal drops an expansion whose window stopped submitting") {
    WindowExpandState s;
    detail::ApplyToggle(s, kWindowA, MakePlacement(kDockNode, 10.0f, 20.0f, 300.0f, 400.0f));

    // Frame 1: the window submits — the expansion survives.
    s.SubmittedIds.push_back(kWindowA);
    detail::SelfHeal(s);
    CHECK(s.ExpandedId == kWindowA);
    CHECK(s.SubmittedIds.empty());
    CHECK_FALSE(RestoreArmed(s, kWindowA));

    // Frame 2: it was closed while expanded — drop the pin and arm the restore so
    // reopening puts it back in its old dock slot rather than fullscreen.
    detail::SelfHeal(s);
    CHECK(s.ExpandedId == 0u);
    CHECK_FALSE(s.FocusOnExpand);
    REQUIRE(RestoreArmed(s, kWindowA));

    WindowExpandSaved out;
    REQUIRE(detail::ConsumeRestore(s, kWindowA, out));
    CHECK(out.DockId == kDockNode);
}

TEST_CASE("WindowExpand: SelfHeal with nothing expanded only clears the submission list") {
    WindowExpandState s;
    s.SubmittedIds.push_back(kWindowA);
    s.SubmittedIds.push_back(kWindowB);
    detail::SelfHeal(s);
    CHECK(s.ExpandedId == 0u);
    CHECK(s.SubmittedIds.empty());
    CHECK(s.RestorePending.empty());
}

TEST_CASE("WindowExpand: a restore is armed at most once across repeated toggles") {
    WindowExpandState s;
    detail::ApplyToggle(s, kWindowA, MakePlacement(kDockNode, 10.0f, 20.0f, 300.0f, 400.0f));
    detail::ApplyToggle(s, kWindowA, MakePlacement(0u, 0.0f, 0.0f, 0.0f, 0.0f)); // minimize
    // Toggling again before the restore frame ran re-expands; the stale latch must
    // not survive as a duplicate entry that would yank the window back next frame.
    detail::ApplyToggle(s, kWindowA, MakePlacement(kDockNode, 11.0f, 21.0f, 301.0f, 401.0f));
    detail::ApplyToggle(s, kWindowA, MakePlacement(0u, 0.0f, 0.0f, 0.0f, 0.0f)); // minimize again

    CHECK(std::count(s.RestorePending.begin(), s.RestorePending.end(), kWindowA) == 1);
    WindowExpandSaved out;
    REQUIRE(detail::ConsumeRestore(s, kWindowA, out));
    // The FIRST capture is the home. The re-expand happened while the restore was
    // still armed, i.e. while the window was still wearing its fullscreen geometry,
    // so its `current` is the override and must not overwrite the saved slot.
    CHECK(out.PosX == doctest::Approx(10.0f));
    CHECK(out.DockId == kDockNode);
    CHECK_FALSE(detail::ConsumeRestore(s, kWindowA, out));
}

TEST_CASE("WindowExpand: re-expanding before the restore replays keeps the original home") {
    WindowExpandState s;
    detail::ApplyToggle(s, kWindowA, MakePlacement(kDockNode, 10.0f, 20.0f, 300.0f, 400.0f));
    detail::ApplyToggle(s, kWindowA, MakePlacement(0u, 0.0f, 0.0f, 0.0f, 0.0f)); // minimize (armed, not replayed)
    // Same frame, before BeginWindow consumed the latch: a second toggle re-expands.
    // What the UI would capture NOW is the work area, which is exactly what must not
    // be stored — otherwise the next minimize restores fullscreen onto fullscreen.
    const WindowExpandSaved fullscreen = MakePlacement(0u, 0.0f, 24.0f, 1920.0f, 1000.0f);
    detail::ApplyToggle(s, kWindowA, fullscreen);
    CHECK(s.ExpandedId == kWindowA);
    CHECK(s.RestorePending.empty());

    detail::ApplyToggle(s, kWindowA, MakePlacement(0u, 0.0f, 0.0f, 0.0f, 0.0f)); // minimize for real
    WindowExpandSaved out;
    REQUIRE(detail::ConsumeRestore(s, kWindowA, out));
    CHECK(out.DockId == kDockNode);
    CHECK(out.PosX == doctest::Approx(10.0f));
    CHECK(out.SizeX == doctest::Approx(300.0f));
}

TEST_CASE("WindowExpand: a first expand after its restore was consumed records the new home") {
    WindowExpandState s;
    detail::ApplyToggle(s, kWindowA, MakePlacement(kDockNode, 10.0f, 20.0f, 300.0f, 400.0f));
    detail::ApplyToggle(s, kWindowA, MakePlacement(0u, 0.0f, 0.0f, 0.0f, 0.0f)); // minimize
    WindowExpandSaved replayed;
    REQUIRE(detail::ConsumeRestore(s, kWindowA, replayed)); // BeginWindow's replay frame

    // Latch gone, so this capture is a genuine home again — the user may have moved
    // or re-docked the window in between, and the guard must not freeze the old slot.
    detail::ApplyToggle(s, kWindowA, MakePlacement(kOtherNode, 55.0f, 66.0f, 700.0f, 800.0f));
    detail::ApplyToggle(s, kWindowA, MakePlacement(0u, 0.0f, 0.0f, 0.0f, 0.0f));
    WindowExpandSaved out;
    REQUIRE(detail::ConsumeRestore(s, kWindowA, out));
    CHECK(out.DockId == kOtherNode);
    CHECK(out.PosX == doctest::Approx(55.0f));
}

// --- Persisted-placement decision while expanded (issue #2059) ---------------------
// Quitting while a window is expanded used to persist the fullscreen pin (floating +
// work-area sized) into imgui.ini, and the next launch force-redocked the window to its
// DEFAULT slot — silently discarding a customised dock placement. The ImGui half
// (FreezeExpandedSettings) stamps the values below into the window's settings entry and
// then sets NoSavedSettings so the settings writer leaves the entry alone; this is the
// pure decision behind that stamp.

using SmatchetWindowExpand::detail::WindowExpandPersistedPlacement;

TEST_CASE("PersistedPlacementForExpanded: a docked home persists its dock id, never a rect") {
    // A docked window's rect belongs to its node — writing one would fight the dock
    // restore on the next launch, so only the dock id is replayed.
    const WindowExpandSaved saved = MakePlacement(0x0000000Au, 100.0f, 200.0f, 640.0f, 480.0f);
    const WindowExpandPersistedPlacement out = detail::PersistedPlacementForExpanded(saved);
    CHECK(out.DockId == 0x0000000Au);
    CHECK_FALSE(out.OverridePosSize);
}

TEST_CASE("PersistedPlacementForExpanded: a floating home persists its rect and no dock id") {
    const WindowExpandSaved saved = MakePlacement(0u, 100.0f, 200.0f, 640.0f, 480.0f);
    const WindowExpandPersistedPlacement out = detail::PersistedPlacementForExpanded(saved);
    CHECK(out.DockId == 0u);
    CHECK(out.OverridePosSize);
    CHECK(out.PosX == doctest::Approx(100.0f));
    CHECK(out.PosY == doctest::Approx(200.0f));
    CHECK(out.SizeX == doctest::Approx(640.0f));
    CHECK(out.SizeY == doctest::Approx(480.0f));
}

TEST_CASE("PersistedPlacementForExpanded: a degenerate floating size is not written out") {
    // Captured before the window was ever laid out. Keeping whatever size the existing
    // settings entry holds beats writing a zero-area window into the .ini.
    CHECK_FALSE(detail::PersistedPlacementForExpanded(MakePlacement(0u, 10.0f, 20.0f, 0.0f, 480.0f)).OverridePosSize);
    CHECK_FALSE(detail::PersistedPlacementForExpanded(MakePlacement(0u, 10.0f, 20.0f, 640.0f, 0.0f)).OverridePosSize);
    CHECK_FALSE(detail::PersistedPlacementForExpanded(WindowExpandSaved()).OverridePosSize);
}

TEST_CASE("PersistedPlacementForExpanded: the placement stored at expand time is what gets persisted") {
    // End-to-end at the data level: expanding stores the home, and the persisted
    // placement is derived from THAT — never from the fullscreen geometry the window
    // wears while expanded.
    WindowExpandState s;
    const WindowExpandSaved home = MakePlacement(0x00000004u, 0.0f, 0.0f, 320.0f, 900.0f);
    detail::ApplyToggle(s, 42u, home);
    REQUIRE(s.ExpandedId == 42u);
    REQUIRE(s.Saved.find(42u) != s.Saved.end());
    CHECK(detail::PersistedPlacementForExpanded(s.Saved[42u]).DockId == 0x00000004u);
}
