#include "NetworkUsageTracker.h"

#include <cpr/cpr.h>

NetworkUsageTracker& NetworkUsageTracker::Instance() {
    static NetworkUsageTracker inst;
    return inst;
}

void NetworkUsageTracker::Record(HttpTrafficKind kind, std::uint64_t uploadBodyBytes, const cpr::Response& r) {
    const std::uint64_t down = static_cast<std::uint64_t>(r.text.size());
    if (kind == HttpTrafficKind::Jira) {
        jiraRequests_.fetch_add(1, std::memory_order_relaxed);
        jiraUploadBytes_.fetch_add(uploadBodyBytes, std::memory_order_relaxed);
        jiraDownloadBytes_.fetch_add(down, std::memory_order_relaxed);
    } else {
        openAiRequests_.fetch_add(1, std::memory_order_relaxed);
        openAiUploadBytes_.fetch_add(uploadBodyBytes, std::memory_order_relaxed);
        openAiDownloadBytes_.fetch_add(down, std::memory_order_relaxed);
    }
}

NetworkUsageSnapshot NetworkUsageTracker::GetSnapshot() const {
    NetworkUsageSnapshot s;
    s.jiraRequests = jiraRequests_.load(std::memory_order_relaxed);
    s.jiraUploadBytes = jiraUploadBytes_.load(std::memory_order_relaxed);
    s.jiraDownloadBytes = jiraDownloadBytes_.load(std::memory_order_relaxed);
    s.openAiRequests = openAiRequests_.load(std::memory_order_relaxed);
    s.openAiUploadBytes = openAiUploadBytes_.load(std::memory_order_relaxed);
    s.openAiDownloadBytes = openAiDownloadBytes_.load(std::memory_order_relaxed);
    return s;
}

void NetworkUsageTracker::Reset() {
    jiraRequests_.store(0, std::memory_order_relaxed);
    jiraUploadBytes_.store(0, std::memory_order_relaxed);
    jiraDownloadBytes_.store(0, std::memory_order_relaxed);
    openAiRequests_.store(0, std::memory_order_relaxed);
    openAiUploadBytes_.store(0, std::memory_order_relaxed);
    openAiDownloadBytes_.store(0, std::memory_order_relaxed);
}
