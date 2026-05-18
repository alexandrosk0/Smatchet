// Test helper: stand-in for `gh` (GitHub CLI) used by the H6 PR-open
// fallback path in tests/Source_Core/ClaudeCodeLocalRunner.test.cpp. The
// runner shells out to `gh pr create --draft --base <X> --title <T>
// --body <B>` after a successful `git push`; this stub answers without
// touching the GitHub API.
//
// Behavioural contract:
//   - `gh --version`             → exit 0 + version line on stdout
//   - `gh pr create <...>`       → exit 0 + a fake PR URL on stdout. The
//                                  URL is "https://example.com/pr/42"
//                                  unless STUB_GH_URL overrides it.
//   - any other command          → exit 0 + empty stdout (best-effort)
//
// Failure modes via STUB_GH_MODE:
//   - default        → success path with URL on stdout
//   - "create-fail"  → `gh pr create` exits 1 with stderr message
//   - "bad-url"      → `gh pr create` exits 0 but prints a non-PR URL so
//                      the runner's URL validator rejects it
//
// stdlib-only — same constraints as stub_claude.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

const char* GetenvOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : fallback;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "stub-gh: no subcommand\n";
        return 0;
    }
    const std::string sub = argv[1];
    const std::string mode = GetenvOr("STUB_GH_MODE", "default");

    if (sub == "--version") {
        std::cout << "gh version 2.0.0 (stub)\n";
        return 0;
    }
    if (sub == "pr" && argc >= 3 && std::strcmp(argv[2], "create") == 0) {
        if (mode == "create-fail") {
            std::cerr << "stub-gh: simulated pr-create failure (auth required)\n";
            return 1;
        }
        if (mode == "bad-url") {
            // Print something that does not satisfy LooksLikeGithubPrUrl —
            // e.g. an issue URL or plain text. The runner's URL validator
            // must reject this and surface Failed.
            std::cout << "https://example.com/issues/1\n";
            return 0;
        }
        const char* urlOverride = GetenvOr("STUB_GH_URL", "https://example.com/pr/42");
        std::cout << urlOverride << "\n";
        return 0;
    }
    return 0;
}
