// AiAssistantUiDetail — unit gate for the pure (ImGui-free) helpers extracted
// from SmatchetAiAssistantUi.cpp during the function-size decomposition. These
// helpers carry the layout-arithmetic + picker-resolution logic that used to be
// inline in the over-cap draw functions; pinning them here keeps the
// behaviour-preserving guarantee testable without an ImGui context.
//
// See Source/Core/include/Ui/SmatchetAiAssistantUi_detail.h for the helpers.

#include <doctest/doctest.h>

#include "Ui/SmatchetAiAssistantUi_detail.h"

#include <string>
#include <vector>

using namespace smatchet::ai;

TEST_CASE("AiPinStripVisibleRows: clamps to the cap, floors at zero") {
    CHECK(AiPinStripVisibleRows(0, 4) == 0);
    CHECK(AiPinStripVisibleRows(-3, 4) == 0);
    CHECK(AiPinStripVisibleRows(1, 4) == 1);
    CHECK(AiPinStripVisibleRows(4, 4) == 4);
    CHECK(AiPinStripVisibleRows(50, 4) == 4);
    CHECK(AiPinStripVisibleRows(3, 4) == 3);
}

TEST_CASE("AiPinStripReservedHeight: zero when nothing visible, else rows*frame + pad") {
    CHECK(AiPinStripReservedHeight(0, 20.0f, 6.0f) == doctest::Approx(0.0f));
    // 2 rows * 20 + 4 (internal pad) + 6 (item spacing) = 50.
    CHECK(AiPinStripReservedHeight(2, 20.0f, 6.0f) == doctest::Approx(50.0f));
    // 4 rows * 18 + 4 + 8 = 84.
    CHECK(AiPinStripReservedHeight(4, 18.0f, 8.0f) == doctest::Approx(84.0f));
}

TEST_CASE("AiHistoryBodyHeight: subtracts reservations, floors at 80px") {
    // 500 - 30 - 100 - 0 - 0 = 370.
    CHECK(AiHistoryBodyHeight(500.0f, 30.0f, 100.0f, 0.0f, 0.0f) == doctest::Approx(370.0f));
    // Over-subscribed: would be negative, clamps to 80.
    CHECK(AiHistoryBodyHeight(100.0f, 50.0f, 60.0f, 20.0f, 10.0f) == doctest::Approx(80.0f));
    // Exactly at the floor.
    CHECK(AiHistoryBodyHeight(200.0f, 40.0f, 50.0f, 20.0f, 10.0f) == doctest::Approx(80.0f));
}

TEST_CASE("AiResolveProvider: maps cfg int to enum, clamps out-of-range to OpenAi") {
    CHECK(AiResolveProvider(0) == AiProvider::OpenAi);
    CHECK(AiResolveProvider(1) == AiProvider::Anthropic);
    CHECK(AiResolveProvider(2) == AiProvider::OllamaOpenAiCompat);
    CHECK(AiResolveProvider(3) == AiProvider::OllamaNative);
    // DR19: DeepSeek (kind 4) is a real selectable provider — it must not fall
    // through to OpenAi, or the per-turn model picker offers the wrong catalog.
    CHECK(AiResolveProvider(4) == AiProvider::DeepSeek);
    CHECK(AiResolveProvider(99) == AiProvider::OpenAi);
    CHECK(AiResolveProvider(-1) == AiProvider::OpenAi);
}

TEST_CASE("AiProviderFromKind: single source of truth for kind->provider (DR19)") {
    // AiResolveProvider and ai.validate-prefs both delegate here; keep this the
    // one place a new provider must be added so the copies can't drift again.
    CHECK(AiProviderFromKind(0) == AiProvider::OpenAi);
    CHECK(AiProviderFromKind(1) == AiProvider::Anthropic);
    CHECK(AiProviderFromKind(2) == AiProvider::OllamaOpenAiCompat);
    CHECK(AiProviderFromKind(3) == AiProvider::OllamaNative);
    CHECK(AiProviderFromKind(4) == AiProvider::DeepSeek);
    CHECK(AiProviderFromKind(99) == AiProvider::OpenAi);
}

TEST_CASE("AiResolveModelCombo: empty catalog -> free-form input") {
    std::vector<std::string> empty;
    const AiModelComboResolution r = AiResolveModelCombo(empty, "anything");
    CHECK(r.useFreeformInput);
    CHECK(r.comboIndex == 0);
}

TEST_CASE("AiResolveModelCombo: empty override -> default sentinel, combo path") {
    std::vector<std::string> catalog = {"gpt-a", "gpt-b"};
    const AiModelComboResolution r = AiResolveModelCombo(catalog, "");
    CHECK_FALSE(r.useFreeformInput);
    CHECK(r.comboIndex == 0);
}

TEST_CASE("AiResolveModelCombo: matching override -> 1-based catalog index") {
    std::vector<std::string> catalog = {"gpt-a", "gpt-b", "gpt-c"};
    CHECK(AiResolveModelCombo(catalog, "gpt-a").comboIndex == 1);
    CHECK(AiResolveModelCombo(catalog, "gpt-b").comboIndex == 2);
    CHECK(AiResolveModelCombo(catalog, "gpt-c").comboIndex == 3);
    CHECK_FALSE(AiResolveModelCombo(catalog, "gpt-b").useFreeformInput);
}

