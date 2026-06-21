# tests/fuzz — libFuzzer harness

Coverage-guided fuzz drivers over Smatchet's **untrusted-byte parsers** — the
functions that decode attacker-controllable input (downloaded attachment bytes,
streamed AI responses, pasted callstacks). Slice E2 of the testing-surface
roadmap (`docs/guides/testing-surface.md` §6 P1 Gap 3; `security.md` §59-60).

## How it builds

Drivers compile **only** under the `ninja-fuzzer-linux` preset
(`SMATCHET_SANITIZER=fuzzer`) — native Clang on Linux with
`-fsanitize=fuzzer,address,undefined`. `-fsanitize=fuzzer` both instruments
coverage and links libFuzzer's `main()`, which repeatedly calls each driver's
`LLVMFuzzerTestOneInput`. `SMATCHET_BUILD_APP=OFF` skips GLFW/ImGui/Lua/whisper so
no X11/OpenGL `-dev` packages are needed (the parsers are ImGui-free Core).

The preset is **Clang-on-Linux only**: `cmake/Sanitizers.cmake` FATALs on MSVC /
clang-cl. On a Windows dev box you cannot build these locally — the CI lane
(`.github/workflows/fuzz-smoke.yml`, ubuntu) is the build validator. The lane is
**advisory** (not on the `develop` merge-gate).

```bash
# Linux clang toolchain:
sudo apt-get install -y clang lld ninja-build
cmake --preset ninja-fuzzer-linux
cmake --build --preset ninja-fuzzer-linux        # builds every fuzz_* driver
ctest --preset ninja-fuzzer-linux                # -runs=0 smoke (loads seeds, exits 0)
# Equivalent non-preset form fuzz-smoke.yml uses (cd into the build dir):
#   ( cd build/ninja-fuzzer-linux && ctest --output-on-failure )

# Real time-boxed run of one driver:
./build/ninja-fuzzer-linux/tests/fuzz/fuzz_image_dims \
    tests/fuzz/corpus/image_dims -max_total_time=60
```

## Targets

| Driver | Parser under test | Status |
|---|---|---|
| `fuzz_image_dims` | `image_dim::ParseImageDimensionsFromBytes` (PNG/GIF/WEBP/JPEG headers) | E2a (shipped, #1296) |
| `fuzz_cpp_lex` | `CppLexLine` / `CppLexCallstackLine` (annotate syntax highlighter) | E2b PR 2a (#1301) |
| `fuzz_callstack` | `ParseCallstackText` (pasted stack frames) | E2b PR 2a (#1301) |
| `fuzz_markdown_adf` | `MarkdownConvert::MarkdownToAdf` (md4c) | E2b PR 2a (#1301) |
| `fuzz_ai_sse` | `AiSseParser::Feed` (SSE stream) | E2b PR 2b (this PR) |
| `fuzz_ai_ndjson` | `AiNdjsonParser::Feed` (NDJSON stream) | E2b PR 2b (this PR) |

## Adding a driver

1. Write `fuzz_<name>.cpp` with `extern "C" int LLVMFuzzerTestOneInput(const
   uint8_t* data, size_t size)` — build a value from `data`/`size`, call the
   parser, return 0. Never assume `size > 0`; the parser must tolerate empty.
2. Add a `smatchet_add_fuzz_target(fuzz_<name> SOURCES ... INCLUDES ... CORPUS
   <name>)` call in `CMakeLists.txt` (link the parser's minimal ImGui-free .cpp
   closure).
3. Drop a few valid seed inputs in `corpus/<name>/`. The CI lane auto-discovers
   any `fuzz_*` binary — no workflow edit needed.

## Corpus

`corpus/<target>/` holds seed inputs that prime libFuzzer's coverage frontier.
Seeds are committed (small, valid examples of the format); crashes found in CI
are uploaded as artifacts and should be added as regression seeds once fixed.
