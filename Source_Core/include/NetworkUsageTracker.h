#ifndef NETWORK_USAGE_TRACKER_H
#define NETWORK_USAGE_TRACKER_H

#include <atomic>
#include <cstdint>

namespace cpr {
class Response;
}

enum class HttpTrafficKind { Tracker, OpenAi };

struct NetworkUsageSnapshot {
    std::uint64_t trackerRequests = 0;
    std::uint64_t trackerUploadBytes = 0;
    std::uint64_t trackerDownloadBytes = 0;
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

    std::atomic<std::uint64_t> trackerRequests_{0};
    std::atomic<std::uint64_t> trackerUploadBytes_{0};
    std::atomic<std::uint64_t> trackerDownloadBytes_{0};
    std::atomic<std::uint64_t> openAiRequests_{0};
    std::atomic<std::uint64_t> openAiUploadBytes_{0};
    std::atomic<std::uint64_t> openAiDownloadBytes_{0};
};

#endif







