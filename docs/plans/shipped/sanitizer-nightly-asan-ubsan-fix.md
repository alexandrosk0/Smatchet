# Plan — Sanitizer nightly (ASan+UBSan) fix + config-skew preventing gate
<!-- plan-date: 2026-06-13 -->

> **Slug**: `sanitizer-nightly-asan-ubsan-fix` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.
>
> **Mandatory rules cross-link**: `AGENTS.md` § Project rules (Plan-doc family), § Merge gates (gate-escape postmortem), `docs/agent-rules/build.md` § ASan over the test rig.

## Context

GitHub **Issue #863** — "Sanitizer nightly failed (ASan+UBSan)". The nightly Clang ASan+UBSan job on `develop` failed three nights running (CI runs `26997373785`, `27053572861`, `27083859902`; 2026-06-05 → 06-07). The Issue body points at "the AddressSanitizer / UBSan report", but the CI logs tell a different, more precise story.

**The failure is a COMPILE error, not a runtime sanitizer finding.** The sanitized binary never linked — the build stopped at:

```
[227/449] Building CXX object …/Source/Core/src/AppController.cpp.obj
FAILED: CMakeFiles/SmatchetStandalone.dir/Source/Core/src/AppController.cpp.obj
D:\a\Smatchet\Smatchet\Source\Core\src\AppController.cpp(293,6):
    error: unused function 'LogLuaScriptFileProbe' [-Werror,-Wunused-function]
ninja: build stopped: subcommand failed.
```

Root cause: `LogLuaScriptFileProbe(const char*, const std::string&)` is a Lua-diagnostics helper whose **two call sites** (init path) were already wrapped in `#if defined(SMATCHET_WITH_LUA_AUTOMATION)`, but the **function definition itself was not**. The nightly sanitizer rig builds the **Lua-OFF** config (its compile line carries `-DSMATCHET_WITH_MCP=1 -DSMATCHET_WITH_AI=1` but **no** `-DSMATCHET_WITH_LUA_AUTOMATION`). With Lua off, the definition has zero callers, and Clang's `/WX -Werror` promotes `-Wunused-function` to a hard error. MSVC at `/W4` (the default iter/publish presets) does **not** warn on an unused free function with internal linkage the same way, so every PR-time CI job stayed green while the nightly broke — a **config-skew gate escape**.

This is a Pillar-3 safety-net failure: the sanitizer gate exists precisely to keep `develop` sanitizer-clean, and a regression slipped past PR-time CI because no PR-gated job compiles the Lua-off sanitizer config.

