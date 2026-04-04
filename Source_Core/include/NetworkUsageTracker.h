#ifndef NETWORK_USAGE_TRACKER_H
#define NETWORK_USAGE_TRACKER_H

#include <atomic>
#include <cstdint>

namespace cpr {
class Response;
}

enum class HttpTrafficKind {
    Jira,
    OpenAi
};

struct NetworkUsageSnapshot {
    std::uint64_t jiraRequests = 0;
    std::uint64_t jiraUploadBytes = 0;
    std::uint64_t jiraDownloadBytes = 0;
    std::uint64_t openAiRequests = 0;
    std::uint64_t openAiUploadBytes = 0;
    std::uint64_t openAiDownloadBytes = 0;
};

/// Thread-safe counters for outbound HTTP from Smatchet (Jira API, OpenAI).
class NetworkUsageTracker {
public:
    static constexpr std::uint64_t kEstimatedGetUploadBytes = 512;

    static NetworkUsageTracker& Instance();

    void Record(HttpTrafficKind kind, std::uint64_t uploadBodyBytes, const cpr::Response& r);

    NetworkUsageSnapshot GetSnapshot() const;

    void Reset();

private:
    NetworkUsageTracker() = default;

    std::atomic<std::uint64_t> jiraRequests_{0};
    std::atomic<std::uint64_t> jiraUploadBytes_{0};
    std::atomic<std::uint64_t> jiraDownloadBytes_{0};
    std::atomic<std::uint64_t> openAiRequests_{0};
    std::atomic<std::uint64_t> openAiUploadBytes_{0};
    std::atomic<std::uint64_t> openAiDownloadBytes_{0};
};

#endif
