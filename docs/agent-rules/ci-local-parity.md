# CI ↔ local-gate parity

> **Goal**: every CI failure that is *locally knowable* must be catchable **before a PR opens**, so a push does not red on something a fast local check would have caught. This doc is the map of which CI checks have a local mirror, which are enforced **at push time** (the `pre-push` hook), and which are irreducibly **CI-only** (Windows / Mesa / Android toolchain) and therefore accepted residue on a non-Windows dev box.

## Why this exists — the churn finding

A review of recent merged PRs (backlog: `postmortems.md` + `categories/{process,tooling,infra,test}.md`; merge ledger: `docs/self-improvement/merge-snapshots.jsonl`) shows the multi-iteration re-push churn is dominated by **locally-knowable** failure classes, not toolchain-only ones:

| Churn class | Required check it reds | Locally runnable? | Root |
|---|---|---|---|
| Comment-noise / high-integrity lint (bare `//` runs, decorative banners, function-too-long, strict-zone, dup, include-cycle, fan-in) | `Comment-noise + high-integrity gate` | **YES** — `agents/scripts/project/test-lint-rules.sh --diff` | content |
| clang-format reflow of a moved comment | `Windows + MSVC` (lint step) | **YES** — `clang-format --dry-run --Werror` | content |
| Doc-anchor / plan-ref / plan-index / markdown-link break (esp. after a `git mv` / plan-archive) | `Doc anchors + agent contract` | **YES** — `scripts/dev/test-docs.sh` | content |
| shellcheck SC2086 / SC2046 / SC2155 | `Shell lint (shellcheck)` | **YES** — `agents/scripts/core/test-lint-bash.sh` (needs shellcheck) | content |

The root cause of the *recurrence* (filed `build-verify-shortcut-bypasses-comment-noise-and-lint-gate`, tooling 2026-06-18): the fast **build-only verify path** (`cmake --build`, `build-and-run.sh --build-only`) does **not** run `test-lint-rules.sh` / `test-docs.sh`, so an author who verifies that way learns of a locally-knowable failure only on the first CI run — one wasted round-trip (and a fresh CodeRabbit auto-review) per occurrence.

**The fix**: `scripts/git-hooks/pre-push` step **(D)** now runs these locally-runnable required-check mirrors on every feature-branch push — delta + path scoped (fast for docs/config/shell; bounded by a 90 s fail-open timeout when first-party C++ changed), fail-CLOSED on a real violation, fail-OPEN on infra/timeout. A locally-knowable failure can no longer reach GitHub. Authors who already run `bash scripts/dev/pre-ship.sh` (which also auto-fixes) pay nothing — the hook just confirms.

## Parity matrix — every PR check

**Required (GitHub branch-protection blocks merge):**

| CI check | Local mirror | Enforced at push? | Notes |
|---|---|---|---|
| `Comment-noise + high-integrity gate` | `test-lint-rules.sh --diff` | ✅ pre-push (D) #1 | delta-scoped |
| `Doc anchors + agent contract` | `test-doc-anchors.sh` + `test-markdown-links.sh` + `test-portable-purity.sh` | ✅ pre-push (D) #2 | the develop-clean, high-churn subset — see note below |
| `Shell lint (shellcheck)` | `test-lint-bash.sh` | ✅ pre-push (D) #4 | needs `shellcheck` on PATH; else fail-open |
| `Windows + MSVC` (clang-format part) | `clang-format --dry-run` | ✅ pre-push (D) #3 | only the format check is local |
| `Windows + MSVC` (build + ctest) | — | ❌ CI-only | needs Windows MSVC toolset 14.38 |
| `Windows + MSVC (light — AI/Whisper/MCP off)` | — | ❌ CI-only | needs Windows MSVC |
| `Test-delta gate` | `coverage-delta-gate.sh` | ⚠️ partial | needs a prior coverage run to diff |
| `Perf PR-fast (windows-2022)` | `scripts/dev/perf-run.sh` | ❌ CI-only | needs Windows + Mesa GL + machine baselines |
| `Coverage (windows-2022 + OpenCppCoverage)` | `coverage.sh` | ❌ CI-only | needs Windows + OpenCppCoverage |
| `Sanitizer (ASAN via MSVC)` | — | ❌ CI-only | needs Windows MSVC ASAN (Source/Core PRs only) |
| `Sanitizer (UBSan via Clang)` | — | ❌ CI-only | needs Windows clang-cl (Source/Core PRs only) |

> **Why (D) #2 runs a subset, not the full `test-docs.sh`:** the full suite also runs `test-plan-index` (the `docs/plans/INDEX.md` sync check), which is **advisory** — a PR-branch helper job auto-regenerates the INDEX, so CI never blocks on it and `origin/develop` itself can carry INDEX drift. Running it in the push gate would false-block an *unrelated* push on pre-existing drift and train override-by-reflex. (D) #2 therefore runs only the **develop-clean, blocking** doc checks (`test-doc-anchors`, `test-markdown-links`, `test-portable-purity`). For the complete doc gate incl. INDEX, run `bash scripts/dev/test-docs.sh` or `bash scripts/dev/pre-ship.sh`.

**Locally-runnable advisory checks** (not yet in the push gate — low churn, but cheap to add later): `Duplication scanner` (already folded into `test-lint-rules`), `Pillar 2 scanner` (`pillar2-scan.sh`), `Android security gate` (`test-mobile-security.sh --check`), `Mobile — POSIX core compile (Linux clang)`, `Fuzz smoke` build/ctest (Linux clang), `Lua mirror smoke`.

## CI-only residue (accepted)

These cannot run on a non-Windows / non-Mesa dev box and are **accepted residue** — CI is the backstop, and develop runs post-merge CI as the safety net (strict-off merge model):

- **Windows MSVC build + ctest** (×2: full + light) — toolchain-pinned (MSVC 14.38).
- **Perf PR-fast / Coverage / Sanitizer ASAN / Sanitizer UBSan** — Windows-only instrumentation.
- **Bucket-C/E screenshot + UI tests, mobile texture-guard, launch-smoke** — need Mesa headless GL on Windows.
- **Mobile Android NDK / APK / emulator** — need the Android toolchain.
- **CodeQL** — needs the CodeQL CLI (~90 min build).
- **Windows-on-ARM / ARM64 cross lanes** — `advisory, runner-gated`; **report `failure` (not `neutral`) when their runner is absent**, so they false-red every PR. They never block merge (not required, not on the meant-to-block allow-list), but the webhook noise is a known follow-up (make the runner-gated lanes skip cleanly — `if: vars.ENABLE_WIN_ARM64_RUNNER` already guards execution; the conclusion should be `neutral`).

## Running it locally

- **Full, auto-fixing** (recommended before any push): `bash scripts/dev/pre-ship.sh` — clang-format `-i` + comment-noise auto-strip, then the same gates the push hook runs.
- **Automatic** (no action needed): the `pre-push` hook step (D) runs the non-mutating mirrors on every feature-branch push. Wire the hook via `bash agents/scripts/core/setup-harness.sh <harness>` (`git config core.hooksPath scripts/git-hooks`).
- **Override** (sanctioned, logged): `SMATCHET_SKIP_PRESHIP_GATE=1 git push …` — use only when the gate itself misfires; never to ship a known-red change.
