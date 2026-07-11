#ifndef SMATCHET_TESTS_HTTP_REQUEST_CAPTURE_H
#define SMATCHET_TESTS_HTTP_REQUEST_CAPTURE_H

// coverage-gap-tier2-backend-shell-fixtures.md — thread-safe capture of the request
// bodies a backend shell actually sends to the loopback fixture. Handlers run on the
// httplib server thread; doctest assertions are NOT thread-safe, so tests Push() from
// the handler and assert on a Snapshot() taken on the test thread after the call
// under test returns. Shared by the Linear / GitHub (and future) fixture suites.

#include <mutex>
#include <string>
#include <vector>

namespace smatchet_tests {

struct HttpRequestCapture {
    std::mutex Mutex;
    std::vector<std::string> Bodies;

    void Push(const std::string& body) {
        std::lock_guard<std::mutex> lock(Mutex);
        Bodies.push_back(body);
    }
    std::vector<std::string> Snapshot() {
        std::lock_guard<std::mutex> lock(Mutex);
        return Bodies;
    }
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_HTTP_REQUEST_CAPTURE_H
