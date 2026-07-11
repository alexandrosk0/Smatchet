# Smatchet QA Engineering Evaluation (without agents.md)

**Evaluator lens:** QA Director — test strategy, coverage, test types, defect prevention, release validation, CI quality gates, flake management, confidence.
**Date:** 2026-06-30
**Verdict scope:** Tangible test and CI artifacts only. The agentic-governance meta-layer was deliberately excluded (see Scope & Method).

---

## 1. Executive Summary + QA-Maturity Verdict

Smatchet presents a **mature, multi-layered quality engineering operation** that is well above what a typical solo-developer / small-team desktop C++ project ships. The product is a native (Dear ImGui / GLFW / DX12) ticket-tracker client with AI assistant, Whisper dictation, Lua automation, an MCP server, and multiple backend integrations (Jira, Plane, GitHub, Linear, Perforce). Against that surface area the team has assembled **307 C++ test files** (~40,800 lines in the Core unit tier alone, **2,199 `TEST_CASE` + 502 `SUBCASE`** in Core), a doctest rig wired into CMake/CTest, an ImGui Test Engine UI tier, a libFuzzer fuzzing tier, golden-image visual snapshots, deterministic backend fixtures, and a dense battery of GitHub Actions quality gates.

The standout strengths are: (a) a **structural test-delta gate that is hard-blocking from day one** — you cannot change `Source/Core` behavior without a paired test delta; (b) **per-PR ASan and UBSan as required branch-protection contexts** (not merely nightly), plus an additional combined ASan+UBSan nightly and a Linux TSan nightly for data races; (c) a **blocking line-coverage gate at 65%** plus a **per-file ≥90% floor on named high-risk security units** (SSRF endpoint sanitizer, credential redaction, JQL escaper); (d) a **required perf-regression gate** with two-sample confirmation and noise-floor calibration; and (e) a **genuinely candid, self-maintained gap inventory** (`docs/guides/testing-surface.md` § 5) that ranks its own blind spots and cross-references each to a backlog item.

The most material weaknesses are: the **visual/interaction UI lane is non-gating** (26 UI tests + 3 goldens run `continue-on-error` on flaky Mesa GL, so UI regressions cannot block a merge); the **absolute frame-budget perf ceiling (6.94 ms = 1000/144 Hz) is configured but DISABLED** (`mean_abs_ceiling_ms: null`) so only *relative* regression is enforced; **fuzzing and TSan are advisory, not gating**, and the fuzz corpus is thin (3 seeds per parser); the **test-delta gate verifies a test changed, not that it asserts anything** (no mutation testing); and several UI tests are hand-maintained **replicas** of production code carrying explicit drift warnings rather than exercising the production path.

**QA-maturity verdict: 8.0/10 — Advanced.** The gate architecture, sanitizer discipline, and self-awareness are excellent; the residual risk concentrates in the unenforced UI/visual lane, the disabled absolute perf budget, advisory fuzz/TSan lanes, and assertion-quality blind spots.

---

## 2. Scope & Method

**What I evaluated:** `tests/**` (Core, Plugins, Lua, ui, golden, fuzz, bats, live, fixtures, support, _mocks); `.github/workflows/**` (build-and-test, coverage, coverage-gate, sanitizer-nightly, tsan-linux-nightly, fuzz-smoke, perf-pr-fast, perf-full, mobile-emulator-smoke, mobile-security, doc-validation, codeql); the doctest/CTest rig via CMake presets; `backlog/MANUAL_TEST_QUEUE.md`; `scripts/publish/INSTALLER_SMOKE_TEST.md`; `docs/guides/testing-surface.md`; `docs/perf/regression-policy.json` + baselines; `project.config.json` branch-protection config.

**What I deliberately ignored (per evaluation rules):** the agentic-governance meta-layer — `AGENTS.md`, `agents/`, `AI_POLICY.md`, `docs/agent-rules/**`, `docs/harness/**`, `docs/self-improvement/**`, `docs/agent-eval/**`, `.coderabbit.yaml`, `.cursor/`. I also did **not** count the agentic governance test surface toward product QA. Notably, **~52 of 56 `.bats` files test the CI/agentic tooling itself** (merge gates, lock claims, fleet preflight, plan locks, postmortem-owed, issue sweep, workflow watchdogs) — not the product — and `tests/agent-eval/` (4 files) is meta-layer. These were inventoried for completeness but explicitly discounted from the product test-pyramid assessment below. I counted with `find`/`grep` and read representative tests and workflows directly.

**Method:** opened representative tests in each tier, read all six core gate workflows end-to-end, counted files per category, and cross-checked the project's own declared gap list against the artifacts (catching at least one place where the doc is now stale — see § 5).

