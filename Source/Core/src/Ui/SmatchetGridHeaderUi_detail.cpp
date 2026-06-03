#include "SmatchetGridHeaderUi_detail.h"

#include <algorithm>

namespace SmatchetGridHeader {
namespace detail {

float ChipFadeFraction(float elapsedSec, float durationSec) {
    if (durationSec <= 0.0f) {
        return 1.0f;
    }
    const float f = elapsedSec / durationSec;
    return std::min(1.0f, std::max(0.0f, f));
}

float AccumulateRightWidth(const RightClusterWidths& w, bool readOnlyMode) {
    float totalRightW = 0.0f;
    bool hasAnyRightElem = false;

    if (w.showTrackerChip) {
        totalRightW += w.trackerW;
        hasAnyRightElem = true;
    }

    if (w.showReadOnlyChip) {
        if (hasAnyRightElem) {
            totalRightW += w.betweenSpacing;
        }
        totalRightW += w.readOnlyW;
        hasAnyRightElem = true;
    }

    if (w.showMcpChip) {
        if (hasAnyRightElem) {
            totalRightW += w.betweenSpacing;
        }
        totalRightW += w.mcpW;
        hasAnyRightElem = true;
    }

    if (!readOnlyMode) {
        if (hasAnyRightElem) {
            totalRightW += w.betweenSpacing;
        }
        totalRightW += w.newIssueBtnW;
    }

    return totalRightW;
}

} // namespace detail
} // namespace SmatchetGridHeader
