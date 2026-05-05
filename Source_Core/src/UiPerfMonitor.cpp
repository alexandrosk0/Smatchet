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

    std::unordered_map<std::string, Agg> map;
    map.reserve(working_.size());
    for (const auto& p : working_) {
        auto& a = map[p.first];
        a.totalNs += p.second.totalNs;
        a.calls += p.second.calls;
        a.maxNs = std::max(a.maxNs, p.second.maxNs);
    }
    working_.clear();

    lastFrame_.clear();
    lastFrame_.reserve(map.size());

    for (auto& kv : map) {
        UiPerfRow row;
        row.name = std::move(kv.first);
        const Agg& a = kv.second;
        row.calls = a.calls;
        row.maxMs = static_cast<double>(a.maxNs) / 1e6;
        row.lastTotalMs = static_cast<double>(a.totalNs) / 1e6;
        row.avgPerCallMs = a.calls > 0 ? row.lastTotalMs / static_cast<double>(a.calls) : 0.0;
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
    for (auto& p : working_) {
        if (p.first == name) {
            p.second.totalNs += ns;
            p.second.calls += 1;
            p.second.maxNs = std::max(p.second.maxNs, ns);
            return;
        }
    }
    working_.push_back({std::string(name), Agg{ns, 1u, ns}});
}

std::vector<UiPerfRow> UiPerfMonitor::GetLastFrameRows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastFrame_;
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






