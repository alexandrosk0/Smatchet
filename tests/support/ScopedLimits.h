#pragma once

// ScopedLimits — RAII for tests that change the process-wide smatchet::limits for their duration.
// Snapshots the published limits on construction and restores them on destruction, so a failed
// REQUIRE (which throws) still leaves later tests with the limits they expect.

#include "TunableLimits.h"

#include <functional>

namespace smatchet_tests {

class ScopedLimits {
  public:
    ScopedLimits() : saved_(smatchet::limits::Get()) {}
    explicit ScopedLimits(const std::function<void(smatchet::limits::LimitsConfig&)>& mutate)
        : saved_(smatchet::limits::Get()) {
        Update(mutate);
    }
    ScopedLimits(const ScopedLimits&) = delete;
    ScopedLimits& operator=(const ScopedLimits&) = delete;
    ~ScopedLimits() { smatchet::limits::Set(saved_); }

    /// Apply `mutate` to the currently published limits and publish the result (clamped by Set()).
    void Update(const std::function<void(smatchet::limits::LimitsConfig&)>& mutate) {
        smatchet::limits::LimitsConfig next = smatchet::limits::Get();
        mutate(next);
        smatchet::limits::Set(next);
    }

  private:
    smatchet::limits::LimitsConfig saved_;
};

} // namespace smatchet_tests
