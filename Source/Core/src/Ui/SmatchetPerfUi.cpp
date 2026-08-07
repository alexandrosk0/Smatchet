#include "SmatchetPerfUi.h"

#include "NetworkUsageTracker.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetHelpMarker.h"
#include "SmatchetLocalization.h"
#include "UiPerfMonitor.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::string FormatNetworkBytes(std::uint64_t n) {
    if (n < 1024u) {
        return std::to_string(n) + " B";
    }
    const double kb = static_cast<double>(n) / 1024.0;
    if (kb < 1024.0) {
        char b[48];
        std::snprintf(b, sizeof(b), "%.2f KB", kb);
        return std::string(b);
    }
    char b[48];
    std::snprintf(b, sizeof(b), "%.2f MB", kb / 1024.0);
    return std::string(b);
}

enum class PerfTableCol : ImGuiID {
    Scope = 1,
    Calls,
    Last,
    Avg,
    Ema,
    Max,
    LifetimeHits,
};

int CompareRoundedMs(double x, double y) {
    const auto qx = static_cast<long long>(std::llround(x * 100.0));
    const auto qy = static_cast<long long>(std::llround(y * 100.0));
    if (qx < qy) {
        return -1;
    }
    if (qx > qy) {
        return 1;
    }
    return 0;
}

/// Coarser than display (0.01 ms); reduces sort churn when smoothed values drift within the same bucket.
int CompareStableLastMs(double x, double y) {
    const auto qx = static_cast<long long>(std::llround(x * 10.0));
    const auto qy = static_cast<long long>(std::llround(y * 10.0));
    if (qx < qy) {
        return -1;
    }
    if (qx > qy) {
        return 1;
    }
    return 0;
}

void SortRowsByStableLastDesc(std::vector<UiPerfRow>& rows) {
    std::sort(rows.begin(), rows.end(), [](const UiPerfRow& a, const UiPerfRow& b) {
        const int cmp = CompareStableLastMs(a.lastTotalMs, b.lastTotalMs);
        if (cmp != 0) {
            return cmp > 0;
        }
        return a.name < b.name;
    });
}

void SortUiPerfRows(std::vector<UiPerfRow>& rows, const ImGuiTableSortSpecs* sort_specs) {
    if (!sort_specs || sort_specs->SpecsCount <= 0) {
        return;
    }
    const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[0];
    const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
    std::sort(rows.begin(), rows.end(), [&](const UiPerfRow& a, const UiPerfRow& b) {
        int cmp = 0;
        switch (static_cast<PerfTableCol>(spec.ColumnUserID)) {
        case PerfTableCol::Scope:
            cmp = a.name < b.name ? -1 : (a.name > b.name ? 1 : 0);
            break;
        case PerfTableCol::Last:
            cmp = CompareStableLastMs(a.lastTotalMs, b.lastTotalMs);
            break;
        case PerfTableCol::Avg:
            cmp = CompareRoundedMs(a.avgPerCallMs, b.avgPerCallMs);
            break;
        case PerfTableCol::Ema:
            cmp = CompareRoundedMs(a.emaAvgMs, b.emaAvgMs);
            break;
        case PerfTableCol::Max:
            cmp = CompareRoundedMs(a.maxMs, b.maxMs);
            break;
        case PerfTableCol::Calls:
            cmp = a.calls < b.calls ? -1 : (a.calls > b.calls ? 1 : 0);
            break;
        case PerfTableCol::LifetimeHits:
            cmp = a.lifetimeHits < b.lifetimeHits ? -1 : (a.lifetimeHits > b.lifetimeHits ? 1 : 0);
            break;
        default:
            cmp = 0;
            break;
        }
        if (cmp == 0) {
            return a.name < b.name;
        }
        return asc ? (cmp < 0) : (cmp > 0);
    });
}

} // namespace

namespace {

/// Seconds; larger = calmer readout (63% toward target after this many seconds).
constexpr double kCpuSmoothTauSec = 3.5;
constexpr double kFpsSmoothTauSec = 2.5;
constexpr double kSmoothDtClampSec = 0.2;

double SmoothingAlpha(double dtSec, double tauSec) {
    if (tauSec <= 0.0) {
        return 1.0;
    }
    if (dtSec <= 0.0) {
        return 0.0;
    }
    double dt = dtSec;
    if (dt > kSmoothDtClampSec) {
        dt = kSmoothDtClampSec;
    }
    double a = 1.0 - std::exp(-dt / tauSec);
    if (a > 1.0) {
        a = 1.0;
    }
    if (a < 1e-9) {
        a = 1e-9;
    }
    return a;
}

} // namespace

