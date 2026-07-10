// AiChatHistoryRenderScenario — opens the AI chat panel, seeds a configurable
// number of mock `AiMessage` rows directly into `g_ui.assistantHistory`, runs
// for N frames so the new `DrawHistoryArea` + `DrawPinStripIfAny` +
// per-message `renderTurn` paths surface in `UiPerfMonitor` rows, then writes
// the snapshot to `outPath`. Phase 6.7 of ai-chat-claude-desktop-parity.
// Pinned ratio is parameterised so the pin-strip scope (`ai_chat.history.
// pin_strip`) gets exercised in the same run. Off-screen culling kicks in for
// large history sizes — the scenario intentionally seeds enough to drive the
// scroll-cull path so both visible-renderTurn and Dummy-skipped-renderTurn
// branches contribute to the avg.
// Restores the chat history vector on OnFinish / OnCancel so the user's real
// chat isn't left polluted with `[bench]` rows.
// Gated on SMATCHET_WITH_AI — the entire TU compiles as a no-op stub on the
// DX12 build (which leaves SMATCHET_WITH_AI undefined). Without the gate the
// `g_ui.assistantHistory` / `assistantHistoryRowIds` / `assistantPanelOpen`
// references would not compile (those fields live inside
// `#if defined(SMATCHET_WITH_AI)` in SmatchetUiSession.h). The factory
// registration in AppController.cpp is symmetrically gated so a non-AI build
// neither registers nor compiles the scenario.

#if defined(SMATCHET_WITH_AI)
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=file-top scaffold clone (AI gate + shared include block) across scenario TUs; owner=command-system; revisit=2026-12-31)
// clang-format on

#include "Commands/Scenarios/IScenario.h"

#include "AiTypes.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "SmatchetUiSession.h"
#include "SmatchetAiAssistantUi.h" // SmatchetClearAiRenderCaches — invalidate index-keyed render caches on history restore
#include "UiPerfMonitor.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

class AiChatHistoryRenderScenario : public IScenario {
  public:
    std::string Name() const override { return "ai-chat-history-render"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = args.value("frames", 300);
        seedCount_ = args.value("seed", 50);
        pinRatio_ = args.value("pinRatio", 0.04f); // ~4% pinned by default → 2 of 50
        warmupFrames_ = args.value("warmup", 30);

        // Snapshot user state so we can restore it cleanly.
        savedHistory_ = g_ui.assistantHistory;
        savedRowIds_ = g_ui.assistantHistoryRowIds;
        savedHydrated_ = g_ui.assistantHistoryHydrated;
        savedPanelOpen_ = g_ui.assistantPanelOpen;
        savedScrollIdx_ = g_ui.assistantScrollToMessageIndex;

        // Force the panel open. Configure-time hydrate flag flipped on so
        // dispatch / pin-toggle paths (which gate on it) behave realistically
        // — though this scenario doesn't drive any user input.
        g_ui.assistantPanelOpen = true;
        g_ui.assistantHistoryHydrated = true;
        g_ui.assistantScrollToMessageIndex = -1;
        g_ui.assistantHistory.clear();
        g_ui.assistantHistoryRowIds.clear();

        // Seed N mock messages alternating user / assistant. Body length picked
        // to be representative — short conversational sentences + the kind of
        // multi-paragraph code-and-prose mix the AI tends to return.
        // CreatedAtUnixMs distributes evenly across the last ~hour so the
        // relative-time formatter exercises every branch boundary (just now /
        // Nm ago / Nh ago).
        const std::int64_t nowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        const std::int64_t startMs = nowMs - 3600 * 1000; // 1 hour ago
        for (int i = 0; i < seedCount_; ++i) {
            AiMessage m;
            m.Role = (i % 2 == 0) ? "user" : "assistant";
            char buf[512];
            if (i % 5 == 0) {
                // Longer, mixed-content message — exercises the markdown
                // BuildPlan / RenderPlan cache + multi-line wrap path.
                std::snprintf(buf, sizeof(buf),
                              "[bench-%d] This is a slightly longer turn body that includes a "
                              "**bold** word and an `inline-code` span so the markdown plan cache "
                              "is realistically exercised across the seeded set.\n\n"
                              "Second paragraph for layout-height stability.",
                              i);
            } else {
                std::snprintf(buf, sizeof(buf), "[bench-%d] short message body for renderTurn perf seed", i);
            }
            m.Content = buf;
            m.CreatedAtUnixMs =
                startMs + (static_cast<std::int64_t>(i) * (nowMs - startMs)) / (seedCount_ > 1 ? seedCount_ - 1 : 1);
            // Pin every Nth message to ensure DrawPinStripIfAny activates.
            const int pinStride =
                (pinRatio_ > 0.0f) ? (std::max)(1, static_cast<int>(1.0f / pinRatio_)) : (seedCount_ + 1);
            m.Pinned = (i % pinStride == 0) && (i > 0);
            g_ui.assistantHistory.push_back(std::move(m));
            g_ui.assistantHistoryRowIds.push_back(-1);
        }
    }

