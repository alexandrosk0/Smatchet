# Slice E2b — libFuzzer drivers for the 5 remaining untrusted-byte parsers (PR 2a + PR 2b)

**Status:** `shipped` — both PRs merged: **PR 2a (#1301)** + **PR 2b (#1307, merged 2026-06-16)**. All 5 drivers (`fuzz_cpp_lex`, `fuzz_callstack`, `fuzz_markdown_adf`, `fuzz_ai_sse`, `fuzz_ai_ndjson`) on develop; the Slice-E2 untrusted-byte-parser fuzz surface is complete.
**Branch / worktree:** PR 2a shipped on `feat/slice-e2b-libfuzzer` (merged #1301). **PR 2b** on `feat/slice-e2b-ai-fuzzers` @ `C:\Dev\trees\slice-e2b-aifz` — ships **off `develop`** (unstacked; 2a already merged so its branch is push-closed), not stacked on 2a's branch. See §10 Deviations.

### PR split (primary plan)

| PR | Drivers | Why this seam |
|---|---|---|
| **PR 2a** (this PR) | `fuzz_cpp_lex`, `fuzz_callstack`, `fuzz_markdown_adf` | The lexer + callstack + markdown surface. `markdown_adf` carries the `LIBS` helper extension (md4c + nlohmann), so the CMake change lands here. |
| **PR 2b** (next) | `fuzz_ai_sse`, `fuzz_ai_ndjson` | The streamed-AI-response surface (SSE + NDJSON), 2-chunk split-feed. Structurally depends on 2a's `LIBS` helper (`ndjson` needs nlohmann). |

Split keeps each PR at ~7-8 files — well under CR's per-PR ceiling.
**Roadmap:** `docs/guides/testing-surface.md` §6 P1 Gap 3 → `docs/plans/active/testing-surface-roadmap.md` step E2. `security.md` §59-60.
**Predecessor:** PR #1296 (E2a, **merged clean**) shipped the lane (`ninja-fuzzer-linux` preset, `cmake/Sanitizers.cmake` fuzzer branch, `tests/fuzz/` scaffold + `smatchet_add_fuzz_target` helper, advisory `fuzz-smoke.yml`) + the first driver `fuzz_image_dims`.

---

## 1. Goal

Add the **5 remaining** libFuzzer drivers over Smatchet's untrusted-byte parsers, completing Slice E2's stated surface (the 6 parsers in `security.md` §59-60). Shipped as **two PRs** (2a + 2b, table above). Each driver links its parser's **minimal ImGui-free `.cpp` link closure** (not the Core static lib — `SMATCHET_BUILD_APP=OFF` may not build it), registers a `-runs=0` smoke ctest, and ships 2-3 seed corpus inputs.

The merged scaffold's own `tests/fuzz/CMakeLists.txt` header anticipated the remaining five as "PR 2"; we split that into 2a + 2b for file-count headroom. **Not a scope deviation** — same surface, two slices.

### Parsers (all confirmed ImGui-free; signatures verified against `origin/develop`)

| Driver | Entry point | Header | Namespace |
|---|---|---|---|
| `fuzz_cpp_lex` | `CppLexLine(const char*, std::size_t)` (+ `CppLexLooksLikeCallstack`, `CppLexCallstackLine`) | `Ui/CppSyntaxLex.h` | **global** |
| `fuzz_markdown_adf` | `MarkdownConvert::MarkdownToAdf(const std::string&)` | `Ui/MarkdownConvert.h` | `MarkdownConvert` |
| `fuzz_ai_sse` | `AiSseParser::Feed(const char*, std::size_t, const EventCallback&)` | `AiSseParser.h` | **global** |
| `fuzz_ai_ndjson` | `AiNdjsonParser::Feed(const char*, std::size_t, const LineCallback&, const ErrorCallback&)` | `AiNdjsonParser.h` | **global** |
| `fuzz_callstack` | `ParseCallstackText(const std::string&)` | `CallstackParser.h` | **global** |

---

## 2. Link closures (the make-or-break detail)

Each driver's `SOURCES` = `fuzz_<name>.cpp` + the parser's minimal `.cpp` set. `INCLUDES` = the dirs that resolve those `.cpp`s' `#include`s. `LIBS` = compiled libraries that must link (object code or interface-propagated includes).

`${C}` = `${CMAKE_SOURCE_DIR}/Source/Core` (the helper already binds `_fuzz_core` to this).

| Driver | `.cpp` closure (under `${C}/src/`) | `LIBS` | Notes |
|---|---|---|---|
| `fuzz_cpp_lex` | `Ui/CppSyntaxLex.cpp` | — | Self-contained. `.cpp` includes only `Ui/CppSyntaxLex.h` + `<algorithm>` + `<cstring>`. No Logger. |
| `fuzz_markdown_adf` | `Ui/MarkdownConvert.cpp`, `Logger.cpp`, `Privacy/TextRedaction.cpp` | `md4c`, `nlohmann_json::nlohmann_json` | `.cpp` does `extern "C" { #include "md4c.h" }` → **must link `md4c`** (compiled C static lib, CMakeLists.txt:574, propagates its include dir PUBLIC). Logger pulls `Privacy/TextRedaction.h`. |
| `fuzz_ai_sse` | `AiSseParser.cpp`, `Logger.cpp`, `Privacy/TextRedaction.cpp` | — | `.cpp` includes `AiSseParser.h` + `Logger.h`. `LOG_ERROR` only on the 4 MiB cap-exceed path. SSE `Event` is plain strings — no nlohmann. |
| `fuzz_ai_ndjson` | `AiNdjsonParser.cpp`, `Logger.cpp`, `Privacy/TextRedaction.cpp` | `nlohmann_json::nlohmann_json` | `LineCallback` takes `const nlohmann::json&`; `.cpp` does `nlohmann::json::parse(line)` in try/catch. |
| `fuzz_callstack` | `CallstackParser.cpp` | — | Self-contained. `.cpp` includes `CallstackParser.h` + `<algorithm>` + `<cctype>` + `<regex>`. No Logger. |

**Shared Logger closure** = `Logger.cpp` + `Privacy/TextRedaction.cpp`. Defined once as a CMake list var (`_fuzz_logger_srcs`) to avoid repetition across sse/ndjson/markdown.

**Logger safety during fuzzing:** `Logger::Instance()` is a function-local static (lazy, thread-safe init) → `LOG_*` is valid with no explicit init. `LOG_ERROR` in SSE/NDJSON fires only on the 4 MiB buffer-cap path (libFuzzer default `max_len`=4096, so normally unreached). `TextRedaction.cpp` deps = `<regex>` + std only (ImGui-free). All verified.

`INCLUDES` for every driver: `${C}/include` + `${C}/include/Ui` (mirrors E2a; covers `Ui/*.h`, `Logger.h`, `Privacy/TextRedaction.h` via the `"Privacy/..."` prefix, `CallstackParser.h`, `AiSseParser.h`, `AiNdjsonParser.h`). md4c + nlohmann include dirs arrive transitively via `LIBS`.

### md4c availability under `BUILD_APP=OFF` (the one build risk — confirmed safe)

The md4c `FetchContent` block (CMakeLists.txt:552-580) sits **outside** any `BUILD_APP` guard: between `FetchContent_MakeAvailable(httplib)` (549) and ghc_filesystem (583), outside the cpr `if(SMATCHET_BUILD_APP)` bracket (465→501) and the GUI-surface gate (`if(SMATCHET_BUILD_APP)` @638). It is also linked by `tests/CMakeLists.txt:851` under `SMATCHET_BUILD_TESTS` (independent of `BUILD_APP`). → the `md4c` target exists under the fuzz preset (`BUILD_APP=OFF`, `BUILD_TESTS=ON`). `fuzz_markdown_adf` can link it.

---

## 3. CMake — one small helper extension

The shipped `smatchet_add_fuzz_target` (tests/fuzz/CMakeLists.txt) takes `SOURCES`/`INCLUDES`/`CORPUS` only — **no link-libraries path**. `fuzz_markdown_adf` (md4c) and `fuzz_ai_ndjson`/`fuzz_markdown_adf` (nlohmann) need one. Add an optional `LIBS` multi-value arg:

```cmake
function(smatchet_add_fuzz_target NAME)
    cmake_parse_arguments(FUZZ "" "CORPUS" "SOURCES;INCLUDES;LIBS" ${ARGN})   # +LIBS
    add_executable(${NAME} ${FUZZ_SOURCES})
    target_include_directories(${NAME} PRIVATE ${FUZZ_INCLUDES})
    if(FUZZ_LIBS)
        target_link_libraries(${NAME} PRIVATE ${FUZZ_LIBS})
    endif()
    target_compile_features(${NAME} PRIVATE cxx_std_14)
    smatchet_apply_sanitizers(${NAME})
    # ... (CORPUS ctest block unchanged)
endfunction()
```

Backward-compatible (existing `fuzz_image_dims` call has no `LIBS` → no-op). No preset / workflow edits — `fuzz-smoke.yml` auto-discovers `fuzz_*` binaries and `tests/fuzz/**`-path-triggers.

---

## 4. Driver bodies

All follow the E2a contract: total parsers, must tolerate empty/any bytes; ASan+UBSan (in the fuzzer set) catch OOB/UB. C++14 generic-lambda callbacks (`[](const auto&){}`) dodge fragile nested-type spelling — generic lambdas are C++14, **not** on the banned list.

**`fuzz_cpp_lex.cpp`** — exercises the lexer + the callstack-detect + callstack lexer on the same bytes:
```cpp
#include "Ui/CppSyntaxLex.h"
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* line = reinterpret_cast<const char*>(data);
    (void)CppLexLine(line, size);
    if (CppLexLooksLikeCallstack(line, size)) {
        (void)CppLexCallstackLine(line, size);
    }
    return 0;
}
```

**`fuzz_markdown_adf.cpp`**:
```cpp
#include "Ui/MarkdownConvert.h"
#include <cstddef>
#include <cstdint>
#include <string>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string md(reinterpret_cast<const char*>(data), size);
    (void)MarkdownConvert::MarkdownToAdf(md);
    return 0;
}
```

**`fuzz_ai_sse.cpp`** — split the input into two `Feed` calls (first byte picks the boundary) to exercise mid-frame buffer reassembly, the security-relevant code:
```cpp
#include "AiSseParser.h"
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    AiSseParser parser;
    const char* p = reinterpret_cast<const char*>(data);
    const size_t split = size ? (data[0] % (size + 1)) : 0;
    auto sink = [](const auto&) {};
    parser.Feed(p, split, sink);
    parser.Feed(p + split, size - split, sink);
    return 0;
}
```

**`fuzz_ai_ndjson.cpp`** — same 2-chunk split; two callbacks:
```cpp
#include "AiNdjsonParser.h"
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    AiNdjsonParser parser;
    const char* p = reinterpret_cast<const char*>(data);
    const size_t split = size ? (data[0] % (size + 1)) : 0;
    auto onLine  = [](const auto&) {};
    auto onError = [](const auto&) {};
    parser.Feed(p, split, onLine, onError);
    parser.Feed(p + split, size - split, onLine, onError);
    return 0;
}
```

**`fuzz_callstack.cpp`**:
```cpp
#include "CallstackParser.h"
#include <cstddef>
#include <cstdint>
#include <string>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    (void)ParseCallstackText(text);
    return 0;
}
```

> Headers will be re-confirmed at code time (`get_skeleton`/Read) for the exact `Event`/`LineCallback` typedefs + that the ctors are default — already verified `AiSseParser() = default` / `AiNdjsonParser() = default` from the headers.

---

## 5. Seed corpora (`tests/fuzz/corpus/<name>/`)

2-3 small, valid seeds each (keep file count under the CR per-PR ceiling). Seeds prime libFuzzer's coverage frontier; they are plain text written verbatim.

| Corpus | PR | Seeds (as committed) |
|---|---|---|
| `cpp_lex/` | 2a | `decl` (`int main(...)`), `mixed` (`"string"` + `'c'` + `// comment` + keyword + `0xDEAD`), `callstack_line` (addr + `Module!Sym::Fn() Line N`) |
| `callstack/` | 2a | `two_frames` (two stack frames), `with_addr` (frames with `0x...` addresses + `+ 0x..` offsets), `noise` (prose, not a callstack) |
| `markdown_adf/` | 2a | `headings` (ATX `#`/`##` + bold/italic/inline-code + blockquote), `table` (GFM pipe table), `fenced_tasks` (`- [ ]`/`- [x]` task list + `~~strike~~` + autolink + ```` ```cpp ```` block) |
| `ai_sse/` | 2b | `event` (`event: message\ndata: {"delta":"hi"}\n\n`), `multiline_data` (two `data:` lines + blank), `comment` (`: keepalive\n\n`) |
| `ai_ndjson/` | 2b | `two_objs` (`{"a":1}\n{"b":2}\n`), `partial` (one full line + an unterminated fragment, no trailing `\n`), `empty_lines` (blank lines between objects) |

---

## 6. Files to modify

**PR 2a (this PR):**

| Path | Action |
|---|---|
| `tests/fuzz/CMakeLists.txt` | EDIT — add `LIBS` to helper; add 3 target calls (`cpp_lex`, `callstack`, `markdown_adf`) + `_fuzz_logger_srcs` var |
| `tests/fuzz/fuzz_cpp_lex.cpp` | NEW |
| `tests/fuzz/fuzz_callstack.cpp` | NEW |
| `tests/fuzz/fuzz_markdown_adf.cpp` | NEW |
| `tests/fuzz/corpus/{cpp_lex,callstack,markdown_adf}/*` | NEW — 9 seed files (3/corpus) |
| `tests/fuzz/README.md` | EDIT — flip the 3 PR-2a Status cells to "E2b PR 2a (this PR)"; mark 2b rows "(next)" |
| `docs/plans/slice-e2b-libfuzzer.md` | NEW — this plan (now archived to `shipped/`) |

**PR 2b (next):**

| Path | Action |
|---|---|
| `tests/fuzz/CMakeLists.txt` | EDIT — add 2 target calls (`ai_sse`, `ai_ndjson`, each w/ `include/Privacy`, no `include/Ui`); **+ add `include/Privacy` to `fuzz_markdown_adf`** (repairs develop's red lane — §10 D1). `LIBS` helper already present from 2a |
| `tests/fuzz/fuzz_ai_sse.cpp` | NEW |
| `tests/fuzz/fuzz_ai_ndjson.cpp` | NEW |
| `tests/fuzz/corpus/{ai_sse,ai_ndjson}/*` | NEW — 6 seed files |
| `tests/fuzz/README.md` | EDIT — flip the 2 PR-2b Status cells to "this PR"; flip the 3 PR-2a cells to "(#1301)" |

---

## 7. Build / verification — **CI is the sole validator**

Windows dev box **cannot** build these (`cmake/Sanitizers.cmake` FATALs on MSVC/clang-cl; the lane is native-clang-on-Linux). The advisory `fuzz-smoke.yml` (ubuntu) is the only build+smoke proof.

- **Local (Windows):** lint gates only — `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (C++14 + comment-noise + function-size; the new `.cpp`s are tiny). No local compile possible.
- **CI (`fuzz-smoke.yml`):** auto-discovers the 5 new `fuzz_*` binaries → builds each under `ninja-fuzzer-linux` → `-runs=0` smoke (loads each seed corpus, runs libFuzzer init, exits 0) → 45 s time-boxed PR fuzz run. **Green here = the closures link + the parsers survive their seeds.** This is the gate that actually exercises the work.
- The lane is **advisory** (not on the develop merge-gate); a red here blocks merge by judgement, not by required-check. Per "never merge past ANY red check," a red fuzz-smoke is investigated before merge regardless.

---

## 8. Perf gate

**N/A.** The diff adds `tests/fuzz/` only; it does not modify any `Source/Core/` runtime code (drivers *compile* existing `.cpp` closures, never edit them). No steady-state UI path touched → no perf-run required.

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| A closure misses a transitive `.cpp` → link error (undefined symbol) | First CI run surfaces it; add the missing `.cpp`. Closures are minimal-by-design; Logger/TextRedaction pre-verified for sse/ndjson/markdown. |
| `MarkdownToAdf` or a parser is **not** total (crashes on adversarial bytes) | That is exactly the bug class the fuzzer exists to find — a real finding, filed as a GitHub Issue + regression seed, fixed by the owning specialist (md4c wrapper → ui-host/`p4-annotate` lineage; not auto-fixed). Not a plan failure. |
| File count trips CR's per-PR ceiling | **Resolved by the 2a/2b split** (primary plan, table at top). Each PR is ~7-8 files (3 drivers + 9 seeds + CMake/README for 2a; 2 drivers + 6 seeds for 2b) — well under the ceiling. |
| md4c unavailable under `BUILD_APP=OFF` | **Disproven** (§2): block is ungated + tests link it under `BUILD_TESTS`. |

---

## 10. Deviations log

- **D1 — `include/Privacy` was missing; PR 2a shipped a red `fuzz-smoke`.** §2/§55 assumed `Logger.cpp`'s `"Privacy/TextRedaction.h"` prefix made `include` + `include/Ui` sufficient for the shared logger closure. Wrong: `TextRedaction.cpp` itself bare-includes `"TextRedaction.h"` (header at `include/Privacy/TextRedaction.h`), so **every target that compiles `_fuzz_logger_srcs` needs `include/Privacy` on its `INCLUDES`.** PR 2a's `fuzz_markdown_adf` lacked it → `fatal error: 'TextRedaction.h' file not found` → 3 consecutive red `fuzz-smoke` runs. #1301 was merged past the red advisory lane (manual human merge — gate-escape, postmortem owed in `docs/self-improvement/postmortems.md`). **PR 2b folds the fix in:** adds `include/Privacy` to `fuzz_markdown_adf` (repairs develop's red lane) and to both new ai_* targets.
- **D2 — PR 2b ships off `develop`, unstacked.** Header envisaged 2b stacking on 2a's branch. Because 2a already squash-merged (#1301) and its branch is merged-closed (push blocked by the merged-PR guard), 2b is built fresh in a new worktree (`feat/slice-e2b-ai-fuzzers` off `origin/develop`) — all non-ai files byte-identical to develop, so the 2b diff is `ai_*` + the one-line markdown hotfix only.
- **D3 — ai_* targets omit `include/Ui`.** §55 specified `include` + `include/Ui` for every driver. The two ai parsers and the logger srcs touch nothing under `Ui/`, so the ai_* targets carry only `include` + `include/Privacy`. (`fuzz_markdown_adf` keeps `include/Ui` for `MarkdownConvert`.)

## 11. Self-improvement

- Closeout (2026-06-16): both PRs landed clean after the D1 `include/Privacy` fix folded into PR 2b. No new friction beyond the gate-escape already logged for #1301 (PR 2a merged past a red advisory `fuzz-smoke` lane — postmortem owed in `docs/self-improvement/postmortems.md`).
