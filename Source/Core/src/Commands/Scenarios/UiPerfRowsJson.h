#pragma once

// The perf scenarios' shared OnFinish serialization: one UiPerfRow snapshot →
// the `rows` JSON array shape that scripts/dev/perf-baseline.sh and the perf
// CI lane consume. Field set and order are part of that contract — scenarios
// that need extra per-row fields (e.g. AiChatHistoryRenderScenario's turn
// annotations) keep their own serializer instead of extending this one.

#include "UiPerfMonitor.h"

#include <nlohmann/json.hpp>

#include <vector>

inline nlohmann::json UiPerfRowsToJson(const std::vector<UiPerfRow>& rows) {
    nlohmann::json rowsJson = nlohmann::json::array();
    for (const UiPerfRow& r : rows) {
        rowsJson.push_back({
            {"name", r.name},
            {"lastTotalMs", r.lastTotalMs},
            {"avgPerCallMs", r.avgPerCallMs},
            {"maxMs", r.maxMs},
            {"calls", r.calls},
            {"emaAvgMs", r.emaAvgMs},
            {"p99Ms", r.p99Ms},
        });
    }
    return rowsJson;
}
