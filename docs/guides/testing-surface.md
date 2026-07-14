# Testing surface — overview & gap guide

**Purpose.** A standing map of *what is tested, how it gates, and where the holes
are* — so an audit of Smatchet's test coverage is a **re-run, not a re-derivation**.
Read this before proposing test work, scoping a "we need more coverage" task, or
reasoning about why a class of bug reached `develop`.

**Status.** Snapshot captured 2026-06-13 against `develop`. The numbers drift;
the *shape* (which lanes gate, which are advisory, which voids exist) is stable.
Refresh the numbers with the [§ Refresh recipe](#7-refresh-recipe) — don't trust a
stale count, re-run the one-liner.

Related: [`agents/core/test-author.md`](../../agents/core/test-author.md) (the
5-bucket taxonomy authority), [`quality-pillars.md`](../agent-rules/quality-pillars.md)
(the invariants tests defend), [`merge-gates.md`](../agent-rules/merge-gates.md)
(how checks become merge-blocking), [`ci-required-check-pattern.md`](../agent-rules/ci-required-check-pattern.md)
(why a check is/ isn't *required*).

---

## 1. Inventory (snapshot 2026-06-13)

| Layer | Count | Location | Runner | Gate status |
|---|---|---|---|---|
| Unit (doctest, pure C++14) | **156** | [`tests/Core`](../../tests/Core) | ctest (`ninja-test-msvc`) | **required** — `Windows + MSVC` |
| Lua sandbox/timeout/bindings | 4 | [`tests/Lua`](../../tests/Lua) | ctest | **required** |
| MCP dispatch/envelope/schema | 4 | [`tests/Plugins/Mcp`](../../tests/Plugins/Mcp) | ctest | **required** |
| LuaConsole plugin | 1 | [`tests/Plugins/LuaConsole`](../../tests/Plugins/LuaConsole) | ctest | **required** |
| Bucket-A CLI probe (`test-*.sh`) | 39 | [`scripts/dev`](../../scripts/dev) | `test-all.sh` | partial — non-UI subset in `Windows + MSVC` |
| Bucket-E ImGui Test Engine | **26** | [`tests/ui`](../../tests/ui) | `ninja-ui-test-msvc` | ⚠ **advisory** (`continue-on-error`) |
| Bucket-C screenshot diff | **3 goldens** | [`tests/golden`](../../tests/golden) | Mesa headless | ⚠ **advisory** (`continue-on-error`) |
| Bucket-D sanitizer (ASan/UBSan/TSan) | — | CI presets | ASan/UBSan/TSan | ⚠ poller-soft (not branch-required) |
| bats (agentic tooling) | 34 | [`tests/bats`](../../tests/bats) | bats | pre-push + `Shell lint` |
| agent-eval | **3** | [`tests/agent-eval`](../../tests/agent-eval) | `agent_eval_run.bats` | thin |
| Fake/support fixtures | 18 | [`tests/support`](../../tests/support) | — | (shared seams) |

Source under test: **255** `.cpp` in `Source/Core/src` (largest subsystems: Ui 75,
Tracker 52, Commands 51).

**Reading the table:** the strength is the 156 pure-logic units — they span every
subsystem and gate on every PR via ctest. The weakness is dynamic/visual: the two
lanes that actually exercise rendered UI and interaction (bucket-C, bucket-E) are
`continue-on-error` and therefore advisory. See § 3.

---

## 2. The 5-bucket taxonomy (authority: `test-author.md`)

Every verification item maps to one bucket. If it doesn't, the gap is **test
infrastructure**, never "manual forever."

| Bucket | Shape | Tactic | Gating reality |
|---|---|---|---|
| **A** Headless CLI probe | "fn X returns Y" / Lua outputs Z | `debug.<feature>_test` JSON + bash assert | required (non-UI subset) |
| **B** Scenario + `perf.snapshot` | "frame ≤ N ms" / "cache hit 100%" | `IScenario` drives N frames, asserts rows | perf-fast subset required |
| **C** Screenshot diff | "cell renders red" / "icon visible" | PPM sentinel-colour scan vs golden | **advisory** |
| **D** Sanitizer build | "no UAF on shutdown" / "no leak" | run scenario under ASan/UBSan; exit code = assertion | poller-soft |
| **E** ImGui Test Engine | "drag column" / "type → autocomplete" | drives real ImGui widget tree | **advisory** |

---

## 3. CI gating map — required vs advisory (the asymmetry)

**Required contexts on `develop`** (branch protection, [`project.config.json`](../../project.config.json) § `branch_protection`) — 9 (Slice C promoted Coverage + both sanitizers, 2026-06):

1. `Test-delta gate` — every `Source/Core/` change needs a test-file delta (ubuntu)
2. `Windows + MSVC` — ctest (units/Lua/MCP) + non-UI bucket-A
3. `Windows + MSVC (Smatchet light — AI/Whisper/MCP off)` — dual-target + DX12 compile
4. `Shell lint (shellcheck)`
5. `Doc anchors + agent contract` — doc-validation suite
6. `Perf PR-fast (windows-2022)` — diff→scenario subset (Pillar 1)
7. `Coverage (windows-2022 + OpenCppCoverage)` — line-coverage floor (`--threshold 65`)
8. `Sanitizer (ASAN via MSVC)` — Debug ASan: android-openssl ctest probes + instrumented doctest rig (Core PRs; `skipped`=success otherwise)
9. `Sanitizer (UBSan via Clang)` — RelWithDebInfo Clang ASan+UBSan ctest (Core PRs; `skipped`=success otherwise)

> **Config↔live drift caveat:** these names are only *enforced* once
> `agents/scripts/core/setup-branch-protection.sh` re-runs (a manual full-replace PUT
> that lives in **no** workflow). Editing `branch_protection.required_contexts` is
> inert until then — the exact gap that let the #1227 red-Coverage merge escape (live
> ruleset had 6 contexts, config listed 7). Slice C re-ran the apply script as Phase 2.

**Outside branch protection (advisory or poller-soft):**

| Check | Trigger | Why not required | Risk |
|---|---|---|---|
| **Bucket-C screenshot diff** | PR (`needs: windows-msvc`) | `continue-on-error: true`; flaky **Mesa software-GL** | Visual regression never blocks merge |
| **Bucket-E UI tests** | PR | `continue-on-error: true`; same Mesa flakiness | 26 interaction tests never block merge |
| **TSan (Linux)** | PR (paths-scoped) + nightly cron | paths-scoped → can't be `required` (path-filter deadlock) | data-race detection advisory |
| **Sanitizer-nightly / perf-full / tsan nightly** | cron | backstop, not PR-gating | regressions surface next day |

**The load-bearing consequence.** UX Pillars 1-3 (perf / no-freeze / no-crash)
are described as auto-fail. Slice C closed the biggest gap — `Coverage` +
`Sanitizer (ASAN via MSVC)` + `Sanitizer (UBSan via Clang)` are now **branch-required**,
so a direct `gh api …/merge` can no longer bypass them (GitHub rejects the merge
until each reports green or `skipped`). The residual escape is the **bucket-C/E**
dynamic lanes (screenshot diff + ImGui Test Engine): both carry a blanket
`continue-on-error: true` for **Mesa software-GL** flakiness, so visual/interaction
failures still never block merge. (`Bucket-` was dropped from the merge-poller
([`merge-gates.sh`](../../agents/scripts/core/merge-gates.sh)) *meant-to-block allow-list*
2026-06-15 — `infra.md` `bucket-mesa-exe-boot` P1: the Mesa-software-GL bucket-C/E lanes
can't boot the CI exe, so they are now fully advisory and the poller no longer blocks
on them; re-add on boot-fix graduation.) **Documented escape:** PR
#1180 shipped red bucket-C/E under a `cr-out-of-band` override (postmortem owed).

---

## 4. Subsystem coverage map

| Subsystem | `.cpp` | Coverage character | Hot spot |
|---|---|---|---|
| **Tracker** (strict) | 52 | Heavy **pure-mapping** (Jira/Plane/GitHub mapping, field parsers, JQL) | ✅ logic / ❌ HTTP transport |
| **Ui** | 75 | Pure-logic units + 26 bucket-E (advisory) | dynamic lane non-gating |
| **Commands** (strict) | 51 | Scenario + CLI-probe + palette/fuzzy units | good |
| **Sync / Offline** (strict) | 3 | Queue-replay, conflict-merge, two-backend replay units | good |
| **Persistence** (strict) | 5 | LocalCache + migration units (happy-path) | ❌ corruption/BUSY |
| **Config** (strict) | 5 | Migration + concurrency units | good |
| **Privacy** | 1 | EmailMask / redact units | good |
| **Vcs** | 3 | VcsSubmission + P4Annotate parse/E2E units | good |
| **Diagnostics** | 3 | CrashSink, MemoryTelemetry, PerfSampleRing | good |
| **Imaging** | 1 | ParseImageDimensions, WavWriter | ❌ no fuzz |

---

## 5. Known gaps (ranked, durable)

Each gap is tagged with its backlog status — `[tracked]`, `[partial]` (a
narrower slice is tracked), or `[UNBACKLOGGED]`. The full entry → file → status
mapping is in [§ 5.1](#51-backlog-cross-reference); the tags here are the
one-glance summary.

1. **Visual/interaction lane non-gating.** Bucket-C + bucket-E are blanket
   `continue-on-error` on flaky Mesa GL → 26 UI tests + 3 goldens never block a
   merge. *Highest leverage to fix.* — **[tracked]** infra.md `bucket-lane-launch-smoke` P1.
2. **No HTTP-transport integration tests.** All Tracker coverage is `*Pure`
   mapping. The real `TrackerHttpClient` retry/backoff/429/timeout/pagination/SSL
   paths have **no fault-injection harness** — Fake fixtures stub the *client
   interface*, not the transport. — **[partial]** debt.md scripted-HTTP fixture extension (catalog paths only).
3. **No fuzz / property tests.** Parsers eat untrusted input (tracker JSON, AI
   SSE/NDJSON, p4 annotate, callstacks, markdown/ADF, WAV, image dims) — all
   example-tested only. Direct miss against the "Never crash" pillar.
   (`grep fuzz` hits only `FuzzyMatch`.) — **[partial]** `tests/fuzz/` now covers image-dims, cpp-lex, callstack, markdown/ADF, AI SSE/NDJSON (E2a/E2b) and the tracker-response *consuming* layer — GitHub/Plane/Linear JSON→`CachedTicket` mappers (E2c); MCP-dispatch + config/locale-override + p4-annotate + WAV parsers still unbacklogged (E2d note: `test/2026-07-05-fuzz-surfaces-next-batch.md`).
4. **Test-delta gate ≠ assertion quality.** Gate checks a test file *changed*,
   not that it *exercises* the diff — a no-op assertion passes. No mutation signal.
   — **[partial]** infra.md tracks the *inverse* (false-RED on no-runtime-surface); the false-GREEN / mutation-signal half unbacklogged.
5. **Agent fleet near-untested.** 30+ specialists; **3** agent-eval fixtures, all
   `code-review`. Prompt/routing regressions invisible.
   — **[tracked]** tooling.md subagent-eval + plan `subagent-eval-agentic-coverage.md`.
6. **Persistence corruption untested.** Cache/config open paths are happy-path;
   no truncated-DB, schema-from-future, or `SQLITE_BUSY` storm test.
   — **[UNBACKLOGGED]**.
7. **3 UI test scripts silently skipped in worktree dev** ([`test-all.sh`](../../scripts/dev/test-all.sh)
   `WORKTREE_INCOMPATIBLE_RE`). AGENTS.md mandates one-worktree-per-session → they
   run **only** in CI on a main checkout, never in the default agent loop.
   — **[resolved 2026-06-14]** re-audited (testing-surface roadmap Slice H): the regex
   was trimmed from 6 → 3 alternatives — `test-lint-hook-split`, `test-ui-callstack-tooltip`,
   and `test-ui-ai-assistant` no longer exist as scripts. The 3 survivors
   (`test-ui-views-columns-reorder`, `test-ui-funcsize-window-render-smoke`,
   `test-ui-funcsize-grid-render`) are bucket-E UI drivers needing a built UI binary +
   Mesa display; their skip guards real worktree baseline-drift and is **orthogonal** to
   the #1166 `GIT_EXEC_PATH` cold-configure fix (so still justified — not removed by it).
8. **Perf gate = fast subset.** Regression in a scenario not in the PR-fast set
   escapes to nightly `perf-full`. — **[tracked]** "8 of 15 scenarios don't emit rows[]", 7/15 baselined.

### 5.1 Backlog cross-reference

Where each gap is tracked in the self-improvement backlog
([`docs/self-improvement/categories/`](../self-improvement/categories/)). **7 of
8 gaps are now tracked** (one fully, six as a narrower slice — Gap 6 newly, via
plan [`slice-g-db-corruption.md`](../plans/shipped/slice-g-db-corruption.md)); only
the false-GREEN/mutation half of Gap 4 stays unbacklogged.

| Gap | Tracked as | File | Status |
|---|---|---|---|
| 1 — visual/interaction non-gating | `bucket-lane-launch-smoke` P1 (+ Mesa-GL CI setup, step-level `continue-on-error` postmortem) | [`infra.md`](../self-improvement/categories/infra.md):40, [`tooling.md`](../self-improvement/categories/tooling.md):282, [`infra.md`](../self-improvement/categories/infra.md):121 | open |
| 2 — no HTTP-transport tests | extend scripted-HTTP fixture (`JiraCatalogHttpFixture.h`) to search/mutation/user-meta paths | [`debt.md`](../self-improvement/categories/debt.md):65 | open — **partial**: catalog-path coverage only; no general `FakeHttpTransport` fault injection |
| 3 — no fuzz / property tests | crafted-PNG-dims fuzz against `GoldenImage.h` cap | [`security.md`](../self-improvement/categories/security.md):59-60 | open — **partial**: image-dims/cpp-lex/callstack/ADF/SSE/NDJSON + tracker GitHub/Plane/Linear mappers (E2a-c) covered; MCP-dispatch/config-locale/p4/WAV unbacklogged ([`test/2026-07-05-fuzz-surfaces-next-batch.md`](../self-improvement/categories/test/2026-07-05-fuzz-surfaces-next-batch.md)) |
| 4 — test-delta ≠ assertion quality | auto-PASS classifier for no-runtime-surface diffs | [`infra.md`](../self-improvement/categories/infra.md):60-61 | open — **inverse direction**: tracks false-RED, not the false-GREEN/mutation-signal half (unbacklogged) |
| 5 — agent fleet near-untested | subagent-eval harness graduation + trace flywheel | [`tooling.md`](../self-improvement/categories/tooling.md):505-507 + plan [`subagent-eval-agentic-coverage.md`](../plans/active/subagent-eval-agentic-coverage.md) | open — Phase 2 (flywheel) gated on Phase 0 judge calibration |
| 6 — persistence corruption untested | Slice G — cache-open corruption: Phase 1 characterization (this slice) → Phase 2 graceful-rebuild fix → G2 `SQLITE_BUSY` | plan [`slice-g-db-corruption.md`](../plans/shipped/slice-g-db-corruption.md) | open — **partial**: Phase 1 characterizes `LocalCacheManager` corrupt/truncated/empty-on-open ([`LocalCacheManagerCorruption.test.cpp`](../../tests/Core/LocalCacheManagerCorruption.test.cpp), in review); Phase 2 fix + the config open path + `SQLITE_BUSY` (G2) still open |
| 7 — worktree-skip (6→3) | skip mechanism (`WORKTREE_INCOMPATIBLE_RE`) + the `GIT_EXEC_PATH` build fix | [`applied.md`](../self-improvement/categories/applied.md):342, [`tooling.md`](../self-improvement/categories/tooling.md):196, [`infra.md`](../self-improvement/categories/infra.md):229, [`test.md`](../self-improvement/categories/test.md):63 | **resolved 2026-06-14** (Slice H): regex trimmed 6→3 (3 dead scripts removed); 3 survivors justified + orthogonal to #1166 |
| 8 — perf gate = fast subset | "8 of 15 candidate perf scenarios don't emit `rows[]`" | [`applied.md`](../self-improvement/categories/applied.md):265 (7/15 baselined) + [`tooling.md`](../self-improvement/categories/tooling.md) follow-up | open — 8 scenarios need a `rows[]` retrofit before they can gate |

**One unbacklogged item worth filing** (no current entry; candidate for a new
backlog row or GitHub Issue): the **mutation-signal half of Gap 4** — a
mutation-smoke harness (flip-and-rerun) to expose no-op assertions that the
test-delta gate waves through GREEN. (**Gap 6** is now tracked by Slice G — plan
[`slice-g-db-corruption.md`](../plans/shipped/slice-g-db-corruption.md): Phase 1
characterizes the cache open path, Phase 2 adds the fix, G2 covers `SQLITE_BUSY`.)

---

## 6. Improvement roadmap

Each item is flagged **[planned]** (a backlog entry already owns it — execute,
don't re-scope) or **[new]** (no entry — file before/with the work). Cross-ref
[§ 5.1](#51-backlog-cross-reference).

**P0 — close the gating holes**
- Make **bucket-E required**: split stable scenarios from Mesa-flaky ones, quarantine
  the latter by name, drop blanket `continue-on-error` on the stable lane. Same for
  bucket-C on the 3 goldens. **[planned]** — infra.md `bucket-lane-launch-smoke` P1.
- Add **Coverage + Sanitizer** to `required_contexts` via the always-report pattern
  ([`ci-required-check-pattern.md`](../agent-rules/ci-required-check-pattern.md) Pattern A)
  so a direct-REST merge can't bypass. **[new]** — the doc-validation context was made
  required (infra.md:87, applied); Coverage + Sanitizer were not — no entry yet.

**P1 — fill the dangerous voids**
- **Fault-injection HTTP fixture** (`FakeHttpTransport` injecting 429/500/timeout/
  truncated-body/partial-page) → integration tests for `TrackerHttpClient` retry +
  pagination. New `tests/support/FakeHttpTransport.h`. **[planned, narrower]** —
  debt.md:65 tracks extending the catalog-path fixture; the general transport-fault
  superset is new.
- **libFuzzer harness** under `ninja-clang-asan`: parser targets (AiSseParser,
  AiNdjsonParser, CppSyntaxLex, CallstackParser, MarkdownConvertAdf,
  ParseImageDimensions), seed corpus in `tests/fixtures/`, short in PR + long nightly.
  **[planned, narrower]** — security.md:59-60 tracks the PNG-dims target only.

**P2 — quality of the tests themselves**
- **Mutation smoke** on the hottest pure files (flip-and-rerun) to expose no-op
  assertions; or upgrade the test-delta gate to require a *coverage* delta on
  changed lines, not just a touched file. **[new]** — the false-GREEN/mutation half
  of Gap 4 has no entry.
- **DB-corruption corpus** test for cache/config open paths. **[in progress]** —
  Gap 6, now Slice G (plan [`slice-g-db-corruption.md`](../plans/shipped/slice-g-db-corruption.md)):
  Phase 1 characterizes the `LocalCacheManager` open path (this PR); Phase 2 adds the
  graceful-rebuild fix, the config open path + `SQLITE_BUSY` (G2) still to do.
- ~~Root-cause the **worktree-skip 6** so the default dev loop runs them.~~ **[done 2026-06-14,
  Slice H]** — re-audited: regex trimmed 6→3 (3 scripts deleted); 3 survivors are
  bucket-E UI drivers, their skip is justified + orthogonal to #1166 (test.md:63, infra.md:229).

**P3 — fleet + perf**
- Grow **agent-eval** beyond code-review (orchestrator routing, merge-gate decision,
  debug repro-loop fixtures). **[planned]** — tooling.md:505-507 + plan
  [`subagent-eval-agentic-coverage.md`](../plans/active/subagent-eval-agentic-coverage.md).
- Widen the perf-fast subset, or add a "touched-scenario-not-in-fast-set ⇒ run it"
  rule to the [`perf-gatekeeper`](../../agents/core/perf-gatekeeper.md) diff→scenario map.
  **[planned, narrower]** — applied.md:265 + the "8 of 15 scenarios" retrofit follow-up.

---

## 7. Refresh recipe

Re-run to update § 1 counts without re-deriving the analysis:

```bash
cd "$(git rev-parse --show-toplevel)"
printf "core_units=%s\n"   "$(ls tests/Core/*.test.cpp        | wc -l)"
printf "lua=%s\n"          "$(ls tests/Lua/*.test.cpp          | wc -l)"
printf "mcp=%s\n"          "$(ls tests/Plugins/Mcp/*.test.cpp  | wc -l)"
printf "bucket_e=%s\n"     "$(ls tests/ui/*.test.cpp           | wc -l)"
printf "bucket_a=%s\n"     "$(ls scripts/dev/test-*.sh         | wc -l)"
printf "bats=%s\n"         "$(ls tests/bats/*.bats             | wc -l)"
printf "goldens=%s\n"      "$(ls tests/golden/*.png            | wc -l)"
printf "agent_eval=%s\n"   "$(find tests/agent-eval -name '*.json' | wc -l)"
printf "src_cpp=%s\n"      "$(find Source/Core/src -name '*.cpp' | wc -l)"
```

Re-confirm the gating map (§ 3) when CI changes:

```bash
# required contexts
grep -A8 required_contexts project.config.json
# which jobs are advisory (continue-on-error)
grep -rn 'continue-on-error' .github/workflows/build-and-test.yml
# coverage threshold + blocking flag
grep -n 'threshold\|continue-on-error' .github/workflows/coverage.yml
```

If the *shape* changed (a lane flipped from advisory to required, a new bucket
added, a void closed), update §§ 3, 5, 6 here in the same PR — this doc is the
single source of truth for "is the testing surface covered?" and a stale answer
costs a full re-audit.
