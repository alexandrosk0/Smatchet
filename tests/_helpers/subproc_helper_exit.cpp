// Test helper: exits with the requested code.
//
// Used by tests/Source_Core/SubprocessCapture.test.cpp to cover the
// exit-code-propagation path deterministically without depending on
// platform shell builtins (`true` / `false` / `exit N`). Single TU,
// stdlib-only — keep it that way so the build cost stays trivial.

#include <cstdlib>

int main(int argc, char** argv) {
    if (argc >= 2) {
        return std::atoi(argv[1]);
    }
    return 0;
}