---

## 3. Test Inventory & Pyramid

| Category | Location | Count | Notes |
|---|---|---|---|
| **Unit / Core (doctest)** | `tests/Core/*.cpp` | **222 files** | 2,199 `TEST_CASE`, 502 `SUBCASE`, ~40.8k LOC. The base of the pyramid. |
| **Plugin tests** | `tests/Plugins/{Mcp,Whisper,LuaConsole}` | 7 files | MCP = 5 (envelope, dispatch, request parser, tool schemas, host origin); Whisper = 1; LuaConsole = 1. |
| **Lua sandbox tests** | `tests/Lua/*.cpp` | 6 files | Sandbox escape, timeout, JSON convert, bounded JSON parse, bindings, stub compile. |
| **UI tests (ImGui Test Engine)** | `tests/ui/*.test.cpp` | **40 files** | Bucket-E headless ImGui driver; includes deterministic Jira/Linear backend UI tests, AI assistant flows, keybindings, command palette. |
| **Golden / visual snapshots** | `tests/golden/` + `tests/support/*Main.cpp` | 3 PNG goldens + diff harness | Syntax coloring, command-palette fuzzy, dock-gap sentinel; pink-pixel-count + screenshot-diff + dim-guard harnesses in `tests/support`. |
| **Fuzz (libFuzzer)** | `tests/fuzz/fuzz_*.cpp` | **6 drivers** | ai_ndjson, ai_sse, callstack, cpp_lex, image_dims, markdown_adf. |
| **Fuzz corpus** | `tests/fuzz/corpus/` | 18 seed files (3/driver) | Thin seed sets. |
| **Live / integration** | `tests/live/` | 1 file | `linear_live_smoke.cpp` (gated, needs live token). |
| **bats (shell)** | `tests/bats/*.bats` | 56 files / 914 `@test` | **~52 are CI/agentic-governance tooling tests (meta-layer, discounted).** Product-adjacent: shell_lint, function_size, build_warnings_gate. |
| **Fixtures** | `tests/fixtures/**` | 49 JSON + assorted | Jira (5), Plane (1), GitHub (1), P4 (2), merge-gates, CI-parity, Whisper, Ollama. |
| **Mocks / support** | `tests/_mocks`, `tests/support` | 1 mock + ~35 fixtures | FakeTrackerClient, FakeGitHubFixture, FakePlaneFixture, JiraFakeTrackerFixture, JiraCatalogHttpFixture (in-process httplib loopback), StubAiClient, SqliteMemFixture, TempDbFile, etc. |

**Pyramid shape:** healthy and **base-heavy**, the correct shape. ~222 Core unit files dominate, a narrower but real UI/integration tier (40 UI + plugin + Lua), a thin fuzz tier, and a single live smoke at the apex. The product C++ test count is **307**, matching the project's own "307 total test files" figure. The pyramid's main distortion is that the **UI tier, while populous (40 files), is non-blocking** (§ 4), so its defect-prevention value is advisory in practice. The bats tier is huge (914 assertions) but almost entirely validates the agentic CI machinery rather than the product, so it does not count toward product confidence.

---

## 4. Quality Gates Analysis (blocking vs advisory, thresholds)

The required-context set (from `project.config.json` § branch_protection.required_contexts) is the authoritative gate list. Ten contexts are **branch-protection required**:

