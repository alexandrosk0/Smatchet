#ifndef SMATCHET_TESTS_FAKE_P4_RUNNER_H
#define SMATCHET_TESTS_FAKE_P4_RUNNER_H

// FakeP4Runner — scripted, in-process replacement for the `p4` CLI used by
// `Source/Core/src/P4Annotate.cpp:P4RunCommand`. Slice 3 of
// docs/plans/shipped/autonomous-debugging-no-creds.md.
//
// Loads canned responses from a JSON fixture under tests/fixtures/p4/ keyed by an
// "argv-prefix" string (whitespace-joined first-N args; the test fixture's
// argv_prefix matches any P4RunCommand call whose joined args _start with_ that
// string). Returns a `AnnotateAnalysisConfig::P4RunCommandFn` the test installs
// directly onto `AnnotateAnalysisConfig::P4RunOverride`. No subprocess spawn, no
// PATH dance, no real p4 server.
//
// Header-only; tests/support/ is already on the test rig include path.

#include "ConfigManager.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace smatchet_tests {

struct FakeP4Response {
    std::string ArgvPrefix;
    int ExitCode = 0;
    std::string Stdout;
    std::string Stderr;
};

/// Header-only loader + matcher. Construct from a fixture path; install via
/// `cfg.P4RunOverride = runner.AsCallback();`.
class FakeP4Runner {
  public:
    FakeP4Runner() = default;

    explicit FakeP4Runner(const std::string& fixturePath) { LoadFromFile(fixturePath); }

    /// Parse the fixture JSON and replace the response table.
    void LoadFromFile(const std::string& fixturePath) {
        std::ifstream in(fixturePath);
        if (!in.is_open()) {
            throw std::runtime_error("FakeP4Runner: cannot open fixture: " + fixturePath);
        }
        nlohmann::json j;
        in >> j;
        std::lock_guard<std::mutex> lock(mutex_);
        responses_.clear();
        if (!j.contains("responses") || !j["responses"].is_array()) {
            throw std::runtime_error("FakeP4Runner: fixture missing 'responses' array: " + fixturePath);
        }
        for (const auto& entry : j["responses"]) {
            FakeP4Response r;
            r.ArgvPrefix = entry.value("argv_prefix", std::string());
            r.ExitCode = entry.value("exit_code", 0);
            r.Stdout = entry.value("stdout", std::string());
            r.Stderr = entry.value("stderr", std::string());
            responses_.push_back(std::move(r));
        }
    }

    /// Inline fixture installation (used by tests that want a tiny scripted table
    /// without a fixture file on disk).
    void AddResponse(const FakeP4Response& r) {
        std::lock_guard<std::mutex> lock(mutex_);
        responses_.push_back(r);
    }

    /// Number of times the override was invoked (across all argv-prefixes).
    int CallCount() const { return callCount_.load(); }

    /// Build the std::function the test installs onto `cfg.P4RunOverride`. The
    /// returned lambda captures `this` — keep the FakeP4Runner alive for the test
    /// duration. Thread-safe.
    AnnotateAnalysisConfig::P4RunCommandFn AsCallback() {
        return [this](const std::vector<std::string>& args, int& outExit, std::string& outStdout,
                      std::string& outStderr) -> bool {
            callCount_.fetch_add(1);
            const std::string joined = JoinArgs(args);
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& r : responses_) {
                if (PrefixMatches(joined, r.ArgvPrefix)) {
                    outExit = r.ExitCode;
                    outStdout = r.Stdout;
                    outStderr = r.Stderr;
                    // exit_code = -1 sentinel encodes spawn-failed / timeout
                    // (matches P4RunCommand's `return false` shape).
                    if (r.ExitCode == -1) {
                        return false;
                    }
                    return true;
                }
            }
            // Unmatched call — model as spawn failure with diagnostic stderr so
            // tests that hit an un-scripted argv produce a recognizable error.
            outExit = -1;
            outStdout.clear();
            outStderr = "FakeP4Runner: no scripted response for argv: " + joined;
            return false;
        };
    }

  private:
    static std::string JoinArgs(const std::vector<std::string>& args) {
        std::string out;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i != 0) {
                out += ' ';
            }
            out += args[i];
        }
        return out;
    }

    static bool PrefixMatches(const std::string& joined, const std::string& prefix) {
        if (prefix.empty()) {
            return false;
        }
        if (joined.size() < prefix.size()) {
            return false;
        }
        if (joined.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        // Whole-arg prefix: either exact match or next char is a whitespace
        // boundary so "annotate" doesn't accidentally match "annotateX".
        if (joined.size() == prefix.size()) {
            return true;
        }
        return joined[prefix.size()] == ' ';
    }

    mutable std::mutex mutex_;
    std::vector<FakeP4Response> responses_;
    std::atomic<int> callCount_{0};
};

} // namespace smatchet_tests

#endif
