# Plan — build-quality + velocity hardening

> **Slug**: `build-quality-velocity-hardening` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Triggered by a "would vlang help?" question. A 13-dimension, 27-agent evidence-backed audit of the C++14 dual-target build, plus a read-only round-2 that resolved three Sprint-2-gating unknowns.

**Verdict:** a language/std swap is rejected — `Source/Core/` compiles into both `SmatchetStandalone` (OpenGL/GLFW) and `SmatchetCore_DX12` (Unreal); Unreal pins C++14 and forbids a non-C++ module, so the swap is impossible *and* the audit shows it unnecessary. **31 of 40 findings are fixable in place; only 9 are genuine intrinsic C++14/dual-target tax, most still mitigable in-language** (pImpl, fwd-decl, per-target PCH, `static_assert`).

After this lands, the highest-ROI build-quality + velocity fixes are sequenced, and the "build/CI reports GREEN while real breakage escapes" correctness classes are closed. Several findings are direct **Pillar-3** violations / real bugs: a sol2 patch that fails open into heap corruption on a dep bump, an OfflineQueue field-edit replay that wedges permanently, a silently-desyncable ImWchar ABI, and a bare swallow of untrusted Plane JSON.

Origin: session audit workflows `wf_8406f84f-757` (40-finding synthesis) + `wf_e18e1d41-69f` (round-2). The raw 40-finding JSON, per-bug fix specs, and round-2 evidence are staged in session scratch (not committed here); the actionable roadmap with evidence refs is inlined below, and every line-number claim was re-verified directly against the tree.

## Approach

Fix in place — no language/std change. Three phases, weighted to protect **both** code-quality and production-speed, never trading a quality gate for speed:

1. **Sprint 1 — stop false-green / fail-open.** All S-effort; each closes a class where the build or CI is green while a real defect or a dropped correctness patch escapes. Includes the four real bugs. Highest quality-per-hour, and several also remove agent/CI round-trips (speed).
2. **Sprint 2 — structural.** Attack the two root multipliers (the `json.hpp` / sol2 / `AppController` header choke; the missing automated safety nets) plus throughput (a merge queue + gate-correctness).
3. **Backlog / accept.** The irreducible C++14/dual-target floor — pay down ride-along, never as a dedicated sprint.

Round-2 corrected two would-be-wasted Sprint-2 efforts (the `json_fwd` swap is ineffective in isolation; ThreadSanitizer cannot run on the Windows runners), captured inline so the structural work targets the real levers rather than the audit's first guess. Trade-off that shaped the sequencing: the four bugs + the dependency-patch hardening lead because they are correctness (Pillar-3), not optimisation — speed work waits behind them.

## Roadmap

### Sprint 1 — stop false-green / fail-open (all S-effort)