1. **Test-delta gate** (`coverage-gate.yml`) — **BLOCKING from day one.** Any PR touching `Source/Core/src/*.cpp` without a paired test delta under `tests/` is rejected. Dismissable only via the `tests-out-of-band` PR label (for docs/include-shape/build-system changes). Self-gates green on `merge_group`. *Structural, not threshold.* This is the single most valuable defect-prevention mechanism here — it makes "change behavior → add a test" non-optional.
2. **Windows + MSVC** and **Windows + MSVC (light: AI/Whisper/MCP off)** — full build + CTest, required. The "light" variant guards the feature-flag-off configuration.
3. **Coverage (windows-2022 + OpenCppCoverage)** — **BLOCKING** (`continue-on-error: false`, `--threshold 65`). The 65% floor is data-chosen (first real measurement was 67% on the Ui-excluded `Source/Core` surface) with a raise-to-70 ramp tracked. Escapable via `coverage-out-of-band` label. **Plus** a per-file **≥90% floor on named high-risk units** (SSRF endpoint sanitizer, credential redaction, JQL escaper, tracker HTTP classifier) — fails-open on missing XML so it never wedges an unrelated PR. Self-gates on docs-only PRs. This is a genuinely strong coverage posture: a global floor *and* a targeted high-risk floor.
4. **Sanitizer (ASAN via MSVC)** and **Sanitizer (UBSan via Clang)** — **per-PR BLOCKING**, path-scoped to first-party C++ changes (`Source/Core|Plugins` `.cpp/.h/.hpp/.cc`). Builds Debug (uncacheable), instruments the doctest rig itself. This contradicts the older sanitizer-nightly.yml header comment ("per-PR ASan was rejected as too slow") — per-PR sanitizers **were** subsequently added and are now required. The **nightly** `sanitizer-nightly.yml` (Clang ASan+UBSan combined, the only preset giving both) is the develop backstop and auto-opens a `bug`/`build-break` issue (it even distinguishes compile-break from sanitizer-finding for correct routing).
5. **Perf PR-fast (windows-2022)** — **required context, but enforcement is partial.** It detects *relative* regression vs `docs/perf/baselines/*` per `docs/perf/regression-policy.json` (default `mean_delta_pct: 10%`, `p99_abs_ceiling_ms: 10`, `max_abs_ceiling_ms: 50`, `mean_min_abs_delta_ms: 0.05` noise floor). Regressions must fail **both independent median samples** to red the check (false-positive guard). A run/plumbing failure (`exit 1`) is *not* override-able by `perf-out-of-band`. **Critical caveat:** the absolute frame budget `mean_abs_ceiling_ms` (target **6.94 ms = 1000/144 Hz**) ships **`null` / DISABLED** pending a "perf-gate-revival step-5 calibration pass." So the headline 144 Hz budget is **documented but not enforced** — only relative drift is gated.
6. **Doc anchors + agent contract**, **Shell lint**, **Comment-noise + high-integrity gate** — required hygiene gates.

**Advisory / non-required (nightly + PR-path-triggered):**
- **Fuzz smoke** (`fuzz-smoke.yml`) — explicitly ADVISORY; PR crashes are `continue-on-error` (stochastic corpus-luck), only the nightly cron hard-fails and auto-opens an issue. A real *broken build* still reds the check.
- **TSan Linux nightly** (`tsan-linux-nightly.yml`) — ADVISORY; the only real data-race gate (TSan has no Windows toolchain). Curated ImGroup-free threading subset (`SmatchetTsanTests`): LocalCacheManager SQLite cache + Pure units. `halt_on_error=1`.
- **Mobile emulator smoke / mobile security** — advisory (boot APK + first frame).
- **CodeQL** — security static analysis.

**Gate strength summary:** the *correctness* gates (build, unit, test-delta, sanitizer, coverage) are strong and blocking. The *visual, perf-absolute, fuzz, and concurrency* gates are advisory or partially disarmed. This is a defensible solo-project trade-off (per-PR ASan + nightly everything-else), but it means three real defect classes — UI visual regressions, absolute frame-budget overruns, and newly-discovered parser crashes — can land on `develop` and only surface in nightly or never.

---

## 5. Test Depth by Area

**Backends (Jira / Plane / GitHub / Linear / Perforce).** Tested **without live services** via two layers: (1) `FakeTrackerClient` / `FakeGitHubFixture` / `FakePlaneFixture` / `JiraFakeTrackerFixture` — scripted in-memory `ITrackerBackend` implementations with per-call recording for "PUT called once with this payload" assertions; (2) JSON fixtures under `tests/fixtures/{jira_backend,plane,github,p4}`. Core has 15 `Tracker*`, 6 `Jira*`, 5 `Plane*`, 7 `GitHub*`, 4 `Linear*`, 4 `P4*` test files. **Notably, the project's own gap doc (§ 5 gap #2) claims "no HTTP-transport integration tests" — this is now STALE:** `tests/Core/TrackerHttpFaults.test.cpp` drives the *real* cpr HTTP helpers against an in-process httplib loopback (`JiraCatalogHttpFixture`) and asserts the safe-tiered retry policy end-to-end (idempotent GET/PUT retry on 5xx/429; non-idempotent POST single-shot so a landed mutation never double-fires), with the pure classify table in `TrackerHttpRetry.test.cpp`. Backend depth is therefore **better than the project itself documents** — transport fault injection exists, at least for catalog paths.

**UI.** 40 ImGui Test Engine tests covering AI assistant chat (Enter-send, Ctrl+Enter newline, model strip, dock swap, preferences save/discard/validation/test-connection), keybindings editor, command palette, omnibar, notification center, offline conflict modal, views/columns reorder, deterministic Jira/Linear backend rendering, mobile confirm modal. **Two structural caveats:** (a) several tests are explicit **hand-maintained replicas** of production code (e.g. `ai_assistant_enter_send.test.cpp` replicates `SmatchetAiAssistantUi.cpp::DrawInputAndButtons` with a "Drift warning — IF YOU CHANGE …, UPDATE THIS REPLICA" header) — they assert a *copy* of the contract, not the production widget, so they can pass while production drifts; (b) the entire bucket-E/visual lane is **non-gating** (continue-on-error on flaky Mesa GL). So UI breadth is high but its merge-blocking power is near zero.

