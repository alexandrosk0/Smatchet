// Fixture for agents/scripts/core/test-lint-hook-split.sh. Pre-formatted (clang-format
// clean) and cppcheck-clean so the inline hook's `clang-format -i` is a no-op
// and the drain reports no findings.
//
// Do NOT include this file in any production build target — it lives under
// tests/fixtures/ which is not part of Source/Core / Plugins / Source/Standalone
// or the doctest rig. The deferred-lint hook's first-party filter accepts it
// (matches `tests/**/*.cpp`) which is the whole point.
//
// If you need to extend the test driver, copy this file and tweak the copy —
// never widen this file's surface, or test failures will couple to unrelated
// edits.

namespace smatchet_lint_probe {

int Multiply(int a, int b) { return a * b; }

} // namespace smatchet_lint_probe