- **[#1] `smatchet_patch_or_die`** — route all vendored-source patches (sol2 metatable token, cpr, sqlitecpp, ghc, luaconf) through one helper that `string(FIND)`s its target and `FATAL_ERROR`s if absent, then SHA-pin cpr/sqlitecpp/sol2. Today the sol2 patch's only guard is a change-detector (`CMakeLists.txt:609`); a dep bump silently no-ops it → heap corruption returns. **Top recommendation — do first.**
- **[#16] OfflineQueue latch reset** — `OfflineQueueService.cpp:666-668` returns on null `Mutations()` without releasing the in-flight latch the other three early-returns release → field-edit replay wedges permanently → offline edits silently lost.
- **[#23] ImWchar `static_assert`** — emit `static_assert(sizeof(ImWchar)==4)` from a DX12 host TU; WCHAR32 parity is hand-synced across three points (`SmatchetImConfig.h:15`, `Build.cs:12`, packaged `imconfig.h:70` commented out) with no compile-time guard → silent ABI/text-memory corruption.
- **[#37] Plane empty-catch marker** — `PlaneIssueMutation.cpp:327` bare `catch(...){}` swallows untrusted-JSON parse with no log/marker (strict-zone CRITICAL).
- **[#3]** delete dead `#include "JiraClient.h"` (`AppController.h:40`) — the only `JiraClient` occurrence in the header is the include itself; drags cpr+json into its 105 direct includers.
- **[#4]** point the `**/Locales/*.json` visual-validation glob (`project.config.json:91`) at the real localization source (it matches zero tracked files) + add a zero-match selftest.
- **[#5]** `tests/CMakeLists.txt` is a hand-maintained `add_executable` list (no GLOB) — add a configure-time glob-vs-list assert so a new test `.cpp` can't be silently uncompiled (false green).
- **[#6]** add `scripts/dev/is-exe-fresh.sh` source-vs-exe mtime preflight wired into the run path — exe-staleness is prose-only today.
- **[#15]** decomposition verification must assert the function is ABSENT from the absolute size list, not just pass `--diff` (a still-over-cap partial reduction currently passes).
- **[#24]** add a `no-glfw-in-core-headers` grep delta rule to `test-lint-rules.sh` (the rule is prose-only).
- **[#33]** document the C2859 / stale-PCH ~4s targeted regen + the `SMATCHET_USE_PCH=OFF` escape in `build.md`.
- **[#38]** prune the retired `ninja-iter-msys2` build dir + codex debris.

### Sprint 2 — structural (estimates use the round-2-corrected numbers)

Root multiplier A — the `json.hpp` / sol2 / `AppController` header choke:

- ~~**[#2]** `json_fwd` swap of the 3 named headers~~ — **NO-GO in isolation** (round-2): relieves only 4 of 298 json-pulling TUs because `Commands/Command.h` (201 TUs) + `AppController.h` (185 TUs, `parametersSchema`) hold `nlohmann::json` **by value** → can't forward. Unblock via #19 first.
- **[#19]** pImpl `AppController` (8,022 LOC across `AppController*.cpp`; 105 direct / ~114 transitive includers; lifts sol2's 22k-line header out of the public graph) — also the prerequisite for any `json_fwd` payoff.
- **[#20/#28]** PCH json: Standalone = NO-GO ("net wash" confirmed at `SmatchetPch.h:17-21`); **Core_DX12 json-PCH = GO for a one-shot trial** — PCH-less today (`CMakeLists.txt:1526` is Standalone-only) yet 64% of its 225 TUs pull json, never measured. ghc-in-PCH = NO-GO (6-7% reach). Tests get a `REUSE_FROM` PCH.

Root multiplier B — missing automated safety nets:

- ~~**[#10]** TSan on existing runners~~ — **NO-GO** (round-2): clang-cl has no Windows TSan runtime; `Sanitizers.cmake:86-92` hard-returns under MSVC/clang-cl; no Linux C++ build exists. Redirect to its own scoped slice: a new `ubuntu-latest` nightly TSan job over the portable pure-logic + threading subset — requires standing up a Linux Clang preset (none today) + confirming Core/Sync/Tracker Linux-portability.
- **[#11]** per-PR UBSan (runs under clang-cl today) + a nightly Whisper/Lua-ON sanitizer.
- **[#12]** bucket-E render coverage for the 8 remaining data-dependent UI windows.
- **[#8/#13]** revive the perf gate — commit `ci-windows-latest` baselines, fix the headless launch, enforce 6.94 ms + p99.

Throughput (protect speed without weakening checks):

- **[#14]** GitHub merge queue for develop — justified by the O(n) per-PR rebuild tax under `strict=true` (build full ≈786s / 1.8× the next required check), not by an extreme single pole.
- **[#7]** tighten the comment-noise gate (the #1 build-green-CI-red cause) + wire `pre-ship.sh`'s format-then-gate order into the Stop hook.
- **[#9/#18]** de-dup the coverage / perf cold-rebuilds (redundant whole rebuilds — the higher win).
- **[#29]** split the serial windows-msvc 2-preset configure — **LOW** (~100s/run).
- **[#21]** project-local `Optional<T>` / `Result` for the 122 `outError` + 19 `Try*` patterns.
- **[#22]** SHA-pin remaining deps + a CI smoke for the load-bearing Lua mirror.
- **[#25]** a CI step exercising the `SmatchetPackageUnrealLibs_DX12` packaging graph.
- **[#31/#32]** extract cpr-free `*Pure` Tracker logic for unit tests; fix the flaky-test env gate + add a quarantine tag.

### Backlog / accept — irreducible floor (ride-along only)

- **[#26]** 116 ImGui-draw monoliths — immediate-mode, not a C++14 defect.
- **[#40]** 449 per-backend clones — ISP-correct; extract only the AI streaming-parse.
- **[#34]** publish LTO link (publish-only, correctly scoped).
- **[#35/#36]** configure re-surgery stamp; dynamic perf-scope-name map growth.

## Files to modify

Sprint 1 (the first PR or two — split along seams if the diff exceeds the per-PR ceiling):

1. [`cmake/SmatchetThirdParty.cmake`](../../../cmake/SmatchetThirdParty.cmake) — add `smatchet_patch_or_die` helper (#1).
2. [`CMakeLists.txt:564`](../../../CMakeLists.txt) — route the sol2 + cpr/ghc/luaconf patches through the helper; SHA-pin cpr/sqlitecpp/sol2 (#1).
3. [`Source/Core/src/Sync/OfflineQueueService.cpp:666`](../../../Source/Core/src/Sync/OfflineQueueService.cpp) — reset the latch under the schedule mutex before the null-Mutations return (#16).
4. [`Source/Core/src/Tracker/PlaneIssueMutation.cpp:327`](../../../Source/Core/src/Tracker/PlaneIssueMutation.cpp) — add the `// catch-all-ok:` marker (#37).
5. A DX12 host TU ([`Source/Core/src/Ui/SmatchetImGuiHost.cpp`](../../../Source/Core/src/Ui/SmatchetImGuiHost.cpp), confirmed to include `imgui.h`) — `static_assert(sizeof(ImWchar)==4)` (#23). Route via `unreal-bridge`.
6. [`Source/Core/include/AppController.h:40`](../../../Source/Core/include/AppController.h) — delete the dead `JiraClient.h` include (#3).
7. [`project.config.json:91`](../../../project.config.json) — fix the visual-validation glob (#4).
8. [`tests/CMakeLists.txt:12`](../../../tests/CMakeLists.txt) — glob-vs-list configure assert (#5).
9. [`agents/scripts/project/test-lint-rules.sh`](../../../agents/scripts/project/test-lint-rules.sh) — `no-glfw-in-core-headers` rule (#24); `--scan-file` absolute-list assert (#15).
10. `scripts/dev/is-exe-fresh.sh` (new) + the relaunch path — exe-staleness preflight (#6).
11. [`docs/agent-rules/build.md`](../../agent-rules/build.md) — C2859 / stale-PCH recovery note (#33).

Sprint 2 file set (interface headers, `AppController.{h,cpp}`, the PCH headers + `CMakeLists.txt`, `.github/workflows/*`, `cmake/Sanitizers.cmake`, `tests/ui/`, `docs/perf/baselines/`) is scoped per-slice at execution time — each slice is its own PR.

## Existing utilities reused

- `WarnIfPackagedLibsAreStale()` mtime-comparison pattern in `SmatchetImGuiPlugin.Build.cs` — reuse the shape for `is-exe-fresh.sh` (#6).
- The existing grep-rule pattern in `test-lint-rules.sh` (`no-printf-stderr`, `define-imgui`) — clone for `no-glfw-in-core-headers` (#24).
- `Config/ConfigManager.h:21` + `Config/ToolbarConfig.h:15` already use `json_fwd` — the in-repo precedent for #2 once unblocked.
- `function_size_audit.py --scan-file` / `--list` — the absolute-list assert for #15.
- The three sibling early-returns in `OfflineQueueService.cpp:648/653/658` — the exact reset pattern bug #16 must mirror.
- The `// catch-all-ok: <reason>` marker convention (e.g. `Config/ConfigManager.cpp:633`) — the fix shape for #37.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: #13 makes the budget enforced (it is not today); the pImpl/header changes (#2/#19/#20) are compile-time only — no runtime delta. Each Source/Core slice runs its mapped perf scenario before merge.
- **Pillar 2 (UI never blocks > 100 ms)**: no new sync I/O on the UI thread is introduced; #11 adds the sanitizer coverage that protects the existing dispatch model.
- **Pillar 3 (never crash)**: the headline of Sprint 1 — bugs #16 (data loss), #23 (memory corruption), #37 (swallowed error), and #1 (fail-open heap corruption) are all Pillar-3 fixes.
- **Pillar 4 (accessibility)**: N/A — no accessibility surface is touched by this plan.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

**Fires** — Sprint-1 #16/#23/#3 and all of Sprint-2's structural items touch `Source/Core/`.

1. **PR-fast CI** — the header/pImpl slices (#2/#19/#20) touch the `AppController` / `Command` hub → scenario **`idle`** (`command-palette-fuzzy` is bucket-C-only — requires `--screenshotPath` — per `agents/core/perf-gatekeeper.md` § Curated diff → scenario map); a slice that also touches grid files adds `priority-grid-scroll`. The Sprint-1 bug fixes are off any hot path.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*` is added by any slice.
3. **Dispatcher drain** — bug #16 touches `OfflineQueueService` replay scheduling, not `MainThreadDispatcher::Drain()`; no change.
4. **Visible-cue bucket-E harness** — no new > 100 ms sync-stall code path.
5. **Marker inventory** — #36 (backlog) would change a `SMATCHET_UI_PERF_SCOPE` name; regen `docs/perf/MARKER_INVENTORY.md` in that PR. #13 is itself a perf-system change → re-baseline intentionally with golden-approval.

**Pre-push local check** (self-contained): `bash scripts/dev/perf-run.sh <scenario>` then `python scripts/dev/perf-compare.py docs/perf/baselines/<scenario>.<host>.json build/perf-runs/<scenario>-<ts>.json` for the scenario(s) named above, before opening any Source/Core PR (full procedure: `docs/guides/perf-workflow.md` § Gate-check).

## Risks / non-goals

- **Risk — deleting `AppController.h:40` (#3) breaks a transitive consumer** that relied on `AppController.h` to pull `JiraClient.h`. Mitigation: the dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) catches it; the 3 `.cpp` that use the type already include it directly.
- **Risk — the Core_DX12 json-PCH trial (#20) may not pay off.** Mitigation: it is a measurement, not a commitment — revert if there's no wall-clock win.
- **Risk — `smatchet_patch_or_die` (#1) over-fires** if a future dep legitimately already matches. Mitigation: the helper returns early when the replacement text is already present (idempotent).
- **Non-goal — any language/std change.** This is the rejected premise of the whole audit.
- **Non-goal — the backlog/accept tier as a dedicated sprint.** Ride-along only.
- **Non-goal — applying the staged bug fixes inside this PR.** This PR is the plan only; the four fixes ship as their own subsystem-routed PRs.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: bug #16 — a `TickOfflineFieldEdits` test with null `Mutations()` asserting a later tick is not latched out; #5/#15/#24 each ship a `--selftest`/bats case proving the gate fails on bad input and passes on good.
- **Bucket E (ImGui Test Engine)**: #12 adds boot-open-assert-widget smokes for the 8 uncovered windows.
- **Bash-driver scenario / screenshot / sanitizer**: #11/#13 wire the per-PR UBSan + perf gate; #1 — a CI smoke asserting the sol2 metatable token is present after configure.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) on every Source/Core slice — the #23 `static_assert` must compile in both worlds.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before executing Sprint 1; record the outcome.
- **Manual residue**: none introduced. Each slice's verification is automated per the bucket above; the only judgment step (TSan Linux-portability for #10) is a scoped slice, not silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Any language/std swap (vlang, C++17/20) — the rejected premise.
- The backlog/accept tier (#26/#40/#34/#35/#36) — ride-along, no dedicated work.
- Applying the four staged bug fixes — separate subsystem PRs (`offline-sync`, `tracker-backend`, `unreal-bridge`, `build-doctor`).
- The raw 40-finding audit JSON + per-bug fix specs — session scratch, not committed; this plan inlines the actionable subset.
- The 4 developer-owned `SmatchetPreferencesUi*` / UI WIP files — untouched.

## Implementation log
*Sprint 1 shipped; Sprint 2 + hygiene #3/#5/#6 remain → plan stays `active` (not archived).*

- `4cc7c4a6` (#905) · #16 OfflineQueue: reset the in-flight latch on the null-`Mutations()` early-return (mirror the 3 sibling returns) + a discriminating runtime test (count 0→1 across the reset).
- `57789552` (#906) · #37 Plane `CreateIssue`: `LOG_WARN` instead of an empty `catch(...){}` on response-parse (Network/API tier) + `catch-all-ok` markers on the 2 sibling parse-fallbacks.
- `909aa2a8` (#907) · #23 ImWchar: `static_assert(sizeof(ImWchar)==4)` in `SmatchetImGuiFonts.cpp` guarding `IMGUI_USE_WCHAR32` parity (compiled in both targets).
- `8a06ae74` (#908) · #1 `smatchet_patch_or_die` helper routing all vendored patches (FATAL on a vanished target) + SHA-pin cpr/sqlitecpp/sol2 + a sol2-token configure-time assert.
- `c3c9a72b` (#909) · #33 `build.md`: MSVC toolset-pin (canonical `STL1001` / `with-msvc-env.sh` / `msvc_toolset_pin`) + stale-PCH (C2859) recovery.
- `272dbabc` (#911) · #24 `no-glfw-in-core-headers` lint rule + #15 `function_size_audit.py --assert-absent` + #4 repoint dead `Locales/*.json` glob at the real source (+ `test-config-globs.sh`).
- #38 (prune retired msys2 debris): already removed in `6537dc3` (`bootstrap-msys2.ps1` + `*-msys2` presets); leftover untracked `build/*msys2*` dirs cleared locally.

## Deviations from plan
- **#37**: planned shorthand was "add the `// catch-all-ok:` marker"; upgraded to a policy-mandated `LOG_WARN` (`exception-handling-policy.md` Network/API tier — a 2xx-response parse failure is not an escape-hatch case).
- **#23**: planned home was `SmatchetImGuiHost.cpp`; relocated to `SmatchetImGuiFonts.cpp` (leaner DX12+GLFW TU with no `SmatchetUI.h`, clears the strict clang `-Wmicrosoft-include` lint, and is the semantic home — it builds the ImWchar glyph-range arrays). Compiles in both targets (broader than the planned DX12-only TU).
- **#4**: there are **no tracked `Locales/*.json`** (runtime override files placed next to the exe); repointed the glob at the real tracked source (`SmatchetLocalization.cpp` + `SmatchetLocaliz*.h`) and added `test-config-globs.sh` (fail-closed zero-match).
- **#15**: implemented as a new `function_size_audit.py --assert-absent <name>` mode (exit 1 if still over-cap) rather than a bare `--diff` check.
- **Merge mechanics**: a 6-PR-per-feature split (vs the PR-batching "one PR per feature") exhausted CodeRabbit's hourly quota → `cr-out-of-band` ×4 (#905-908) + `tests-out-of-band` (#906/#907) + 2 strict-`BEHIND` admin force-merges (#908/#911). Postmortem: `postmortems.md` 2026-06-06.

## Verification (actual)
- **Dual-target build** (`ninja-iter-msvc`, `SmatchetStandalone` + `SmatchetCore_DX12`) green for the bug fixes (#16/#23/#37); #1 verified configure-idempotent + negative-test FATAL + dual-target build.
- **ctest** (`ninja-test-msvc`) 100%; the #16 discriminating case runs (8/8 assertions).
- **#24/#15/#4**: `test-lint-rules.sh --selftest` + `lint_rules.bats` (27) + `function_size.bats` (19) + `function_size_audit.py --selftest` + `test-config-globs.sh` + doc suite — all green.
- **#33**: `scripts/dev/test-docs.sh` 9/9.
- **Not yet done** (remaining Sprint-1 hygiene): #3 (drop dead `JiraClient.h` include), #5 (tests glob-vs-list assert), #6 (`is-exe-fresh.sh`).

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
