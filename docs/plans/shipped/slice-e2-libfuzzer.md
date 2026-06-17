# Slice E2 — libFuzzer harness for the untrusted-byte parsers

> **Status:** `shipped` — all 6 fuzz targets on develop. E2a (harness + `fuzz_image_dims`) via #1296; the remaining 5 drivers shipped under the child plan [`slice-e2b-libfuzzer.md`](slice-e2b-libfuzzer.md) (PR 2a #1301 + PR 2b #1307). Surface complete.

| Field | Value |
|---|---|
| Slice | **E2** of [`testing-surface-roadmap.md`](testing-surface-roadmap.md) (§6 P1, Gap 3) |
| Type | **plan** (review → code on approval) |
| Effort | L |
| Risk | Low (pure-additive test binaries + advisory CI; no production code path changes) |
| Depends on | **E1** (PNG-dims cap test — done) |
| Source | [`docs/security/*`] `security.md:59-60` (libFuzzer is the superset of the E1 hand-crafted cap test) |
| Branch | `feat/slice-e2-libfuzzer` |

## 1. Goal

Stand up a **continuous-fuzzing harness** over the six functions that parse
*untrusted bytes* in Smatchet, so malformed input that today only gets the one or
two hand-crafted unit cases instead gets millions of coverage-guided mutations
under ASan + UBSan. A crash (OOB read, UB, unbounded alloc, infinite loop) becomes
a reproducible artifact + an auto-filed issue instead of a field crash.

This is the **superset** of the E1 PNG-dimension cap test: E1 pins a handful of
boundary bytes by hand; E2 lets libFuzzer *find* the boundaries.

## 2. The six fuzz targets

Every one is a pure, ImGui-free, cpr-free function that consumes attacker-influenced
bytes. The link closure column is taken from the existing doctest rig
(`tests/CMakeLists.txt`) — these are the exact `.cpp` files each target needs.

| # | Entry point | Header | Link closure (`.cpp`) | Untrusted surface |
|---|---|---|---|---|
| 1 | `image_dim::ParseImageDimensionsFromBytes` | `Ui/ImageDimensionsPure.h` | `Ui/ImageDimensionsPure.cpp` | PNG/GIF/WEBP-VP8X/JPEG-SOF header bytes off disk |
| 2 | `CppLexLine` | `Ui/CppSyntaxLex.h` | `Ui/CppSyntaxLex.cpp` | annotate / callstack source lines (non-NUL-terminated `(ptr,len)`) |
| 3 | `MarkdownConvert::MarkdownToAdf` | `Ui/MarkdownConvert.h` | `Ui/MarkdownConvert.cpp` (+ md4c, nlohmann) | AI-assistant / Jira markdown → ADF |
| 4 | `AiSseParser::Feed` | `AiSseParser.h` | `AiSseParser.cpp` | streaming SSE bytes from the AI provider, arbitrarily split |
| 5 | `AiNdjsonParser::Feed` | `AiNdjsonParser.h` | `AiNdjsonParser.cpp` (+ nlohmann) | streaming NDJSON from Ollama, arbitrarily split |
| 6 | `ParseCallstackText` | `CallstackParser.h` | `CallstackParser.cpp` | pasted / `p4`-sourced crash callstacks |

All six are already unit-tested (`tests/Core/{ParseImageDimensions,CppSyntaxLex,MarkdownConvert,AiSseParser,AiNdjsonParser,CallstackParser}.test.cpp`),
so the seam is proven decoupled — the fuzz driver is a thin wrapper over the same call.

## 3. Key decision (for reviewer) — fuzzer host

The roadmap line reads *"new CMake `-fsanitize=fuzzer` target(s) reusing the
`ninja-clang-asan` toolchain"* — i.e. Windows **clang-cl**. I recommend deviating
to a **native-clang Linux lane** instead, mirroring the existing `ninja-tsan-linux`
precedent. Reviewer ratifies one of:

| | **A — Windows clang-cl** (roadmap literal) | **B — Linux native clang** (recommended) |
|---|---|---|
| Toolchain | reuse `ninja-clang-asan` (clang-cl) | new `ninja-fuzzer-linux`, mirrors `ninja-tsan-linux` |
| libFuzzer support | fragile on Windows — clang-cl `-fsanitize=fuzzer` is under-documented, needs the same manual `clang_rt.fuzzer` lib-hunt the ASan path already does (`Sanitizers.cmake:55-93`) | first-class; `-fsanitize=fuzzer,address,undefined` composes natively |
| Subset model | full Windows build env | `SMATCHET_BUILD_APP=OFF` → no GLFW/ImGui/X11/OpenGL, only clang+lld+TLS/zlib headers (proven by tsan-linux) |
| CI precedent | none for fuzzing | `tsan-linux-nightly.yml` is the working template (advisory, paths-scoped PR + nightly cron, issue-on-fail) |
| Runner cost | windows-2022 (slow, pricey) | ubuntu-latest (fast, cheap — matters for long nightly runs) |

**Recommendation: B.** libFuzzer's home is clang/Linux; the six targets are
ImGui-free Core (the same decoupled-subset constraint TSan-Linux already satisfies);
and we get a proven advisory-lane template for free. The roadmap's "reuse the asan
toolchain" intent is *honoured in spirit* — both lanes are clang+ASan+UBSan — just
on the host where the fuzzer actually works. **§5 Doc-correction** records the
roadmap-text fix.

> The rest of this plan is written for **Option B**. If the reviewer picks A, the
> CMake `fuzzer` branch and per-target target list are unchanged; only the preset
> (`ninja-fuzzer-linux` → reuse `ninja-clang-asan`) and the CI lane host
> (ubuntu-latest → windows-2022, dropping the apt step) differ.

## 4. Design (Option B)

### 4.1 `SMATCHET_SANITIZER=fuzzer` branch in `cmake/Sanitizers.cmake`

Add a fourth recognised value alongside `asan`/`tsan`/`msan`. Under non-MSVC clang:

```cmake
elseif("${SMATCHET_SANITIZER}" STREQUAL "fuzzer")
    if(_msvc)
        message(WARNING "SMATCHET_SANITIZER=fuzzer: use native clang (Linux); ignoring for '${tgt}'.")
        return()
    endif()
    list(APPEND _flags  -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    list(APPEND _link   -fsanitize=fuzzer,address,undefined)
```

`-fsanitize=fuzzer` both instruments for coverage **and** links libFuzzer's `main`,
which calls each driver's `LLVMFuzzerTestOneInput`. Flags stay PRIVATE (vendored
FetchContent deps untainted), matching the existing contract. Doc-comment the
compiler-matrix block at the top of the file (`Sanitizers.cmake:8-17`) with the new
row.

### 4.2 `ninja-fuzzer-linux` preset (CMakePresets.json)

Clone `ninja-tsan-linux` and change only the sanitizer + binary dir:

- inherits the Linux clang base (native `clang++`, lld)
- `SMATCHET_SANITIZER=fuzzer`
- `SMATCHET_BUILD_APP=OFF` (no GLFW/ImGui/Lua/whisper/Standalone/DX12)
- `SMATCHET_BUILD_TESTS=OFF` (the fuzz target list is its own thing; don't drag the full doctest rig)
- `binaryDir build/ninja-fuzzer-linux`
- a matching build preset listing the fuzz executables as `targets`

### 4.3 `tests/fuzz/` layout

```
tests/fuzz/
  CMakeLists.txt           # one add_executable per target, gated on SMATCHET_SANITIZER==fuzzer
  fuzz_image_dims.cpp
  fuzz_cpp_lex.cpp
  fuzz_markdown_adf.cpp
  fuzz_sse.cpp
  fuzz_ndjson.cpp
  fuzz_callstack.cpp
  corpus/
    image_dims/  cpp_lex/  markdown_adf/  sse/  ndjson/  callstack/
```

`tests/CMakeLists.txt` adds `add_subdirectory(fuzz)` under an
`if(SMATCHET_SANITIZER STREQUAL "fuzzer")` guard, and `tests/fuzz/CMakeLists.txt`
early-`return()`s otherwise — so a normal build never sees these targets (same
guard pattern the TSan subset uses at `tests/CMakeLists.txt:39`).

> **Corpus location note:** the roadmap said *"seed corpora in `tests/fixtures/`"*.
> I propose `tests/fuzz/corpus/<target>/` instead — corpora are fuzzer working-dirs
> (libFuzzer reads+writes them), conceptually distinct from the static golden
> fixtures in `tests/fixtures/`. Where an existing fixture is a valid seed we copy
> /derive it (see §4.5), not move it. Flagged as a deliberate roadmap deviation.

### 4.4 Driver skeleton (C++14-safe)

Every driver is the same shape — libFuzzer owns `main`:

```cpp
// fuzz_image_dims.cpp
#include "Ui/ImageDimensionsPure.h"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<unsigned char> bytes(data, data + size);
    // mimeType only steers the final error string; pin one to keep the
    // signature-dispatch path the dominant fuzzed surface.
    (void)smatchet::image_dim::ParseImageDimensionsFromBytes(bytes, "image/png");
    return 0;
}
```

Per-target wrapper specifics:
- **cpp_lex / callstack / markdown_adf** — feed the whole buffer as the
  `(const char*, len)` / `std::string` argument; discard the return.
- **sse / ndjson** — these are *stateful*. To exercise the split-buffer state
  machine (the high-value bug surface), the driver carves `data` into pseudo-random
  chunks (derived from the bytes themselves, **no RNG** — keep it deterministic for
  reproducibility) and `Feed`s them sequentially into one parser instance with a
  no-op callback, then `Flush`. A trivial single-`Feed` variant can be added if the
  chunked driver proves too clever for early coverage.

### 4.5 Seed corpora

Small, valid-ish inputs that put the fuzzer near interesting branches immediately:

| Target | Seeds |
|---|---|
| image_dims | minimal valid PNG/GIF/WEBP-VP8X/JPEG-SOF headers + a 16384-edge one (reuse the E1-crafted bytes) |
| cpp_lex | a few real C++ lines (keywords, strings, comments) + one callstack line |
| markdown_adf | headings / lists / code-fence / link snippets |
| sse | `event: x\ndata: {"a":1}\n\n` frames, incl. a split-mid-frame seed |
| ndjson | lines derived from `tests/fixtures/ollama_chat_sample.json` |
| callstack | real `p4`/debugger callstack text (a couple of frames) |

### 4.6 CI lane — `.github/workflows/fuzz-smoke.yml` (advisory)

Mirror `tsan-linux-nightly.yml` exactly in shape:

- **Triggers:** `workflow_dispatch`; nightly `cron`; `pull_request` **paths-scoped**
  to the six target `.cpp` + their headers + `tests/fuzz/**` + `CMakePresets.json` +
  `cmake/Sanitizers.cmake` + the workflow file.
- **PR run = short:** each target `-max_total_time=60` (≈6 min total for 6 targets;
  tune so the lane stays under the tsan lane's ~45 min budget) seeded from
  `tests/fuzz/corpus/<target>`.
- **Nightly run = long:** each target `-max_total_time=600` (or `-runs=N`), cron-only.
- **Advisory / NON-required:** **not** added to `project.config.json`
  `required_contexts` (same posture as tsan-linux — fuzzing is inherently
  time-bounded-nondeterministic; making it a hard PR gate re-introduces flake). A
  *crash* still fails the job red so it's visible on the PR.
- On failure: upload the `crash-*` reproducer as an artifact + (cron only) open/dedup
  a bug issue, copied from the tsan lane's issue step.

### 4.7 Crash-triage path

1. libFuzzer writes `crash-<sha1>` (the exact crashing bytes) on first failure; CI
   uploads it as a build artifact.
2. Repro locally: `build/ninja-fuzzer-linux/tests/fuzz/fuzz_<target> crash-<sha1>`.
3. Minimize: `fuzz_<target> -minimize_crash=1 -runs=10000 crash-<sha1>`.
4. Route the fix to the owning specialist (parser bug → `command-system` /
   `tracker-backend` / `p4-annotate`; image bug → `ui-host`); deep C++ root-cause →
   `debug-detective`.
5. Land the minimized crash input into `tests/fuzz/corpus/<target>/` as a permanent
   regression seed (so the fixed bug stays fixed under future runs).

## 5. Doc-correction follow-ups (land in this slice's PR, not batched)

- `testing-surface-roadmap.md` §6 E2 row + §"plan-doc" line: change "reusing the
  `ninja-clang-asan` toolchain" → "new `ninja-fuzzer-linux` lane (native clang,
  `BUILD_APP=OFF`, mirrors `ninja-tsan-linux`)" and "seed corpora in
  `tests/fixtures/`" → "`tests/fuzz/corpus/`", per §3 + §4.3 decisions.
- `testing-surface.md` §5 Gap 3: mark the fuzzing void "resolved (advisory lane)".

## 6. Proposed slicing (incremental, reviewable)

| Slice | Scope | PR |
|---|---|---|
| **E2a** | `fuzzer` branch in `Sanitizers.cmake` + `ninja-fuzzer-linux` preset + `tests/fuzz/CMakeLists.txt` + **one** driver (`fuzz_image_dims`) + its corpus. Prove the toolchain builds+runs end-to-end locally. | PR 1 |
| **E2b** | remaining five drivers + corpora (markdown_adf gated on the md4c check in §8). | PR 2 |
| **E2c** | `fuzz-smoke.yml` advisory CI lane (PR short-run + nightly long-run + issue-on-fail) + the §5 doc fixes. | PR 2 (folded with E2b) |

Two PRs: **PR 1** proves the harness on the simplest pure target before scaling;
**PR 2** scales to the other five + wires CI. (One-PR-per-feature is respected —
E2 is one feature; E2a is split off only as a de-risking toolchain proof, the
pattern the TSan-Linux subset PR itself used.)

## 7. Files to modify

| File | Change | Slice |
|---|---|---|
| `cmake/Sanitizers.cmake` | add `fuzzer` branch + matrix doc-comment | E2a |
| `CMakePresets.json` | add `ninja-fuzzer-linux` configure + build preset | E2a |
| `tests/CMakeLists.txt` | `add_subdirectory(fuzz)` under the fuzzer guard | E2a |
| `tests/fuzz/CMakeLists.txt` | **new** — per-target `add_executable`, guard-`return()` | E2a |
| `tests/fuzz/fuzz_image_dims.cpp` + `corpus/image_dims/*` | **new** | E2a |
| `tests/fuzz/fuzz_{cpp_lex,markdown_adf,sse,ndjson,callstack}.cpp` + corpora | **new** | E2b |
| `.github/workflows/fuzz-smoke.yml` | **new** — advisory lane | E2c |
| `docs/plans/active/testing-surface-roadmap.md`, `docs/guides/testing-surface.md` | §5 doc fixes | E2c |

## 8. Perf-gate (mandatory per roadmap line 174)

**N/A — no production hot-path change.** The fuzz drivers are *additive test
executables*: they `#include` Core headers and link the six Core `.cpp` files
**unmodified**, behind a `SMATCHET_SANITIZER==fuzzer` configure guard that the
shipped Standalone/Unreal builds never set. The shipped Smatchet binary's runtime
code and frame loop are untouched; no perf scenario is affected; no `Source/Core/`
production `.cpp` is edited.

**Exception that would flip this:** if making `MarkdownToAdf` fuzzable requires
un-gating md4c from `SMATCHET_BUILD_APP` (a real CMake/Core touch — see the §10
risk), that edit gets a perf note at implementation time. The other five targets
have zero Core-build coupling.

## 9. Verification

- **E2a:** `cmake --preset ninja-fuzzer-linux && cmake --build --preset ninja-fuzzer-linux`
  builds `fuzz_image_dims`; `fuzz_image_dims -runs=100000 corpus/image_dims` runs
  clean (no crash); a deliberately-corrupt seed is found+rejected (sanity that the
  harness *can* surface a fault — temporary, not committed).
- **E2b:** all six targets build + each survives a `-runs=200000` smoke clean.
- **E2c:** `fuzz-smoke.yml` goes green on a no-op PR (paths-filter no-op) and runs
  the short loop on a PR that touches a target `.cpp`; nightly dispatch
  (`gh workflow run fuzz-smoke.yml`) completes; an intentionally-planted crash
  (local only) produces a `crash-*` artifact + would open the dedup issue.
- Gates: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop`
  (drivers are first-party C++14 → subject to `no-printf-stderr` etc.; keep them
  `LOG_*`-free / minimal). Sanitizer + Coverage required contexts are unaffected
  (new files, not in their scope).

## 10. Risks & open questions (for reviewer)

1. **md4c reachability under `BUILD_APP=OFF`** — `MarkdownConvert.cpp` needs md4c.
   If md4c's FetchContent is gated behind `SMATCHET_BUILD_APP`, the markdown target
   either pulls md4c in for the fuzz config or `MarkdownToAdf` is **deferred to a
   follow-up** and E2b ships five targets. *Verified at E2b implementation time;
   does not block E2a.*
2. **nlohmann under the subset** — targets 3+5 need nlohmann_json; the TSan-Linux
   subset already links it (`tests/CMakeLists.txt:84`), so this is low-risk.
3. **Stateful-parser driver cleverness** — the chunked `Feed` driver (§4.4) may
   under-cover early on. Mitigation: ship the trivial single-`Feed` variant first,
   add chunking once base coverage is established.
4. **Advisory vs required** — confirm the reviewer agrees fuzzing stays **advisory**
   (my recommendation, matching tsan-linux). A red *crash* is still visible on the
   PR; we just don't block merge on a time-bounded nondeterministic lane.
5. **PR time budget** — `-max_total_time=60`×6 ≈ 6 min CPU; confirm acceptable on
   the paths-scoped PR trigger (only fires when a target file changes).

## Implementation log

- 2026-06-16 — **Slice complete / archived.** The 5 remaining drivers (cpp_lex, callstack, markdown_adf, ai_sse, ai_ndjson) shipped under the child plan `slice-e2b-libfuzzer.md` (PR 2a #1301 + PR 2b #1307). All 6 `fuzz_*` targets now on develop; this parent plan is archived to `shipped/`.
- 2026-06-15 — Plan drafted from infra recon (Sanitizers.cmake, tsan-linux-nightly.yml
  template, the six target headers + their doctest link closures, CMakePresets). Awaiting
  user review of §3 host decision (recommend B) + §10 open questions.
- 2026-06-15 — **E2a implemented** (autonomous, §3 Option B + §10 recommendations adopted).
  `cmake/Sanitizers.cmake` fuzzer branch (FATAL on MSVC/clang-cl + non-Clang; flags
  `-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`)
  + matrix doc row + recognised-values list. `ninja-fuzzer-linux` configure+build presets
  (clone of `ninja-tsan-linux`). `tests/CMakeLists.txt` fuzzer guard (`add_subdirectory(fuzz)`
  → `return()`, mirrors tsan). New `tests/fuzz/` — reusable `smatchet_add_fuzz_target()`
  helper + `fuzz_image_dims` driver (links `ImageDimensionsPure.cpp` only) + PNG/GIF/JPEG
  seed corpus + README. `.github/workflows/fuzz-smoke.yml` advisory lane (auto-discovers
  `fuzz_*` binaries). Local validation: `cmake --list-presets` registers both presets,
  YAML+JSON parse clean, lint-rules gate on the diff. **Linux native-clang build/run =
  CI-only** (Windows host cannot build the lane) — `fuzz-smoke.yml` is the sole build
  validator, so it ships in PR 1 (deviation 2 below).

## Deviations

1. **Preset `SMATCHET_BUILD_TESTS=ON`, not `OFF`** — §4.2 specified `BUILD_TESTS=OFF`. That
   is wrong: the root `CMakeLists.txt` only runs `add_subdirectory(tests)` under
   `if(SMATCHET_BUILD_TESTS)`, so OFF would mean `tests/fuzz/` is never configured and no
   driver builds. Set to `ON` (matching `ninja-tsan-linux`); the fuzzer guard in
   `tests/CMakeLists.txt` early-`return()`s past the ImGui-linked doctest rig, so ON costs
   nothing but the `add_subdirectory(fuzz)`.
2. **CI lane (`fuzz-smoke.yml`) pulled forward into PR 1** — §6 slated it for E2c/PR 2. The
   `ninja-fuzzer-linux` lane is **native-clang on Linux**; this is a Windows host, so the
   harness cannot build or run the fuzzers locally → **CI (ubuntu) is the only build
   validator**. The workflow must ship with E2a to prove the toolchain end-to-end. The lane
   auto-discovers `fuzz_*` binaries (find-loop + `tests/fuzz/**` path trigger), so PR 2 adds
   drivers with **no workflow edit** — the original "CI in E2c" intent (zero churn when
   drivers land) is preserved, just front-loaded.
3. **Build preset omits an explicit `targets` list** (builds the default `all`) instead of
   naming a single target like the tsan build preset names `SmatchetTsanTests`. Reason: same
   auto-scale goal — PR 2's five drivers build with no preset edit.
4. **Verification method: `-runs=0` smoke (ctest) + `-max_total_time` CI run**, not §9's
   `-runs=100000` local loop. The local loop is impossible on a Windows host; the ctest
   `-runs=0` smoke proves each binary loads its corpus + libFuzzer init, and `fuzz-smoke.yml`
   does the real time-boxed bug-finding run (45 s/target PR, 300 s/target nightly).
5. **PR fuzz budget 45 s/target, not §10-Q5's 60 s** — trims worst-case PR CPU as the driver
   count grows (45 s × 6 ≈ 4.5 min). Nightly stays generous at 300 s/target.