TEST_CASE("AiResolveModelCombo: stale override not in catalog -> free-form fallback") {
    std::vector<std::string> catalog = {"gpt-a", "gpt-b"};
    const AiModelComboResolution r = AiResolveModelCombo(catalog, "stale-id-from-other-provider");
    CHECK(r.useFreeformInput);
    CHECK(r.comboIndex == 0);
}

TEST_CASE("AiEffortComboIndex: id -> index, unknown/empty -> default 0") {
    CHECK(AiEffortComboIndex("") == 0);
    CHECK(AiEffortComboIndex("low") == 1);
    CHECK(AiEffortComboIndex("medium") == 2);
    CHECK(AiEffortComboIndex("high") == 3);
    CHECK(AiEffortComboIndex("bogus") == 0);
}

TEST_CASE("AiToastAlpha: full before fade window, linear ramp inside, clamped") {
    CHECK(AiToastAlpha(1000, 250) == doctest::Approx(1.0f)); // before fade window
    CHECK(AiToastAlpha(250, 250) == doctest::Approx(1.0f));  // at the boundary
    CHECK(AiToastAlpha(125, 250) == doctest::Approx(0.5f));  // mid-ramp
    CHECK(AiToastAlpha(0, 250) == doctest::Approx(0.0f));    // fully dismissed
    CHECK(AiToastAlpha(-10, 250) == doctest::Approx(0.0f));  // past dismissal clamps
    CHECK(AiToastAlpha(100, 0) == doctest::Approx(1.0f));    // zero fade window -> full
}

TEST_CASE("AiTruncatedPasteDroppedBytes: zero under cap, overflow above, degenerate guards") {
    constexpr int kBufCap = 8 * 1024; // BufSize = capacity+1; usable text = 8191 bytes
    // Comfortably under cap -> nothing dropped.
    CHECK(AiTruncatedPasteDroppedBytes(100, kBufCap) == 0u);
    // Exactly at the usable limit (cap-1) -> nothing dropped.
    CHECK(AiTruncatedPasteDroppedBytes(kBufCap - 1, kBufCap) == 0u);
    // One byte over the usable limit -> exactly 1 dropped.
    CHECK(AiTruncatedPasteDroppedBytes(kBufCap, kBufCap) == 1u);
    // 2 KB over -> that many bytes dropped.
    CHECK(AiTruncatedPasteDroppedBytes(kBufCap - 1 + 2048, kBufCap) == 2048u);
    // Degenerate inputs guard to zero.
    CHECK(AiTruncatedPasteDroppedBytes(0, kBufCap) == 0u);
    CHECK(AiTruncatedPasteDroppedBytes(-5, kBufCap) == 0u);
    CHECK(AiTruncatedPasteDroppedBytes(100, 1) == 0u);
    CHECK(AiTruncatedPasteDroppedBytes(100, 0) == 0u);
}

// --- Assistant dock-side fallback (issue #2048) ------------------------------------
// ApplyAssistantDocking falls back to the OTHER side bar when the requested one is not
// live. kSecondarySideBar is cut by no DockBuilder call and is absent from the embedded
// default ini, so a stored AssistantPanelOnSecondarySide = true falls back on every stock
// layout. Persisting that fallback destroyed the preference outright — and the swap button
// is disabled in exactly that state, so the UI offered no way back. These two helpers keep
// the fallback session-only while the header label still describes where the panel IS.

TEST_CASE("AiAssistantSideFallbackAfterDock: no override when the requested side was reached") {
    CHECK(AiAssistantSideFallbackAfterDock(false, false) == -1);
    CHECK(AiAssistantSideFallbackAfterDock(true, true) == -1);
}

TEST_CASE("AiAssistantSideFallbackAfterDock: records the side actually reached when it differs") {
    // The #2048 case: preference says secondary, no live secondary node, panel lands primary.
    CHECK(AiAssistantSideFallbackAfterDock(true, false) == 0);
    CHECK(AiAssistantSideFallbackAfterDock(false, true) == 1);
}

TEST_CASE("AiAssistantEffectiveOnSecondary: the stored preference rules while no fallback is set") {
    CHECK_FALSE(AiAssistantEffectiveOnSecondary(false, -1));
    CHECK(AiAssistantEffectiveOnSecondary(true, -1));
}

TEST_CASE("AiAssistantEffectiveOnSecondary: an active fallback wins without disturbing the preference") {
    // The preference stays true — the panel returns to the right the moment a real
    // secondary node exists — while the label reads "on the left" today.
    CHECK_FALSE(AiAssistantEffectiveOnSecondary(true, 0));
    CHECK(AiAssistantEffectiveOnSecondary(false, 1));
}

TEST_CASE("Assistant dock side: a stock-layout fallback round-trips without touching the preference") {
    // The whole #2048 regression at the data level: the user prefers the secondary side,
    // only the primary node is live, and the fallback fires frame after frame. The
    // preference must be untouched throughout, and the label must track the real side.
    const bool preference = true; // AssistantPanelOnSecondarySide as loaded from config
    int sideFallback = -1;
    for (int frame = 0; frame < 3; ++frame) {
        sideFallback = AiAssistantSideFallbackAfterDock(preference, false);
        CHECK(sideFallback == 0);
        CHECK_FALSE(AiAssistantEffectiveOnSecondary(preference, sideFallback));
    }
    // A secondary node appears (layout reset / a newer default ini): the stored preference
    // is honoured again with no user action and without ever having been rewritten.
    sideFallback = AiAssistantSideFallbackAfterDock(preference, true);
    CHECK(sideFallback == -1);
    CHECK(AiAssistantEffectiveOnSecondary(preference, sideFallback));
}