    void OnFrame(IAppScenarioHost& /*app*/, int frameIndex) override {
        // No per-frame mutation — the scenario's whole job is to let
        // DrawHistoryArea render the seeded set repeatedly so the perf monitor
        // accumulates representative timings. Past warmup, sample each scope's
        // *finalized* per-frame total (the
        // value BeginFrame just rolled into lastFrame_) and accumulate it. The
        // gated metric is then the run-median of these per-frame totals rather
        // than a single arbitrary frame's lastTotalMs (#1261): for a sub-10µs
        // scope the single-frame snapshot swings 2.7× run-to-run — above the
        // 0.05ms perf-gate abs floor — so the gate trips on jitter, not code.
        // GetLastFrameRows(false) is a cheap copy of lastFrame_ (no nth_element)
        // and runs here in OnFrame, *outside* any measured Draw scope, so it
        // adds no observer cost to the scopes it samples.
        if (frameIndex >= warmupFrames_) {
            const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/false);
            for (std::vector<UiPerfRow>::const_iterator it = rows.begin(); it != rows.end(); ++it) {
                perFrameTotals_[it->name].push_back(it->lastTotalMs);
                perFrameAvgPerCall_[it->name].push_back(it->avgPerCallMs);
            }
        }
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    void OnCancel(IAppScenarioHost& /*app*/) override { Restore(); }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = nlohmann::json::array();
        // std::transform satisfies cppcheck `useStlAlgorithm` over the raw
        // push_back loop; functionally identical (back_inserter appends one
        // JSON object per row). The PriorityGridScrollScenario reference uses
        // a raw loop and is not flagged — likely grandfathered. New code
        // follows the algorithm style.
        std::transform(rows.begin(), rows.end(), std::back_inserter(rowsJson), [this](const UiPerfRow& r) {
            // Override the per-frame-volatile totals with the run-median of the
            // finalized per-frame totals collected across the post-warmup window
            // (#1261). avgPerCallMs is medianed from its OWN per-frame samples
            // (same sampling basis as the total) rather than derived as
            // median(total) / r.calls — r.calls is the final-frame snapshot's
            // count, so dividing a window-median total by it mixes two bases and
            // skews the per-call figure whenever the call count varies frame to
            // frame. p99Ms/maxMs/emaAvgMs already aggregate over many frames
            // (ring / EMA) and stay as the monitor reports them. If a scope was
            // never sampled (e.g. it only appeared on the final frame) keep its
            // snapshot value rather than fabricating a zero.
            double lastTotalMs = r.lastTotalMs;
            double avgPerCallMs = r.avgPerCallMs;
            std::unordered_map<std::string, std::vector<double>>::const_iterator s = perFrameTotals_.find(r.name);
            if (s != perFrameTotals_.end() && !s->second.empty()) {
                lastTotalMs = MedianOf(s->second);
            }
            std::unordered_map<std::string, std::vector<double>>::const_iterator a = perFrameAvgPerCall_.find(r.name);
            if (a != perFrameAvgPerCall_.end() && !a->second.empty()) {
                avgPerCallMs = MedianOf(a->second);
            }
            return nlohmann::json{
                {"name", r.name},   {"lastTotalMs", lastTotalMs}, {"avgPerCallMs", avgPerCallMs},
                {"maxMs", r.maxMs}, {"calls", r.calls},           {"emaAvgMs", r.emaAvgMs},
                {"p99Ms", r.p99Ms},
            };
        });
        nlohmann::json out;
        out["frames"] = frames_;
        out["warmup"] = warmupFrames_;
        out["seed"] = seedCount_;
        out["pinRatio"] = pinRatio_;
        out["rows"] = std::move(rowsJson);
        Restore();
        return out;
    }

  private:
    // Median of a sample set (copy-by-value: caller's vector is untouched).
    // Even counts average the two central order statistics; biased-low ties
    // are fine for a perf-gate metric. nth_element is O(n) per call.
    static double MedianOf(std::vector<double> v) {
        if (v.empty()) {
            return 0.0;
        }
        const std::size_t n = v.size();
        const std::size_t mid = n / 2;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
        const double hi = v[mid];
        if (n % 2 == 1) {
            return hi;
        }
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid - 1), v.end());
        const double lo = v[mid - 1];
        return (lo + hi) * 0.5;
    }

    void Restore() {
        g_ui.assistantHistory = std::move(savedHistory_);
        g_ui.assistantHistoryRowIds = std::move(savedRowIds_);
        g_ui.assistantHistoryHydrated = savedHydrated_;
        g_ui.assistantPanelOpen = savedPanelOpen_;
        g_ui.assistantScrollToMessageIndex = savedScrollIdx_;
        // Invalidate the index-keyed render caches: the panel's body-hash cache only
        // resets on a size change, so if the restored history length equals the seeded
        // count the stale per-index hashes would be reused against the real bodies.
        SmatchetClearAiRenderCaches();
    }

    int frames_ = 300;
    int seedCount_ = 50;
    float pinRatio_ = 0.04f;
    int warmupFrames_ = 30;

    std::vector<AiMessage> savedHistory_;
    std::vector<std::int64_t> savedRowIds_;
    bool savedHydrated_ = false;
    bool savedPanelOpen_ = false;
    int savedScrollIdx_ = -1;

    // scope name → finalized per-frame totalMs collected across the post-warmup
    // window; OnFinish reports the median of each as the gated lastTotalMs.
    std::unordered_map<std::string, std::vector<double>> perFrameTotals_;
    // scope name → finalized per-frame avgPerCallMs over the same window. OnFinish
    // reports the median of each as the gated avgPerCallMs (kept on the same
    // sampling basis as the total rather than derived from it).
    std::unordered_map<std::string, std::vector<double>> perFrameAvgPerCall_;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeAiChatHistoryRenderScenario() {
    return std::make_unique<smatchet::cmd::AiChatHistoryRenderScenario>();
}

#endif // SMATCHET_WITH_AI
