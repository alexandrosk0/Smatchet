// Test helper: sleeps for argv[1] milliseconds then exits 0.
//
// Used by tests/Core/SubprocessCapture.test.cpp to cover the
// timeout-fires and cancel-token paths without depending on platform
// shell builtins (`sleep`). Cross-platform via std::this_thread::sleep_for.

#include <chrono>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    int ms = 0;
    if (argc >= 2) {
        ms = std::atoi(argv[1]);
    }
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    return 0;
}