**Plugins (MCP / Whisper / Lua).** MCP is the best-tested plugin: 5 tests (envelope framing, dispatch, request parsing, tool schemas, host-origin validation) plus `McpServerInfoTextPure` in Core and an MCP/Lua fresh-state race UI test. Lua has a dedicated 6-file sandbox suite (sandbox escape, timeout, bounded JSON parse, bindings) — appropriate for an untrusted-script surface. Whisper: 6 Core tests (consent gate, API payload, mode router, mock seams, prefs, API-key resolve) + 1 plugin test, using `tests/_mocks/openai-whisper-response.json`.

**AI / streaming / MCP.** 28 `Ai*` Core files — strong. Includes `AiSseParser`, `AiNdjsonParser`, `OllamaStreamError`, `AiAssistantStreamHandoff`, `AiClientCancel`, `AiClientErrorRedact`/`AiErrorRedact` (credential leakage), `AiEndpointSanitize` (SSRF), `AiOutboundConsent`, `AiLuaPromptRateLimit`, `EmailMask`. The streaming *parsers* are example-tested **and** fuzzed (`fuzz_ai_sse`, `fuzz_ai_ndjson`). The *streaming UX under partial/torn frames and mid-stream cancel* is the thinner spot, exercised mostly at unit level via `StubAiClient`.

**Ingress / parsers (untrusted bytes).** This is the security-critical surface — tracker JSON, AI SSE/NDJSON, p4 annotate, callstacks, markdown/ADF, image dims, WAV. Six are fuzzed (ai_ndjson, ai_sse, callstack, cpp_lex, image_dims, markdown_adf). **Gaps vs the full untrusted set:** WAV writer, p4 annotate, and tracker JSON ingest are example-tested only, not fuzzed. Corpus depth is thin (3 seeds/driver), which limits early coverage-guided discovery.

---

## 6. Gaps & Risks

The project maintains its own ranked gap list in `testing-surface.md` § 5; I validated it and add my own. Ranked by residual risk:

1. **Visual/interaction lane is non-gating (highest leverage).** 26 UI tests + 3 goldens are blanket `continue-on-error` on flaky Mesa GL — a UI/render regression cannot block a merge. Tracked, unfixed.
2. **Absolute 144 Hz frame budget disabled.** `mean_abs_ceiling_ms: null` — the marquee 6.94 ms ceiling is not enforced; only relative drift is. A scenario can be slow *in absolute terms* from day one and never trip the gate. The relative gate also only catches scenarios that emit `rows[]` (the doc notes "8 of 15 scenarios don't emit rows", 7/15 baselined), so over half the perf scenarios aren't actually baselined.
3. **Fuzzing is advisory + thin.** PR fuzz crashes don't block; only the nightly hard-fails. Corpus is 3 seeds/driver. Several untrusted parsers (WAV, p4 annotate, tracker JSON) aren't fuzzed at all. Direct partial-miss against a "never crash on untrusted input" goal.
4. **Test-delta gate ≠ assertion quality.** The gate proves a test file *changed*, not that it *exercises the diff* — a no-op/empty-assertion test passes it. No mutation testing closes this false-GREEN. (The project tracks only the *inverse* false-RED.)
5. **UI replica drift.** Replica-style UI tests assert a hand-copied contract; production can diverge silently between manual replica updates.
6. **Persistence corruption under-tested.** Cache/config open paths are happy-path; truncated-DB / schema-from-future / `SQLITE_BUSY` storm are only partially characterized (one in-review `LocalCacheManagerCorruption.test.cpp`); the config-open path and busy-storm remain open.
7. **TSan subset is narrow.** Only LocalCacheManager + Pure units are race-checked; the rest of the threaded surface (sync, dispatchers, MCP workers) is not under TSan, and it's nightly-only anyway.
8. **Error-path / negative-path coverage for AI streaming and MCP** is thinner than the happy path (mid-stream cancel, torn frames, MCP malformed-envelope storms).

---

## 7. Scorecard

