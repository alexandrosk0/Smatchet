#include "UiPerfMonitor.h"

#include <algorithm>
#include <chrono>

namespace {

constexpr double kEmaAlpha = 0.15;

} // namespace

UiPerfMonitor& UiPerfMonitor::Instance() {
    static UiPerfMonitor inst;
    return inst;
}

void UiPerfMonitor::BeginFrame() {
    std::lock_guard<std::mutex> lock(mutex_);

    lastFrame_.clear();

    for (ScopeRecord& s : scopes_) {
        if (s.frame.calls == 0) {
            continue; // not hit this frame — keep ring/EMA history, emit no row
        }
        const Agg& a = s.frame;
        UiPerfRow row;
        row.name = s.name;
        row.calls = a.calls;
        row.maxMs = static_cast<double>(a.maxNs) / 1e6;
        row.lastTotalMs = static_cast<double>(a.totalNs) / 1e6;
        row.avgPerCallMs = row.lastTotalMs / static_cast<double>(a.calls);
        std::uint64_t& life = lifetimeHits_[row.name];
        life += static_cast<std::uint64_t>(row.calls);
        row.lifetimeHits = life;

        const auto emaIt = emaByName_.find(row.name);
        if (emaIt == emaByName_.end()) {
            emaByName_[row.name] = row.avgPerCallMs;
            row.emaAvgMs = row.avgPerCallMs;
        } else {
            emaIt->second = kEmaAlpha * row.avgPerCallMs + (1.0 - kEmaAlpha) * emaIt->second;
            row.emaAvgMs = emaIt->second;
        }
        s.frame = Agg();
        lastFrame_.push_back(std::move(row));
    }

    std::sort(lastFrame_.begin(), lastFrame_.end(),
              [](const UiPerfRow& a, const UiPerfRow& b) { return a.lastTotalMs > b.lastTotalMs; });
}

void UiPerfMonitor::Record(const char* name, std::chrono::nanoseconds duration) {
    if (!name) {
        return;
    }
    const std::uint64_t ns = static_cast<std::uint64_t>(duration.count());
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(scopes_.begin(), scopes_.end(), [&](const ScopeRecord& s) { return s.name == name; });
    if (it == scopes_.end()) {
        // First sighting of this scope name: one-time registration allocation
        // (name string + the 2 KB ring inside the vector slot). The steady
        // state never reaches here — every later call is the branch below.
        scopes_.push_back(ScopeRecord());
        it = scopes_.end() - 1;
        it->name = name;
    }
    it->frame.totalNs += ns;
    it->frame.calls += 1;
    it->frame.maxNs = std::max(it->frame.maxNs, ns);
    it->ring.Push(static_cast<double>(ns) / 1e6);
}

std::vector<UiPerfRow> UiPerfMonitor::GetLastFrameRows(bool includeP99) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UiPerfRow> rows = lastFrame_;
    if (includeP99) {
        // Cold path (CLI perf.snapshot / scenario finish serialization): copy
        // each scope's ring and run one nth_element. The per-frame UI panel
        // passes false and never pays this.
        std::vector<double> scratch;
        scratch.reserve(PerfSampleRing::kCapacity);
        for (UiPerfRow& row : rows) {
            const auto sIt =
                std::find_if(scopes_.begin(), scopes_.end(), [&](const ScopeRecord& s) { return s.name == row.name; });
            if (sIt == scopes_.end()) {
                continue; // Reset raced between BeginFrame and snapshot — leave 0.0
            }
            scratch.clear();
            sIt->ring.AppendSamples(scratch);
            row.p99Ms = ComputeP99(scratch);
        }
    }
    return rows;
}

double UiPerfMonitor::LastFrameTopTotalMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double topMs = 0.0;
    for (const UiPerfRow& row : lastFrame_) {
        if (row.lastTotalMs > topMs)
            topMs = row.lastTotalMs;
    }
    return topMs;
}

void UiPerfMonitor::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    scopes_.clear();
    lastFrame_.clear();
    emaByName_.clear();
    lifetimeHits_.clear();
}

UiPerfScope::UiPerfScope(const char* name)
    : name_(name), t0_(name ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}

UiPerfScope::~UiPerfScope() {
    if (!name_) {
        return;
    }
    const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0_);
    UiPerfMonitor::Instance().Record(name_, dt);
}
