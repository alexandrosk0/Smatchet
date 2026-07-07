#pragma once

// Pure (ImGui-free) decision + arithmetic helpers extracted from
// SmatchetAiAssistantUi.cpp during the function-size decomposition. Keeping
// these as free functions in a header lets the doctest rig
// (AiAssistantUiDetail.test.cpp) pin every branch without dragging ImGui or the
// SmatchetAiAssistantUi TU's namespace-private statics into the test binary.
// Behaviour is byte-identical to the inline arithmetic the draw functions used
// before the split — this is a mechanical extraction, not a logic change.

#include "AiTypes.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace smatchet {
namespace ai {

/// Visible-row count for the pinned-bookmarks strip, capped at maxVisibleRows.
/// Mirrors the `(std::min)(static_cast<int>(pinnedCount), kMaxVisiblePinRows)`
/// inline computed in both DrawPinStripIfAny and DrawHistoryArea so the two
/// layouts stay in lock-step.
inline int AiPinStripVisibleRows(long pinnedCount, int maxVisibleRows) {
    if (pinnedCount <= 0) {
        return 0;
    }
    const int asInt = static_cast<int>(pinnedCount);
    return asInt < maxVisibleRows ? asInt : maxVisibleRows;
}

/// Reserved pixel height for the pin strip given the visible-row count, the
/// per-row frame-with-spacing height, and the inter-item spacing. Returns 0
/// when nothing is pinned (no layout reservation). Matches the height the strip
/// child uses internally plus one ItemSpacing.y of separation.
inline float AiPinStripReservedHeight(int visibleRows, float frameHeightWithSpacing, float itemSpacingY) {
    if (visibleRows <= 0) {
        return 0.0f;
    }
    return frameHeightWithSpacing * static_cast<float>(visibleRows) + 4.0f + itemSpacingY;
}

/// Realised scroll-body height for the history child: total avail minus the
/// header, input, error-strip and pin-strip reservations, floored at 80px so a
/// tall toolbar never collapses the scroll region to nothing.
inline float AiHistoryBodyHeight(float availY, float headerH, float inputH, float errorStripH, float pinStripH) {
    const float remaining = availY - headerH - inputH - errorStripH - pinStripH;
    return (std::max)(80.0f, remaining);
}

/// Bytes dropped when a paste / splice would push the chat-input text past the
/// fixed InputText buffer cap. `requestedTextLen` is the would-be text length in
/// bytes (excluding the NUL), `bufCapBytes` is the buffer's total capacity in
/// bytes (capacity+1, i.e. ImGui's BufSize — includes the NUL slot). The usable
/// text room is `bufCapBytes - 1`; anything beyond that is silently clamped by
/// ImGui, so this returns the count the user lost (0 when nothing was dropped or
/// inputs are degenerate). Pure so AiAssistantUiDetail.test.cpp can pin the
/// boundary without an ImGui context — see the CallbackResize wiring in
/// SmatchetAiAssistantUi.cpp::InputBufferResizeCallback.
inline std::size_t AiTruncatedPasteDroppedBytes(int requestedTextLen, int bufCapBytes) {
    if (requestedTextLen <= 0 || bufCapBytes <= 1) {
        return 0;
    }
    const int usable = bufCapBytes - 1; // reserve the NUL terminator slot
    if (requestedTextLen <= usable) {
        return 0;
    }
    return static_cast<std::size_t>(requestedTextLen - usable);
}

/// Maps the persisted `cfg.AiProviderKind` int onto the AiProvider enum,
/// clamping any out-of-range value to OpenAi (same policy as
/// AiPrefsValidator::ClampProvider).
inline AiProvider AiResolveProvider(int providerKind) {
    switch (providerKind) {
    case 1:
        return AiProvider::Anthropic;
    case 2:
        return AiProvider::OllamaOpenAiCompat;
    case 3:
        return AiProvider::OllamaNative;
    case 4:
        return AiProvider::DeepSeek;
    case 0:
    default:
        return AiProvider::OpenAi;
    }
}

/// Result of resolving the per-turn model picker against the active provider's
/// catalog. `useFreeformInput` is true when the catalog is empty OR the saved
/// override doesn't match any catalog entry (so the UI shows a free-form
/// InputText rather than silently displaying `<default model>` over a hidden
/// non-catalog override). `comboIndex` is the 1-based catalog index (0 ==
/// `<default model>` sentinel) for the Combo path.
struct AiModelComboResolution {
    bool useFreeformInput = false;
    int comboIndex = 0;
};

/// Resolve the model Combo selection. `catalogIds` is the active provider's
/// catalog in display order; `savedModelId` is the persisted per-turn override
/// (empty == inherit default). Behaviour mirrors the inline resolution in
/// SmatchetDrawAiAssistantPanel's per-turn model row.
inline AiModelComboResolution AiResolveModelCombo(const std::vector<std::string>& catalogIds,
                                                  const std::string& savedModelId) {
    AiModelComboResolution out;
    out.useFreeformInput = catalogIds.empty();
    out.comboIndex = 0;
    if (!savedModelId.empty() && !catalogIds.empty()) {
        for (std::size_t i = 0; i < catalogIds.size(); ++i) {
            if (catalogIds[i] == savedModelId) {
                out.comboIndex = 1 + static_cast<int>(i);
                return out;
            }
        }
        // Saved override is not in the catalog (stale provider switch / custom
        // id). Fall back to free-form so the UI reflects what Send will use.
        out.useFreeformInput = true;
    }
    return out;
}

/// Combo index (0..3) for the saved reasoning-effort id. `effortId` matches one
/// of {"", "low", "medium", "high"}; anything unrecognised (including the empty
/// default) maps to index 0 (`<default effort>`).
inline int AiEffortComboIndex(const std::string& effortId) {
    static const char* kIds[] = {"", "low", "medium", "high"};
    for (int i = 1; i < 4; ++i) {
        if (effortId == kIds[i]) {
            return i;
        }
    }
    return 0;
}

/// Copy-toast fade-out alpha. Full opacity until the final `fadeOutMs`, then a
/// linear ramp to 0 as `remainingMs` approaches 0. Clamped to [0,1].
inline float AiToastAlpha(long long remainingMs, long long fadeOutMs) {
    if (fadeOutMs <= 0) {
        return 1.0f;
    }
    if (remainingMs >= fadeOutMs) {
        return 1.0f;
    }
    if (remainingMs <= 0) {
        return 0.0f;
    }
    return static_cast<float>(remainingMs) / static_cast<float>(fadeOutMs);
}

} // namespace ai
} // namespace smatchet