**The product fix has ALREADY landed.** Commit `61b17427` (`refactor(multi-grid): Slice 1a …`, #945, 2026-06-07 09:27) wrapped the *definition* in the same `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` guard as its call sites (current `Source/Core/src/AppController.cpp:302`/`:322`), with a self-documenting comment naming the exact reason. The nightly went **green on 2026-06-08** (run `27118115403`) and has stayed green every night through 2026-06-13 (`27457877260`).

**Intended outcome — the one-sentence "after this lands, X is true":** the Lua-off `-Werror` config-skew class that produced #863 can no longer reach `develop` undetected, because a PR-time gate (or a documented decision to accept nightly-only coverage) now catches an unused symbol in a `SMATCHET_WITH_*`-off build before merge. The product code is already correct; **the deliverable of this plan is the preventing gate + Issue closeout**, not a re-fix.

## Approach

Two-part, in priority order.

**Part 1 — verify + close (cheap, no product code).** Re-confirm the guard is symmetric (definition + both call sites under one `#if defined(SMATCHET_WITH_LUA_AUTOMATION)`), confirm 6 consecutive green nightlies, and close Issue #863 as already-fixed-by-`61b17427` with the CI-evidence trail. No source edit — `develop` is already correct. (If a local Clang ASan Lua-off build is wanted as belt-and-suspenders, build the existing provisioned sanitizer dir per § Verification; do **not** cold-configure a shared dir.)

**Part 2 — the preventing gate (the real work).** The escape was *config skew*: PR-time CI builds only Lua-ON / MSVC configs, so a Lua-off `-Werror` break is invisible until the nightly. Close that with the cheapest gate that catches the class. Preferred option (least infra, fastest signal): a **PR-time lint rule** `unused-symbol-under-config-guard` that flags a function/variable *definition* gated differently from (or less than) all its call sites — i.e. call sites under `SMATCHET_WITH_X` but the definition unguarded. This is a static text/AST-lite check, runs in the existing `test-lint-rules.sh` harness, and needs no extra CI minutes. Trade-off named: a full per-`SMATCHET_WITH_*`-permutation compile matrix would be airtight but multiplies CI cost across 3+ feature flags — rejected for now in favour of the targeted lint + the existing nightly as backstop. The gate decision is recorded as a `gate-escape-postmortem` entry (mandatory `### Preventing gate`).

## Files to modify

The **product fix is already merged** (`61b17427`) — no `Source/` edit in this plan. Files below are for Part 2 (the gate) + closeout.

1. `agents/scripts/project/lint-rules/` (new rule script, name per existing convention) — implement `unused-symbol-under-config-guard`: detect a definition whose every reference sits under a `SMATCHET_WITH_*` guard while the definition does not. Scope to first-party C++; delta-gated vs `origin/develop`.
2. `AGENTS.md` § Enforcement contract-card (the rule-table) — add the `unused-symbol-under-config-guard` row (scope: all first-party C++; note: config-skew `-Werror,-Wunused-function` catch). The card is the single source of truth each gate's `--selftest` asserts against.
3. `agents/scripts/project/test-lint-rules.sh` — register the new rule + a `--selftest` fixture (one guarded-callsite/unguarded-def positive, one symmetric-guard negative).
4. `docs/self-improvement/postmortems.md` — append the #863 gate-escape entry with the mandatory `### Preventing gate` naming rule (1).
5. `docs/self-improvement/categories/infra.md` (or `tooling.md`) — the gate as a normal self-improvement entry, cross-linked from the postmortem.
6. **Closeout (not a file edit):** `gh issue close 863` with the root-cause + `61b17427` + green-nightly evidence comment.

Before adding the rule TU: `rg -l 'unused-function|config-guard|SMATCHET_WITH_' agents/scripts/project/` to confirm no existing rule already covers the class (avoid a parallel duplicate).

## Existing utilities reused

- The committed guard pattern itself — `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` around both definition (`Source/Core/src/AppController.cpp:302`) and call sites (`:1960`) — is the **correct reference shape** the new lint rule encodes as the "good" case.
- `agents/scripts/project/test-lint-rules.sh` — existing delta-gated lint harness (`--diff origin/develop`, `SMATCHET_DEVIATION(...)` escape grammar, per-rule `--selftest`); the new rule plugs into it rather than standing alone.
- The nightly `Sanitizer nightly` workflow (already authoring auto-Issues like #863) stays as the **backstop** behind the new PR-time gate — no change to it.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — the change is build-tooling + a lint rule; zero runtime code path touched.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no UI or threading code involved.
- **Pillar 3 (never crash)**: **this is the point.** The sanitizer nightly is the Pillar-3 safety-net gate (sanitizer build clean = no UB / UAF / OOB reaches ship). #863 was that gate doing its job (it red-flagged the regression) but *too late* (post-merge). The preventing gate moves detection of this class to PR-time, tightening Pillar-3 enforcement so a future config-skew break can't sit red on `develop` for three nights.
- **Pillar 4 (accessibility)**: N/A — no user-facing surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

**N/A — this plan touches NO `Source/Core/` runtime code.** The product fix already merged in `61b17427`; this plan's remaining work is a lint-rule script (`agents/scripts/`), `AGENTS.md`, and self-improvement docs — none on a perf-gated runtime path. The already-merged guard change is a pure preprocessor `#if` around a diagnostics-only logging helper that runs once at init under Lua-ON builds; it has no steady-state frame cost and no `SMATCHET_UI_PERF_SCOPE` markers. No PR-fast scenario, dispatcher drain, or visible-cue harness applies.

## Risks / non-goals

- **Risk: the new lint rule over-fires (false positives) on legitimately-asymmetric guards** (e.g. a definition used by *both* a guarded and an unguarded call site). Mitigation: the rule flags only definitions where **all** references are guarded but the def is not; the `SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; …)` escape covers any intentional case. Ship WARN-first (calibration) per the DRY/`duplication` precedent if positives are noisy.
- **Risk: lint rule can't see `-Werror` semantics MSVC-vs-Clang differ on** — the rule is a *proxy* for the Clang warning, not the compiler. Mitigation: keep the nightly Lua-off sanitizer build as the authoritative backstop; the lint is the fast-fail, not the sole gate.
- **Accepted: no full `SMATCHET_WITH_*` permutation compile matrix.** Rejected as too costly for prerelease; the targeted lint + nightly backstop covers the observed class. Revisit if a *second* config-skew escape lands (then the matrix earns its CI minutes).
- **Non-goal: re-fixing the product code.** It's already correct on `develop` (`61b17427`). This plan must not touch `AppController.cpp`'s guard.
- **Non-goal: redesigning the diagnostics probe.** `LogLuaScriptFileProbe` is a deliberate, kept Lua-init diagnostic (LOG_INFO/LOG_WARN), not stray `[temp-debug]` residue — leave it.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no pure-logic C++ helper added.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario / screenshot / sanitizer**:
  1. **Authoritative sanitizer-green proof (already true):** `gh run list --workflow="Sanitizer nightly" -L 8` shows `success` for 2026-06-08 → 06-13 (6 consecutive). The bucket is green on `develop` today.
  2. **Local Lua-off ASan compile (optional belt-and-suspenders):** build the existing provisioned Clang-ASan dir with `cmake --build` (NOT a shared-dir `--preset` reconfigure). If none provisioned, the cold recipe is `cmake --preset ninja-msvc-asan -DSMATCHET_BUILD_TESTS=ON` (build.md § ASan over the test rig) configured through `scripts/dev/with-msvc.ps1` with `-DFETCHCONTENT_BASE_DIR=C:/Development/Smatchet/.fetchcontent-src`; confirm `AppController.cpp.obj` compiles clean in the Lua-off config.
  3. **New lint-rule `--selftest`:** `bash agents/scripts/project/test-lint-rules.sh --selftest` — the `unused-symbol-under-config-guard` fixtures pass (positive flags the unguarded-def-with-guarded-callsites shape; negative passes the symmetric guard).
  4. **Regression replay:** point the new rule at commit `61b17427~1`'s `AppController.cpp` (the pre-fix state) — it MUST flag line 293. Point it at HEAD — it MUST be clean. This proves the gate would have caught #863.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — confirms the lint-rule wiring doesn't disturb the build (no `Source/` change expected, so this is a no-op sanity build).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint). The `AGENTS.md` contract-card edit must keep the card's `--selftest` token assertions consistent.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the build/CI domain model + sharpen "config-skew gate escape" terminology before finalising; record the outcome.
- **Manual residue**: none anticipated. If the lint rule proves infeasible as static analysis, the deferred-automation fallback is a Lua-off compile job added to PR-fast CI — file a `docs/self-improvement/categories/infra.md` entry, no silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray "sanitizer nightly broken / #863 open" references and revise once the Issue closes.

- **Full `SMATCHET_WITH_*` permutation CI matrix** — named in § Risks as rejected-for-now; follow-up only if a second config-skew escape lands.
- **Auditing other `SMATCHET_WITH_LUA_AUTOMATION` / `SMATCHET_WITH_MCP` / `SMATCHET_WITH_AI` guards for the same asymmetry** — the new lint rule will surface any others on first run; no manual sweep needed here, but a clean first-run is worth noting in the impl-log.
- **Changing the nightly to fail-faster or post richer reports** — the nightly already auto-Issues correctly (#863 is proof); no change.

## Implementation log

- `ac4b94be` · #1184 — `unused-symbol-under-config-guard` lint rule + #863 gate-escape postmortem: AGENTS.md contract-card row, rule wired into `agents/scripts/project/test-lint-rules.sh` (with `--selftest` fixtures in `tests/bats/lint_rules.bats`), `postmortems.md` #863 entry with mandatory `### Preventing gate`, and applied `infra.md` self-improvement entry.
- `61b17427` · #945 — product premise fix (`#if defined(SMATCHET_WITH_LUA_AUTOMATION)` guard on the `LogLuaScriptFileProbe` definition); pre-merged before this plan (non-goal — no re-fix here). Issue #863 CLOSED 2026-06-13.

## Deviations from plan

- **Down-scoped the new `unused-symbol-under-config-guard` rule from the preferred absolute-0 enforcement to WARN-first / advisory calibration** — consistent with the AGENTS.md WARN-first precedent (e.g. `duplication`); the rule is a per-file text proxy for the Clang `-Werror,-Wunused-function` semantics, not the compiler, so it ships advisory with the nightly Lua-OFF sanitizer build as the authoritative backstop. Rationale recorded in commit `ac4b94be` + `infra.md`.

## Verification (actual)

- AGENTS.md Enforcement contract-card carries the `unused-symbol-under-config-guard` (WARN) row — verified present in tree (archival audit 2026-06-16).
- Lint rule registered in `agents/scripts/project/test-lint-rules.sh` with `--selftest` fixtures in `tests/bats/lint_rules.bats` — verified present in tree (archival audit 2026-06-16), not re-run.
- `docs/self-improvement/postmortems.md` #863 entry present with the mandatory `### Preventing gate` section — verified present in tree (archival audit 2026-06-16).
- `docs/self-improvement/categories/infra.md` self-improvement entry present (applied) — verified present in tree (archival audit 2026-06-16).
- Issue #863 CLOSED 2026-06-13; product premise fix `61b17427`/#945 pre-merged — verified present in tree (archival audit 2026-06-16), not re-run.
