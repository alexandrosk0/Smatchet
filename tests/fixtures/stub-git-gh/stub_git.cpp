// Test helper: stand-in for `git` used by the H6 PR-open fallback path in
// tests/Source_Core/ClaudeCodeLocalRunner.test.cpp. The runner shells out to
// `git push -u origin <branch>` + `git log -1 --pretty=%s` in the fallback
// path; this stub answers both without performing real version-control work.
//
// Behavioural contract:
//   - `git --version`            → exit 0 + version line on stdout
//   - `git push <...>`           → exit 0 + "pushed <ref>" on stdout
//   - `git log -1 --pretty=%s`   → exit 0 + a synthetic commit subject so the
//                                  runner's first-line-of-last-commit title
//                                  heuristic has something to bite on
//   - any other command          → exit 0 + empty stdout (best-effort)
//
// Failure modes are selectable via env STUB_GIT_MODE:
//   - default     → success path
//   - "push-fail" → `git push` exits 1 with stderr message (test the
//                   fallback-failure branch on push)
//
// stdlib-only — same constraints as stub_claude.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char* GetenvOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : fallback;
}

bool ArgvContains(int argc, char** argv, const char* token) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], token) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        // No subcommand — emit `git`'s usage-style line and exit 0 so the
        // runner's probe-shaped invocations (`git --version`) do not bias the
        // dispatch when run without arguments. Real git exits 1 here; the
        // stub is more lenient.
        std::cout << "stub-git: no subcommand\n";
        return 0;
    }

    const std::string sub = argv[1];
    const std::string mode = GetenvOr("STUB_GIT_MODE", "default");

    if (sub == "--version") {
        std::cout << "git version 2.99.0 (stub)\n";
        return 0;
    }
    if (sub == "push") {
        if (mode == "push-fail") {
            std::cerr << "stub-git: simulated push failure (remote rejected)\n";
            return 1;
        }
        std::string ref;
        for (int i = 2; i < argc; ++i) {
            if (argv[i][0] != '-' && std::strcmp(argv[i], "origin") != 0) {
                ref = argv[i];
                break;
            }
        }
        std::cout << "pushed " << ref << "\n";
        return 0;
    }
    if (sub == "log") {
        // Best-effort: emit a synthetic subject so the runner's title
        // heuristic picks it up. Real git would honour --pretty=%s; we
        // unconditionally emit a constant since the test does not depend
        // on the exact subject text.
        (void)ArgvContains;
        std::cout << "stub: synthetic commit subject\n";
        return 0;
    }
    // Any other subcommand — succeed silently. The fallback path only uses
    // push + log; future stub callers extend this switch as needed.
    return 0;
}