void SmatchetPerfUi::buildSmoothedCpuRows(const std::vector<UiPerfRow>& raw, std::vector<UiPerfRow>& out,
                                          double dtSec) {
    out.clear();
    out.reserve(raw.size());

    std::unordered_set<std::string> present;
    present.reserve(raw.size() * 2 + 1);
    for (const UiPerfRow& r : raw) {
        present.insert(r.name);
    }

    for (auto it = cpuSmooth_.begin(); it != cpuSmooth_.end();) {
        if (present.find(it->first) == present.end()) {
            it = cpuSmooth_.erase(it);
        } else {
            ++it;
        }
    }

    const double a = SmoothingAlpha(dtSec, kCpuSmoothTauSec);
    const double inv = 1.0 - a;

    for (const UiPerfRow& r : raw) {
        CpuSmoothBucket& b = cpuSmooth_[r.name];
        if (!b.hasValue) {
            b.lastTotalMs = r.lastTotalMs;
            b.avgPerCallMs = r.avgPerCallMs;
            b.emaAvgMs = r.emaAvgMs;
            b.maxMs = r.maxMs;
            b.hasValue = true;
        } else {
            b.lastTotalMs = a * r.lastTotalMs + inv * b.lastTotalMs;
            b.avgPerCallMs = a * r.avgPerCallMs + inv * b.avgPerCallMs;
            b.emaAvgMs = a * r.emaAvgMs + inv * b.emaAvgMs;
            b.maxMs = a * r.maxMs + inv * b.maxMs;
        }

        UiPerfRow d;
        d.name = r.name;
        d.lastTotalMs = b.lastTotalMs;
        d.avgPerCallMs = b.avgPerCallMs;
        d.emaAvgMs = b.emaAvgMs;
        d.maxMs = b.maxMs;
        d.calls = r.calls;
        d.lifetimeHits = r.lifetimeHits;
        out.push_back(std::move(d));
    }
}

void SmatchetPerfUi::updateSmoothedFps() {
    const ImGuiIO& io = ImGui::GetIO();
    const double dt = static_cast<double>(io.DeltaTime);
    const double fps = static_cast<double>(io.Framerate);
    const double frameMs = dt > 0.0 ? dt * 1000.0 : 0.0;
    if (!fpsSmoothInit_) {
        smoothFps_ = fps;
        smoothFrameMs_ = frameMs;
        fpsSmoothInit_ = true;
    } else {
        const double fa = SmoothingAlpha(dt, kFpsSmoothTauSec);
        smoothFps_ = fa * fps + (1.0 - fa) * smoothFps_;
        smoothFrameMs_ = fa * frameMs + (1.0 - fa) * smoothFrameMs_;
    }
}

