// Test helper: writes argv[1] bytes of 'A' to stdout, then exits 0.
//
// Used by tests/Core/SubprocessCapture.test.cpp to cover the
// stdout-byte-cap truncation path deterministically. Single TU,
// stdlib-only.

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    long long n = 0;
    if (argc >= 2) {
        n = std::atoll(argv[1]);
    }
    // Buffer-flush is fine — the runner's read loop happens on a
    // separate process, so even synchronous putc here is observable.
    for (long long i = 0; i < n; ++i) {
        std::fputc('A', stdout);
    }
    std::fflush(stdout);
    return 0;
}
