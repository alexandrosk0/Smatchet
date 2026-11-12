# Plan — UBSan + instrumented rig on the Windows clang-cl sanitizer lanes

> **Slug**: `clang-cl-ubsan-lanes` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Backlog entry `docs/self-improvement/categories/infra/2026-10-03-clang-cl-sanitizer-lanes-apply-asan-only.md` (P1): the Windows "Clang ASan+UBSan" lanes apply ASan only, because `smatchet_apply_sanitizers()` sends clang-cl down its MSVC branch, which emits `/fsanitize=address` alone. The REQUIRED per-PR check "Sanitizer (UBSan via Clang)" therefore runs no UBSan, and it runs the doctest rig uninstrumented (no `SMATCHET_SANITIZE_TESTS`). An instrumented clang-cl rig also crashed in C++ exception dispatch (rethrow) with today's flags. After this lands, the per-PR Clang check and the nightly Clang job build Core and the doctest rig with ASan **and** UBSan and run the instrumented rig.

## Approach

Fix the flags rather than the job names. In the clang-cl arm of `smatchet_apply_sanitizers()` add `-fsanitize=undefined -fno-sanitize-recover=all` (UBSan handlers live in `clang_rt.asan_dynamic`, so no extra runtime) and `/clang:-fsanitize-address-use-after-return=never` (the rethrow-crash fix the backlog probe found). Add a compiler-independent `SMATCHET_ASAN` compile definition to ASan targets, and key the tests' widened wall-clock budgets on it, because clang-cl defines `__SANITIZE_ADDRESS__` only from LLVM 22 and the runner has LLVM 20. Then turn on `SMATCHET_SANITIZE_TESTS` in the per-PR and nightly clang configure lines, and make their `ctest` refuse an empty test list.

## Interface contracts

N/A — build flags, CI configure lines and test-only preprocessor guards; no product interface changes.

## Files to modify

1. `cmake/Sanitizers.cmake` — clang-cl UBSan + use-after-return flags; `SMATCHET_ASAN` definition; header matrix comment.
2. `CMakePresets.json` — `ninja-clang-asan` / `ninja-ui-test-asan-clang` descriptions stop saying "ASAN only".
3. `.github/workflows/build-and-test.yml` — `sanitizer-ubsan-pr`: `-DSMATCHET_SANITIZE_TESTS=ON`, `ctest --no-tests=error`.
4. `.github/workflows/sanitizer-nightly.yml` — clang job header comment (no longer ASan-only).
5. `tests/Core/{CancelToken,CallstackParser,BulkImportAbandonNonBlocking,UserInfoActivityCancelUaf,LocalCacheManagerChat,StubAiClientCancel,SubprocessCapture}.test.cpp` — the 9 `__SANITIZE_ADDRESS__` budget guards also accept `SMATCHET_ASAN`.
6. `docs/self-improvement/categories/infra/2026-10-03-clang-cl-sanitizer-lanes-apply-asan-only.md` — status.

## Existing utilities reused

`smatchet_apply_sanitizers()`, the `SMATCHET_SANITIZE_TESTS` knob, the `ubsan-out-of-band` label valve.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — no extraction.

## UX Pillar callouts

Pillar 3 (never crash): UBSan coverage on Windows for the code the rig exercises. No user-facing change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — no `Source/Core/` change; flags apply to sanitizer builds only.

## Risks / non-goals

- UBSan may surface real findings; each is fixed or triaged before the lane blocks on it (the `ubsan-out-of-band` label is the in-workflow valve).
- The runner's LLVM 20.1.8 may behave differently from local LLVM 22.1.6; CI on this PR is the proof.
- Non-goal: the UI-test (bucket-E) clang preset's own lane; it inherits the flags but is not re-wired here.

## Implementation handoff

Single slice, orchestrator-implemented.

## Verification

- Local: `ninja-clang-asan` + `-DSMATCHET_BUILD_TESTS=ON -DSMATCHET_SANITIZE_TESTS=ON`, build `SmatchetTests`, run it with the ASan runtime on PATH; no `runtime error:` / AddressSanitizer report.
- CI: "Sanitizer (UBSan via Clang)" green on this PR with the instrumented rig.
- Doc-validation suite; grill-with-docs stress-test; deferral residue-sweep — N/A for the latter two beyond this file (single-slice build fix).

## Open questions

None.

## Out of scope (flagged, not designed)

- `ctest --no-tests=error` across every other workflow (backlog item (d)) beyond the job touched here.

## Implementation log

- Clang-cl arm of `smatchet_apply_sanitizers()`: `-fsanitize=undefined -fno-sanitize-recover=all /clang:-fsanitize-address-use-after-return=never`; `SMATCHET_ASAN=1` on every ASan/fuzzer target; POST_BUILD copy of LLVM's `clang_rt.asan_dynamic-<arch>.dll` next to each sanitized executable.
- 9 test budget guards + `tests/support/StubAiClient.h` cancel-ack budget accept `SMATCHET_ASAN`.
- Per-PR `sanitizer-ubsan-pr`: `-DSMATCHET_SANITIZE_TESTS=ON`, `ctest --no-tests=error`; comments, preset descriptions and the nightly header updated.

## Deviations from plan

- Added the runtime-DLL copy (code-review CRITICAL): MSVC's `VC\Tools\...\bin` ships a same-named `clang_rt.asan_dynamic-x86_64.dll` without the UBSan exports and sits first on PATH under msvc-dev-cmd, so the instrumented rig would not have loaded on CI.
- `tests/support/StubAiClient.h` added to the write-set (review LOW: the 100 ms cancel-ack budget was widened for coverage only).

## Verification (actual)

- Local LLVM 22.1.6 / MSVC 14.38: `ninja-clang-asan` + `-DSMATCHET_BUILD_TESTS=ON -DSMATCHET_SANITIZE_TESTS=ON` builds `SmatchetStandalone` and `SmatchetTests`; every rig TU carries `-fsanitize=undefined`; the exe imports `clang_rt.asan_dynamic-x86_64.dll` and links `__ubsan_handle_*`.
- Rig run under `with-msvc-env.sh` with MSVC's DLL first on PATH: 3927/3927 test cases, 52766 assertions, no sanitizer report.
- CI on the runner's LLVM 20.1.8: this PR.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