void SmatchetPerfUi::DrawWindow(bool* pOpen, bool wantFocus) {
    if (!pOpen || !*pOpen) {
        return;
    }
    updateSmoothedFps();
    static bool s_needsReDock = false;
    // 0 once the bottom panel is gone; docking to it then produces an orphan node that only
    // looks docked (EnsureDockSlotAlive).
    const ImGuiID slot = SmatchetDockNodeIds::EnsureDockSlotAlive(SmatchetDockNodeIds::kBottomPanel);
    if (slot != 0) {
        if (s_needsReDock) {
            // Consumed only against a live slot. The re-arm below sits inside `if (Begin(...))`,
            // so it does not run for a collapsed window or an unselected tab — consuming the
            // latch on a dead node would drop the request with nothing left to restore it.
            s_needsReDock = false;
            ImGui::SetNextWindowDockID(slot, ImGuiCond_Always);
        } else if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
            ImGui::SetNextWindowDockID(slot, ImGuiCond_FirstUseEver);
        }
    }
    ImGui::SetNextWindowSize(ImVec2(580, 380), ImGuiCond_FirstUseEver);
    if (wantFocus) {
        ImGui::SetNextWindowFocus();
    }
    if (ImGui::Begin("Performance", pOpen)) {
        if (!ImGui::IsWindowDocked() && !ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
            s_needsReDock = true;
        }
        if (wantFocus) {
            ImGui::SetWindowFocus();
        }
        const double dt = static_cast<double>(ImGui::GetIO().DeltaTime);
        ImGui::Text("FPS: %.1f    Frame: %.2f ms (smoothed)", smoothFps_, smoothFrameMs_);
        ImGui::Separator();

        if (ImGui::BeginTabBar("PerformanceTabs")) {
            drawCpuTab(dt);
            drawNetworkTab();
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// CPU tab body showing the per-scope smoothed wall-time table. Owns its own tab-item begin and end
// pair and runs inside the active tab-bar scope. Split out of DrawWindow for function-size
// compliance.
void SmatchetPerfUi::drawCpuTab(double dtSec) {
    if (ImGui::BeginTabItem("CPU")) {
        ImGui::TextUnformatted("Wall time per scoped UI path (smoothed).");
        ImGui::SameLine();
        SmatchetHelpMarker::Render(
            "perf.cpu.intro.help",
            "Wall time per scoped UI path; nested scopes overlap (not additive). "
            "Values use heavy time-based smoothing (~few second response) and 2 decimal places. "
            "Row order for Last (ms) uses 0.1 ms buckets so nearby scopes do not swap every frame.");
        ImGui::Separator();
        std::vector<UiPerfRow> rows;
        buildSmoothedCpuRows(UiPerfMonitor::Instance().GetLastFrameRows(), rows, dtSec);
        if (ImGui::BeginTable("UiPerfTable", 7,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_Sortable)) {
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_None, 0.0f,
                                    static_cast<ImGuiID>(PerfTableCol::Scope));
            ImGui::TableSetupColumn("Calls/Frame", ImGuiTableColumnFlags_None, 0.0f,
                                    static_cast<ImGuiID>(PerfTableCol::Calls));
            ImGui::TableSetupColumn("Last (ms)",
                                    ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
                                    0.0f, static_cast<ImGuiID>(PerfTableCol::Last));
            ImGui::TableSetupColumn("Avg/call (ms)", ImGuiTableColumnFlags_None, 0.0f,
                                    static_cast<ImGuiID>(PerfTableCol::Avg));
            ImGui::TableSetupColumn("EMA avg (ms)", ImGuiTableColumnFlags_None, 0.0f,
                                    static_cast<ImGuiID>(PerfTableCol::Ema));
            ImGui::TableSetupColumn("Max (ms)", ImGuiTableColumnFlags_None, 0.0f,
                                    static_cast<ImGuiID>(PerfTableCol::Max));
            ImGui::TableSetupColumn("Total Calls", ImGuiTableColumnFlags_None, 0.0f,
                                    static_cast<ImGuiID>(PerfTableCol::LifetimeHits));
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                if (sort_specs->SpecsCount > 0) {
                    SortUiPerfRows(rows, sort_specs);
                    if (sort_specs->SpecsDirty) {
                        sort_specs->SpecsDirty = false;
                    }
                } else {
                    SortRowsByStableLastDesc(rows);
                }
            } else {
                SortRowsByStableLastDesc(rows);
            }
            for (const UiPerfRow& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(r.calls));
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.lastTotalMs);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.avgPerCallMs);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.emaAvgMs);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.maxMs);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(r.lifetimeHits));
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
}

// Network tab body: session HTTP traffic counters + reset button. Owns its own BeginTabItem/
// EndTabItem pair; runs inside the active BeginTabBar scope.
void SmatchetPerfUi::drawNetworkTab() {
    if (ImGui::BeginTabItem("Network")) {
        const NetworkUsageSnapshot snap = NetworkUsageTracker::Instance().GetSnapshot();
        ImGui::TextUnformatted("HTTP traffic from this session of Smatchet.");
        ImGui::SameLine();
        SmatchetHelpMarker::RenderText(SmatchetLocalization::Format(
            "perf.network.intro.help",
            "Downloaded = response bodies; uploaded = request bodies for POST/PUT, or ~%llu B per Jira GET "
            "(estimate).",
            static_cast<unsigned long long>(NetworkUsageTracker::kEstimatedGetUploadBytes)));
        ImGui::Separator();
        ImGui::TextUnformatted("Jira API");
        ImGui::BulletText("Requests: %llu", static_cast<unsigned long long>(snap.trackerRequests));
        ImGui::BulletText("Uploaded (approx.): %s", FormatNetworkBytes(snap.trackerUploadBytes).c_str());
        ImGui::BulletText("Downloaded: %s", FormatNetworkBytes(snap.trackerDownloadBytes).c_str());
        ImGui::Separator();
        if (ImGui::Button("Reset counters")) {
            NetworkUsageTracker::Instance().Reset();
        }
        ImGui::EndTabItem();
    }
}

void SmatchetPerfUi::DrawFpsOverlay() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, io.DisplaySize.y - 12.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##SmatchetFpsOverlay", nullptr, kFlags)) {
        ImGui::Text("FPS %.1f  %.2f ms", smoothFps_, smoothFrameMs_);
    }
    ImGui::End();
}
