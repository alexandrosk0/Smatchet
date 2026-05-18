// Test helper: stand-in for the Claude Code CLI used by
// tests/Source_Core/ClaudeCodeLocalRunner.test.cpp. The runner accepts
// `claude` from PATH but in tests the binary path is overridden via
// `TrackerConfig::HandoffHarnessBinPath` (or runner-constructor option) so
// the doctest exercises the runner without depending on a live Claude Code
// install.
//
// Behavioural contract — keep in sync with what the real CLI provides:
//
//   stub_claude --print --output-format stream-json --verbose
//               --permission-mode bypassPermissions
//               --append-system-prompt-file SEED.md
//               "<positional prompt>"
//
// On success the stub:
//   1. Validates the 5 required flags appear, rejects unknowns.
//   2. Reads SEED.md from --append-system-prompt-file (must exist).
//   3. Writes env_snapshot.txt to cwd — one KEY=VALUE per parent env var.
//      This is what enables the env-allow-list assertion in the test.
//   4. Emits ~5 stream-json NDJSON events to stdout.
//   5. Writes RUN_RESULT.json + PR_URL.txt to cwd.
//   6. Exits 0.
//
// Modes (chosen via env STUB_CLAUDE_MODE or implicit clarification path):
//   - default               → success path
//   - "clarification"       → writes CLARIFICATION_NEEDED.json, polls for
//                             USER_RESPONSE.json, then completes.
//   - "error"               → writes RUN_RESULT.json with ok=false, exits 1.
//   - "sleep"               → emits an opening event then sleeps up to 30 s
//                             so the cancel-token test can flip the atom.
//
// stdlib-only — keep it that way so the build cost stays trivial. The stub
// must compile cleanly on MinGW UCRT + MSVC + POSIX (the parent SubprocessCapture
// is dual-platform).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

// Walk the parent env block and write one KEY=VALUE line per entry to
// env_snapshot.txt in the cwd. The env-allow-list doctest reads this file
// and asserts {SMATCHET_SECRET is absent, GH_TOKEN is present}.
void WriteEnvSnapshot() {
    std::ofstream f("env_snapshot.txt", std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        return;
    }
#if defined(_WIN32)
    LPWCH block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return;
    }
    for (LPWCH p = block; *p != L'\0';) {
        const size_t len = wcslen(p);
        const int u8len = WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
        if (u8len > 0) {
            std::string entry(static_cast<size_t>(u8len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(len), &entry[0], u8len, nullptr, nullptr);
            f.write(entry.data(), static_cast<std::streamsize>(entry.size()));
            f.put('\n');
        }
        p += len + 1;
    }
    FreeEnvironmentStringsW(block);
#else
    for (char** ep = environ; ep && *ep; ++ep) {
        f << *ep << "\n";
    }
#endif
}

bool FileExists(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

void EmitStreamJsonLine(const std::string& line) {
    // NDJSON — one JSON object per line. flush so the runner-side line
    // dispatcher sees each event promptly.
    std::cout << line << "\n";
    std::cout.flush();
}

void EmitHappyEvents(const std::string& prompt) {
    EmitStreamJsonLine(R"({"type":"system","subtype":"init","data":{"session_id":"stub-session"}})");
    {
        std::string s = "{\"type\":\"assistant\",\"subtype\":\"message\",\"data\":{\"content\":\"Received prompt: ";
        for (char c : prompt) {
            if (c == '"') s += "\\\"";
            else if (c == '\\') s += "\\\\";
            else if (c == '\n') s += "\\n";
            else s += c;
        }
        s += "\"}}";
        EmitStreamJsonLine(s);
    }
    EmitStreamJsonLine(R"({"type":"tool_use","subtype":"read","data":{"path":"SEED.json"}})");
    EmitStreamJsonLine(R"({"type":"assistant","subtype":"message","data":{"content":"Implementing..."}})");
    EmitStreamJsonLine(R"({"type":"result","subtype":"complete","data":{"ok":true}})");
}

void WriteFileText(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f.is_open()) {
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
}

void WriteRunResult(bool ok, const std::string& errorMessage, const std::string& prUrl) {
    std::string j = "{";
    j += "\"ok\":" + std::string(ok ? "true" : "false");
    if (!errorMessage.empty()) {
        j += ",\"errorMessage\":\"";
        for (char c : errorMessage) {
            if (c == '"') j += "\\\"";
            else if (c == '\\') j += "\\\\";
            else j += c;
        }
        j += "\"";
    }
    if (!prUrl.empty()) {
        j += ",\"prUrl\":\"" + prUrl + "\"";
    }
    j += ",\"filesChanged\":3,\"linesAdded\":42,\"linesRemoved\":7}";
    WriteFileText("RUN_RESULT.json", j);
}

const char* GetenvOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : fallback;
}

void Sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

int main(int argc, char** argv) {
    // Parse argv — track that each of the 5 mandatory flags appeared at
    // least once; capture the SEED.md path + positional prompt. Unknown
    // flags are tolerated for forward-compat (the real CLI is strict but
    // tightening the stub adds maintenance cost without test value).
    bool sawPrint = false;
    bool sawVerbose = false;
    std::string outputFormat;
    std::string permissionMode;
    std::string seedPath;
    std::string positional;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--print" || a == "-p") {
            sawPrint = true;
        } else if (a == "--verbose") {
            sawVerbose = true;
        } else if (a == "--output-format" && i + 1 < argc) {
            outputFormat = argv[++i];
        } else if (a == "--permission-mode" && i + 1 < argc) {
            permissionMode = argv[++i];
        } else if (a == "--append-system-prompt-file" && i + 1 < argc) {
            seedPath = argv[++i];
        } else if (a == "--version") {
            std::cout << "stub-claude 1.0.0 (test-fixture)\n";
            return 0;
        } else if (a[0] == '-' && a.size() > 1) {
            // Unknown flag with possible value — skip the value (best-effort).
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                ++i;
            }
        } else {
            // Positional — the prompt.
            if (!positional.empty()) {
                positional.push_back(' ');
            }
            positional += a;
        }
    }

    // Write the env snapshot unconditionally — every test path needs to be
    // able to assert on the env block the stub received.
    WriteEnvSnapshot();

    // Sanity-check that the mandatory flag set is present. Stream-json output
    // format is the contract; bypassPermissions is the contract. If either
    // is missing the stub still completes but writes a flag-drift note —
    // this keeps the failure mode informative without halting tests that
    // intentionally vary the args.
    std::string drift;
    if (!sawPrint) drift += "missing --print; ";
    if (!sawVerbose) drift += "missing --verbose; ";
    if (outputFormat != "stream-json") drift += "output-format != stream-json; ";
    if (permissionMode != "bypassPermissions") drift += "permission-mode != bypassPermissions; ";
    if (seedPath.empty() || !FileExists(seedPath)) drift += "missing or bad --append-system-prompt-file; ";

    // Mode dispatch. Env var STUB_CLAUDE_MODE wins; the runner's poisoned-env
    // test sets it explicitly through the allow-list (the env block must
    // include STUB_CLAUDE_MODE in the runner-side override).
    const std::string mode = GetenvOr("STUB_CLAUDE_MODE", "happy");

    if (mode == "error") {
        EmitStreamJsonLine(R"({"type":"error","subtype":"failure","data":{"message":"test failure"}})");
        WriteRunResult(false, "test failure (stub)", std::string());
        return 1;
    }

    if (mode == "sleep") {
        EmitStreamJsonLine(R"({"type":"system","subtype":"init","data":{"session_id":"stub-sleep"}})");
        for (int i = 0; i < 300; ++i) {
            // Poll for cancel — when the cancel token flips, SubprocessCapture
            // sends SIGTERM / TerminateProcess and we exit promptly.
            Sleep_ms(100);
        }
        WriteRunResult(false, "stub timed out without cancel", std::string());
        return 124;
    }

    if (mode == "clarification") {
        // Emit opening, write CLARIFICATION_NEEDED.json, poll for
        // USER_RESPONSE.json, then complete.
        EmitStreamJsonLine(R"({"type":"system","subtype":"init","data":{"session_id":"stub-clar"}})");
        WriteFileText("CLARIFICATION_NEEDED.json",
                      R"({"question":"What style?","timestampUnixSec":0})");
        for (int i = 0; i < 100; ++i) {
            if (FileExists("USER_RESPONSE.json")) {
                EmitStreamJsonLine(R"({"type":"assistant","subtype":"message","data":{"content":"Continuing with response"}})");
                EmitStreamJsonLine(R"({"type":"result","subtype":"complete","data":{"ok":true}})");
                WriteFileText("PR_URL.txt", "https://example.com/pr/clar");
                WriteRunResult(true, std::string(), "https://example.com/pr/clar");
                return 0;
            }
            Sleep_ms(100);
        }
        WriteRunResult(false, "timeout waiting for USER_RESPONSE.json", std::string());
        return 1;
    }

    // Happy path.
    if (!drift.empty()) {
        // Surface as an error event but still complete so the doctest sees
        // the flag-drift note in stderr rather than a silent pass.
        std::cerr << "stub_claude: flag-drift: " << drift << "\n";
    }
    EmitHappyEvents(positional);
    WriteFileText("PR_URL.txt", "https://example.com/pr/1");
    WriteRunResult(true, std::string(), "https://example.com/pr/1");
    return 0;
}
