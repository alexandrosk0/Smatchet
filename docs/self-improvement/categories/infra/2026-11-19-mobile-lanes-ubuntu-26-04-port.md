# `ubuntu-latest` flipped to 26.04 and reddened every Mobile lane for ~a day; lanes pinned to `ubuntu-24.04` until ported

- **Category**: infra
- **Priority**: P2
- **Date**: 2026-11-19
- **Observed on**: PR runs from 2026-11-18T17:18Z onward (first red 44357118897; last green 44321778430, 2026-11-18T14:12Z) — `Mobile — POSIX core compile gate (Linux clang)`, `Mobile — Android NDK arm64-v8a (.so configure+link)`, `Mobile — Android emulator smoke (x86_64, boot APK + first frame)` red on every PR; develop pushes skip these lanes, so develop's own runs stayed green and showed nothing.
- **Status**: open — lanes pinned to `ubuntu-24.04` (stopgap); the port is open

## What happened

The job logs' `Image:` line moved from `ubuntu-24.04` (image 20261111.127.1; CMake 3.31, host clang 18, perl 5.38) to `ubuntu-26.04` (image 20261111.196.1; CMake 4.4, host clang 21, perl 5.40; the NDK lanes use NDK clang 17 on both) between those two runs, with no repo commit involved. Two independent breaks:

1. **POSIX lane:** bundled cpr 1.9.2 `include/cpr/callback.h:15` fails with `unknown type name 'intptr_t'` — the header never includes `<cstdint>` and the 26.04 libstdc++ no longer pulls it in transitively. Fix on the port: add `<cstdint>` through `smatchet_patch_cpr_after_fetch` (`cmake/SmatchetThirdParty.cmake`), which already patches `body.h` the same way (`smatchet_patch_or_die`).
2. **NDK / APK / emulator lanes:** the link of `libSmatchetMobile.so` fails with `ld.lld: error: unable to find library -l:libjitterentropy.a` and `-lzstd`. Those are host (26.04 OpenSSL) link flags; with CMake 4.4 the configure log also shows `Looking for dlopen in dl;-pthread;OpenSSL::SSL;OpenSSL::Crypto - not found` (found on 3.31.6) and a newly found `libz`. Working hypothesis, unverified: CMake 4's FindOpenSSL pulls static link deps from the host `openssl.pc` via pkg-config. Candidate fix to try on the port: `-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON` (or `PKG_CONFIG_LIBDIR=${OPENSSL_PREFIX}/lib/pkgconfig`) in the Android configure step.

## Concrete next action

1. Reproduce both on a `ubuntu-26.04` runner (a branch with `runs-on: ubuntu-26.04` on the pinned jobs), apply the two fixes above, then drop the pins in `build-and-test.yml` (`mobile-posix-core-check`, `mobile-android-ndk`, `mobile-android-apk`), `mobile-emulator-smoke.yml`, and the two sibling lanes that build the same `posix-core-check` preset and were pinned preemptively: `monkey-nightly.yml` and `linear-live-smoke.yml`. When doing so, add `${{ env.ImageOS }}` to the OpenSSL/FetchContent cache keys (they carry no image component today, so a 24.04-built cache would restore on 26.04 and could mask the pkg-config question).
2. Decide whether Mobile lanes should stay pinned to an explicit LTS image permanently (24.04 is supported to 2029); the Windows x64 lanes already pin `windows-2022` for the same reason.
3. The three `build-and-test.yml` Mobile lanes skip on develop pushes, so a runner-image regression is invisible until a PR touches them; consider a scheduled develop run of these lanes.

Same class (hosted image moved with no introducing commit): [`2026-10-05-runner-image-toolset-change-reds-a-push-only-lane.md`](2026-10-05-runner-image-toolset-change-reds-a-push-only-lane.md).
