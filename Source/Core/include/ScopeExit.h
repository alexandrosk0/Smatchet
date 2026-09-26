#pragma once

// ScopeExit — runs a callable when the scope ends, on every exit path including a throw. Used to
// clear in-flight latches so a failure can never leave a loader stuck (Quality Pillar 6). The
// callable must not throw.

#include <functional>
#include <utility>

namespace smatchet {

class ScopeExit {
  public:
    explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~ScopeExit() {
        if (fn_) {
            fn_();
        }
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

  private:
    std::function<void()> fn_;
};

} // namespace smatchet
