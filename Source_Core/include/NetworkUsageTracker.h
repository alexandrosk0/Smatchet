#ifndef NETWORK_USAGE_TRACKER_H
#define NETWORK_USAGE_TRACKER_H

#include <atomic>
#include <cstdint>

namespace cpr {
class Response;
}

struct NetworkUsageSnapshot {
    std::uint64_t trackerRequests = 0;
    std::uint64_t trackerUploadBytes = 0;
    std::uint64_t trackerDownloadBytes = 0;
    std::uint64_t trackerErrors = 0; ///< Requests with HTTP status outside 2xx range.
};

/// Thread-safe counters for outbound HTTP from Smatchet (tracker API).
class NetworkUsageTracker {
  public:
    static constexpr std::uint64_t kEstimatedGetUploadBytes = 512;

    static NetworkUsageTracker& Instance();

    void Record(std::uint64_t uploadBodyBytes, const cpr::Response& r);

    NetworkUsageSnapshot GetSnapshot() const;

    void Reset();

  private:
    NetworkUsageTracker() = default;

    std::atomic<std::uint64_t> trackerRequests_{0};
    std::atomic<std::uint64_t> trackerUploadBytes_{0};
    std::atomic<std::uint64_t> trackerDownloadBytes_{0};
    std::atomic<std::uint64_t> trackerErrors_{0};
};

#endif







