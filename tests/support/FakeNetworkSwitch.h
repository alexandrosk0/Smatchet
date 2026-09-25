#ifndef SMATCHET_TESTS_FAKE_NETWORK_SWITCH_H
#define SMATCHET_TESTS_FAKE_NETWORK_SWITCH_H

// FakeNetworkSwitch — process-wide "is the network up" switch for fake tracker backends (Quality
// Pillar 6 offline-first harness). FakeTrackerClient consults it before every network-shaped call, so
// bucket-A doctests and bucket-E UI tests can take the tracker offline and bring it back.
// GlobalFakeNetwork() is an inline function with a function-local static: ONE instance per executable,
// shared by every TU. A namespace-scope static would give each TU its own copy.

#include "Tracker/TrackerError.h"

#include <atomic>

namespace smatchet_tests {

enum class FakeNetworkMode { Up = 0, TransportDown = 1, ServiceUnavailable = 2 };

class FakeNetworkSwitch {
  public:
    void Set(FakeNetworkMode m) { mode_.store(static_cast<int>(m)); }
    FakeNetworkMode Mode() const { return static_cast<FakeNetworkMode>(mode_.load()); }
    bool IsDown() const { return Mode() != FakeNetworkMode::Up; }
    /// The error a real client returns for the current outage.
    TrackerError MakeError() const {
        return Mode() == FakeNetworkMode::ServiceUnavailable ? TrackerErrorServer("fake network: HTTP 503", 503)
                                                             : TrackerErrorTransport("fake network: connection refused", 0);
    }
    /// Count a network-shaped call made while down (tests assert "no network while offline").
    void NoteCallWhileDown() { callsWhileDown_.fetch_add(1); }
    int CallsWhileDown() const { return callsWhileDown_.load(); }
    void ResetCounters() { callsWhileDown_.store(0); }

  private:
    std::atomic<int> mode_{0};
    std::atomic<int> callsWhileDown_{0};
};

inline FakeNetworkSwitch& GlobalFakeNetwork() {
    static FakeNetworkSwitch s;
    return s;
}

/// RAII: restores the global switch to Up and clears its counters when the test scope ends.
class ScopedFakeNetworkReset {
  public:
    ScopedFakeNetworkReset() { GlobalFakeNetwork().ResetCounters(); }
    ~ScopedFakeNetworkReset() {
        GlobalFakeNetwork().Set(FakeNetworkMode::Up);
        GlobalFakeNetwork().ResetCounters();
    }
    ScopedFakeNetworkReset(const ScopedFakeNetworkReset&) = delete;
    ScopedFakeNetworkReset& operator=(const ScopedFakeNetworkReset&) = delete;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_NETWORK_SWITCH_H
