#include "NetworkUsageTracker.h"

#include <cpr/cpr.h>

NetworkUsageTracker& NetworkUsageTracker::Instance() {
    static NetworkUsageTracker inst;
    return inst;
}

void NetworkUsageTracker::Record(HttpTrafficKind kind, std::uint64_t uploadBodyBytes, const cpr::Response& r) {
    const std::uint64_t down = static_cast<std::uint64_t>(r.text.size());
    const bool isError = (r.status_code < 200 || r.status_code >= 300);
    switch (kind) {
    case HttpTrafficKind::Tracker:
        trackerRequests_.fetch_add(1, std::memory_order_relaxed);
        trackerUploadBytes_.fetch_add(uploadBodyBytes, std::memory_order_relaxed);
        trackerDownloadBytes_.fetch_add(down, std::memory_order_relaxed);
        if (isError)
            trackerErrors_.fetch_add(1, std::memory_order_relaxed);
        break;
    case HttpTrafficKind::Ai:
        aiRequests_.fetch_add(1, std::memory_order_relaxed);
        aiUploadBytes_.fetch_add(uploadBodyBytes, std::memory_order_relaxed);
        aiDownloadBytes_.fetch_add(down, std::memory_order_relaxed);
        if (isError)
            aiErrors_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

NetworkUsageSnapshot NetworkUsageTracker::GetSnapshot() const {
    NetworkUsageSnapshot s;
    s.trackerRequests = trackerRequests_.load(std::memory_order_relaxed);
    s.trackerUploadBytes = trackerUploadBytes_.load(std::memory_order_relaxed);
    s.trackerDownloadBytes = trackerDownloadBytes_.load(std::memory_order_relaxed);
    s.trackerErrors = trackerErrors_.load(std::memory_order_relaxed);
    s.aiRequests = aiRequests_.load(std::memory_order_relaxed);
    s.aiUploadBytes = aiUploadBytes_.load(std::memory_order_relaxed);
    s.aiDownloadBytes = aiDownloadBytes_.load(std::memory_order_relaxed);
    s.aiErrors = aiErrors_.load(std::memory_order_relaxed);
    return s;
}

void NetworkUsageTracker::Reset() {
    trackerRequests_.store(0, std::memory_order_relaxed);
    trackerUploadBytes_.store(0, std::memory_order_relaxed);
    trackerDownloadBytes_.store(0, std::memory_order_relaxed);
    trackerErrors_.store(0, std::memory_order_relaxed);
    aiRequests_.store(0, std::memory_order_relaxed);
    aiUploadBytes_.store(0, std::memory_order_relaxed);
    aiDownloadBytes_.store(0, std::memory_order_relaxed);
    aiErrors_.store(0, std::memory_order_relaxed);
}