| Dimension | Score | Rationale |
|---|---:|---|
| **Test breadth** | **9/10** | 307 product C++ test files across unit/UI/plugin/Lua/fuzz/golden/live; every major subsystem has files. Among the best I'd expect at this project scale. |
| **Test depth** | **7.5/10** | Deep unit + scripted-fixture + real-HTTP-loopback backend coverage; thinner on error paths, streaming-under-torn-frames, and UI (replicas, non-gating). |
| **Memory/concurrency safety testing** | **8.5/10** | Per-PR ASan + UBSan as *required* gates + combined nightly + TSan nightly. Docked only because TSan covers a narrow curated subset and is nightly-only. |
| **Fuzzing / security testing** | **6.5/10** | 6 libFuzzer drivers, CodeQL, dedicated SSRF/redaction/JQL units with ≥90% floor — strong. But fuzz is advisory, corpus is thin (3 seeds), and several untrusted parsers aren't fuzzed. |
| **Perf-regression testing** | **6.5/10** | Required gate with two-sample confirmation + noise-floor calibration is sophisticated; but the absolute 6.94 ms/144 Hz budget is *disabled* and <half the scenarios are baselined. |
| **Coverage enforcement** | **8.5/10** | Blocking 65% global floor (data-chosen, raise-to-70 ramp) **plus** per-file ≥90% on named high-risk units. Strong. Docked for the label escape hatch and Ui-excluded denominator. |
| **Release validation** | **7.5/10** | Documented Windows installer smoke (assets, portable ZIP, silent install/uninstall, user-data, Unreal/Fab bundles) + structured `MANUAL_TEST_QUEUE.md` (179 tracked items with ⏳/✅/❌ status). But these are *manual*, human-gated, not CI-automated. |
| **Flake / determinism** | **8/10** | Deterministic backend fixtures, spawn-warmup deterministic gate, two-sample perf confirmation, p99 warmup-frame exclusion, in-memory SQLite — strong flake discipline. The known Mesa-GL UI flake is *managed by quarantine* (continue-on-error) rather than fixed. |
| **OVERALL QA MATURITY** | **8.0/10** | Advanced. Excellent blocking correctness gates, sanitizer discipline, coverage rigor, and self-awareness; residual risk in the unenforced UI/visual lane, disabled absolute perf budget, advisory fuzz/TSan, and assertion-quality blind spots. |

---

## 8. Prioritized Recommendations

1. **Make the visual/UI lane gating (highest ROI).** Stabilize the Mesa-GL bucket-E/golden lane (pin a software-renderer image, increase warmup, retry-once-then-fail) and flip the 26 UI tests + 3 goldens off `continue-on-error`. Today the single largest test investment (40 UI files) produces zero merge-blocking confidence.
2. **Arm the absolute perf budget.** Run the deferred "step-5 calibration pass," set `mean_abs_ceiling_ms` to 6.94 ms (with documented per-scenario overrides for legitimately heavy scopes like `SmatchetUI::Draw`), and baseline the ~8 scenarios that don't yet emit `rows[]`. The 144 Hz promise is currently unenforced.
3. **Promote fuzz from advisory to a real backstop and deepen corpora.** Keep PR fuzz time-boxed/advisory, but (a) expand the corpus well beyond 3 seeds/driver (mine real tracker/SSE payloads), (b) add fuzz drivers for the remaining untrusted parsers (WAV, p4 annotate, tracker JSON ingest), and (c) wire crash artifacts into auto-seed-on-fix so the corpus compounds.
4. **Add a mutation-testing signal to close the test-delta false-GREEN.** A periodic (nightly/weekly) mutation run on the high-risk units would convert "a test changed" into "a test that actually kills mutants," closing gap #4 — the gap that lets a no-op assertion satisfy the day-one delta gate.
5. **Replace UI replicas with production-path drivers where feasible.** The drift-warning replica pattern is a maintenance liability; drive the real `SmatchetAiAssistantUi` widget under the ImGui Test Engine instead of asserting a hand-copied contract, so production drift fails the test rather than silently diverging.
6. **Finish persistence-corruption hardening.** Land the in-review `LocalCacheManagerCorruption` characterization, extend it to the config-open path and `SQLITE_BUSY` storms, and add a graceful-rebuild fix — corruption on cold open is a realistic field crash.
7. **Broaden the TSan subset and consider promoting it to a required gate on threading-path PRs.** Add sync, dispatcher, and MCP-worker TUs to `SmatchetTsanTests`; the curated cache-only subset under-covers the concurrency surface that ships the highest race risk.
8. **Automate a slice of the release/manual queue.** The installer smoke is scripted (`test_installer_smoke.ps1`) but human-gated; wire the silent install/uninstall + version-metadata + user-data checks into a release-tag CI job so the 179-item manual queue shrinks to genuinely human-judgment items (visual/UX) only.
