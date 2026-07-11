# Smatchet — QA Director Evaluation (WITH agents.md)

> Lens: test strategy, coverage, gates, defect-prevention, release validation, confidence.
> Pass constraint: this evaluation deliberately reads and factors the agentic-governance
> meta-layer (root + per-subsystem `AGENTS.md`, `docs/agent-rules/**`, `AI_POLICY.md`,
> `docs/self-improvement/**`, `docs/agent-eval/**`, the test-related agents in `agents/core/`,
> the `agents/scripts/**` gate self-tests, `.coderabbit.yaml`, and all CI workflows), because
> in this project the *quality philosophy* lives in that meta-layer, not just in `tests/**`.
> Evaluator date: 2026-06-30.

---

## 1. Executive Summary + Verdict

Smatchet is, by a wide margin, the most quality-engineered solo/agent-driven codebase I have
assessed. It pairs a respectable tangible test estate (307 C++ test translation units, ~9,492
doctest assertions in the Core unit rig alone, 6 libFuzzer targets with seeded corpora, 56 Bats
gate-contract suites, 44 headless ImGui-Test-Engine UI tests, golden-image pixel diffs, ASan +
UBSan as **per-PR required** checks, nightly TSan) with something genuinely novel: **quality codified
as governance**. The project treats every defect that reaches `develop` as a *gate failure*, not a
person failure, and the institutional response to an escape is a **new permanent gate**, recorded in a
blameless, append-only postmortem ledger (51 entries) whose `### Preventing gate` field is *mandatory
and cannot be omitted*. This is a self-tightening ratchet, and — critically — it is real, not theater:
the ledger reads like an honest incident database, the gates it spawned exist on disk, and a meta-suite
of **85 `test-*.sh` gate self-tests** exists specifically to stop the gates themselves from rotting.

**Verdict: this is a genuinely advanced quality system — top-decile defect-prevention discipline — with
a small number of real, honestly-documented soft spots.** The headline strengths: a single-source
"meant-to-block" allow-list that closes the non-required-red-check escape class (#923); fail-closed
defaults everywhere (freshness guards, base-ref resolution, pagination ceilings); a lossless merge-time
snapshot ledger (115 entries) that defeats GitHub's post-merge state overwrite; an agent-eval harness
that calibrates the LLM judge against human labels with a *blocking* divergence gate; and the
discipline that "the green assertion is an exit code, never advisory text." The honest soft spots: a
blunt global `SKIP_MERGE_GATES=true` bypass; a coverage floor parked at 65% (target 70%) with a
fail-*open* per-file gate; the Pillar-1 absolute 6.94 ms perf budget is **documented but not yet
encoded** (only relative-regression detection is armed); data-race (TSan) coverage is nightly-advisory,
not per-PR; and a recurring class of UI/threading changes ships under `tests-out-of-band` because the
headless rig structurally cannot host them — a gap the project itself has diagnosed and is chasing.

There is some process weight (51 postmortems, 50 backlog category files, a 1,519-line gate poller). For
a solo+agent project this is unusually heavy, but it is *load-bearing* weight: nearly every rule traces
to a cited incident ID. This is reactive-hardened engineering, not top-down ceremony.

**Overall score: 8.4 / 10.**

---

## 2. Scope & Method

I read the governance meta-layer directly (`docs/agent-rules/quality-pillars.md`,
`merge-gates.md`, the full `postmortems.md`, `AI_POLICY.md`, the coverage workflows, the agent-eval
calibration policies) and delegated breadth sweeps to parallel sub-agents covering: (a) the gate scripts
and all 27 CI workflows; (b) the test tree, mocking infrastructure, sanitizers, and perf budget; (c) the
self-improvement category backlog and the merge-snapshot ledger. Findings were cross-checked against the
primary sources — e.g. I independently confirmed the 10 branch-protection `required_contexts`, the 65%
coverage threshold (`coverage.yml:64,176`), the 51-entry postmortem count, the 85 gate self-tests, and
the 9,492 Core assertions. Where the two gate-analysis sub-agents diverged (per-PR Sanitizer required vs
nightly-only), I reconciled against `project.config.json`: **per-PR ASan (MSVC) and UBSan (Clang) ARE
required contexts**; the nightly sanitizer sweep and TSan are separate, advisory lanes.

Counts cited below are from direct `find`/`grep`/`wc` over the working tree.

---

## 3. Test Inventory & Pyramid (counts)

| Surface | Count | Framework / mechanism | Tier |
|---|---|---|---|
| `tests/Core` | **222 .cpp** (~9,492 `CHECK`/`REQUIRE`/`SUBCASE`) | doctest, pure C++14 logic | Unit (base of pyramid) |
| `tests/Lua` | 8 | doctest (`lua_main.cpp`) | Unit — bindings/sandbox/timeout |
| `tests/Plugins` | 7 | doctest (5 MCP + Whisper + LuaConsole) | Unit — plugin envelopes/dispatch |
| `tests/ui` | 44 | ImGui Test Engine, headless software-GL (Mesa) | Component/integration (bucket E) |
| `tests/golden` | 3 PNG + README | screenshot / pixel-diff | Visual regression |
| `tests/fuzz` | 6 targets + corpus (3 seeds × 6) | libFuzzer (`-fsanitize=fuzzer,address,undefined`) | Security/robustness |
| `tests/bats` | 56 | Bats — shell/gate-contract suites | Gate/meta + infra |
| `tests/live` | 1 | doctest (`linear_live_smoke.cpp`), credential-gated nightly | E2E (live backend) |
| `tests/fixtures` | 90 files | JSON backend fixtures + CI-parity + merge-gate fixtures | Test data |
| `tests/support` | ~30 headers | `Fake*`/`Stub*`/`Scripted*` fixtures | Mock infra |

**Total: 307 test `.cpp` TUs + 56 Bats suites.** (The brief's "~307 test files" maps to the `.cpp`
count; the full `tests/` tree is ~503 files including fixtures, corpora, and headers.)

**Pyramid shape:** broad and correctly weighted. The base is a large, fast, headless doctest unit rig
(222 TUs / ~9.5k assertions) — exactly where a QA director wants the mass. Above it sit 44 headless UI
component tests (ImGui Test Engine, deterministic multi-frame ticks, per-test `Reset*State()`
isolation), then a thin layer of visual goldens and a single live E2E smoke. Fuzzing and Bats sit
*orthogonal* to the pyramid as robustness and meta-gate layers. This is a healthy distribution — heavy
unit base, no inverted ice-cream-cone.

**Backend testing without live services — a standout.** Live trackers (Jira / Linear / GitHub / Plane)
and p4 are mocked **without HTTP, cpr, or PATs**: `FakeTrackerClient` is the base, fed by JSON fixtures
through pure mapping helpers (`FakeGitHubFixture::MapGraphQlNodesToTickets`, `JiraFakeTrackerFixture`,
`FakePlaneFixture`, `FakeP4Runner`, `StubAiClient`, `AiHttpFixture`, `SqliteMemFixture`).
`ScriptedTrackerBackendFactory` injects fixture-configured backends via `AppController::SetBackendFactory`
*before* `Initialize`, owning the fixture by value (no lifetime hazard). The result: nearly the entire
backend surface is exercised deterministically and offline, with exactly one network-touching test
(`linear_live_smoke`) quarantined to a credential-gated nightly lane so fork PRs never see the secret and
live-API flakiness never reds a contributor's PR.

**Fuzzing targets the right surfaces** — all six fuzz the *untrusted-byte* parsers: AI NDJSON (Ollama),
AI SSE (OpenAI/Anthropic streams), pasted callstack text, C++/callstack lexers, image header dimensions
(PNG/GIF/WEBP/JPEG over attachment bytes), and Markdown→ADF via md4c. Corpus is checked in (3 seeds each).

---

## 4. Quality Gates — Strength Analysis (blocking vs escape-hatches)

### 4.1 The "gate, don't trust" model

Before any squash-merge, one of three sanctioned actors (orchestrator, `git-janitor`, or the
`smatchet-merge-watcher` host daemon) polls a single `gh api graphql` query and evaluates **four gate
families simultaneously** (a single AND-conjunction, no short-circuit) via `merge-gates.sh` (1,519 lines):
(1) **CI** — every branch-protection *required* check terminal-green, **plus** any non-required check on
the meant-to-block allow-list; (2) **CodeRabbit** — latest on-head review not blocking, zero unresolved
CR threads; (3) **User comments** — zero unresolved non-bot/non-self threads; (4) **Cursor Bugbot** — zero
unresolved inline findings. Plus PR-state OPEN, `reviewDecision in {APPROVED, null}`, a `mergeStateStatus`
guard, and a hard `PAGINATION_OVERFLOW` block on any `hasNextPage`.

The system is **fail-closed by construction**: parse misses default to `-1` (fails integer pass-checks),
unverifiable states block, and an invalid `MERGE_GATES_FRESHNESS` value returns 3 (never passes).

### 4.2 The genuinely-blocking set

`project.config.json` `branch_protection.required_contexts` enforces **10 GitHub-required checks**:
`Test-delta gate`, `Windows + MSVC`, `Windows + MSVC (light)`, `Comment-noise + high-integrity gate`,
`Shell lint`, `Doc anchors + agent contract`, `Perf PR-fast`, `Coverage (OpenCppCoverage)`,
`Sanitizer (ASAN via MSVC)`, `Sanitizer (UBSan via Clang)`. On top of those, the **meant-to-block
allow-list** (`MERGE_GATES_BLOCK_ALLOWLIST_RE`, the single source of truth, sourced by
`safe-admin-merge.sh` and `postmortem-owed.sh`):

```
Coverage|Sanitizer|Perf PR-fast|Android security gate|Fuzz smoke|Bucket launch-smoke (Mesa GL)|Intent section|Plan-lock gate
```

This means a *non-required* RED check (e.g. the deterministic half of `Fuzz smoke`) still blocks the
poller. This regex exists *because of* the #923 escape (the watcher auto-merged past a red non-required
`Coverage`). That is the ratchet in action: an escape became a permanent structural gate.

**Two coverage gates, distinct:**
- **Test-delta gate** (`coverage-gate.yml` → `coverage-delta-gate.sh`) — *structural*, **hard-blocking
  from day 1**: "any PR that touches `Source/Core/src/*.cpp` without a paired test delta is rejected."
  It enforces "if you change behaviour, add a test," and carries a conservative auto-exempt "test-light"
  classifier (comments / logging-only / `static_assert` / fwd-decls / includes / preprocessor guards /
  catch-scaffold). That classifier was itself hardened against escapes (#918: `/* note */ launchTask();`
  and `*out = compute();` were wrongly exempted).
- **Numeric coverage** (`coverage.yml` → `coverage.sh`) — `--threshold 65`, `continue-on-error: false`
  (blocking). **WARN-first graduation is explicit**: it ran advisory during a soak, then graduated to
  blocking on 2026-06-04 at a *data-chosen floor* of 65 (first real measurement was 67%) with a
  raise-to-70 ramp tracked in `categories/test.md`. Plus a **per-file ≥90% floor** for four named
  high-risk units (SSRF sanitizer, credential redaction, JQL escaper, tracker HTTP classifier).

**Sanitizers** — per-PR ASan (MSVC) and UBSan (Clang) are **required contexts** (real per-PR memory-safety
gating), wired via `cmake/Sanitizers.cmake` across `ninja-msvc-asan`, `ninja-clang-asan`, and the bucket-E
`ninja-ui-test-asan-{clang,msvc}` presets. The *nightly* ASan+UBSan sweep and the **TSan** subset
(`ninja-tsan-linux`, ImGui-free `SmatchetTsanTests`) are separate advisory lanes — TSan has no Windows
toolchain, so data-race detection is nightly-only, not per-PR.

### 4.3 Escape hatches — scrutinized honestly

Gates **are** bypassable. The discipline is that every bypass is **named, logged to stderr, mostly
label-gated (visible in the PR sidebar), several attestation-required, and recorded in the snapshot
ledger.** Inventory:

| Mechanism | Scope | Strength assessment |
|---|---|---|
| **`SKIP_MERGE_GATES=true`** | **Global — bypasses ALL gates** | **The blunt instrument.** Session-init only ("no per-merge skip"), does NOT auto-inherit through the subagent boundary, emits `GATES_SKIPPED`. Gated only by caller discipline. The one real weak link. |
| `tests-out-of-band` | Test-delta FAIL→WARN | Named, logged, snapshotted; recurringly *legitimate* (behaviour-preserving refactors with no headless test home) |
| `coverage-out-of-band` | Numeric coverage FAIL→WARN | Named; a *prose-only* version once existed nowhere in code (the #941 "prose-promise gate" escape) — now wired and self-tested by `test-oob-label-impl.sh` |
| `perf-out-of-band` | Perf regression FAIL→WARN | Named; **does NOT downgrade run/plumbing failures**, only regressions (#1566 hardening) |
| `intent-out-of-band`, `plan-lock-out-of-band`, `bugbot-out-of-band` | Named single-gate downgrades | Logged, sidebar-visible |
| `cr-out-of-band` | CodeRabbit block→WARN | **Strengthened (PR-3): NOT honoured alone — REQUIRES a paired `cr-disposition:<reason>` attestation.** CI + user-comment gates still bind. |
| self-improvement-doc auto-exempt | Docs-only PRs skip CR + Bugbot | **No label**, detection-based, fail-safe to FALSE; CI + user-comment gates still bind |
| `gh pr merge --admin` | Bypasses branch protection | Wrapped by `safe-admin-merge.sh`, which re-runs the same allow-list first |

**Anti-escape hardening of the hatches themselves** is the impressive part:
- **`issue-sweep.sh` strips lingering `*-out-of-band` labels off merged PRs** so an override can't survive
  to skew the ledger; the "never pre-apply" hygiene rule is documented and enforced.
- **`MERGE_GATES_FRESHNESS=block`** (the watcher sets this) refuses `GATES_PASSED` when the running
  `merge-gates.sh` blob differs from `origin/develop` — closing #1428, where a daemon parked on a stale
  branch enforced two-day-old allow-list logic. Fail-closed if unverifiable.
- **`safe-merge.sh` is the only sanctioned non-admin merge path**; a bare `gh pr merge --auto` or direct
  REST `PUT` is *forbidden in the ship-loop* because GitHub auto-merge only waits on *required* contexts,
  so it under-fires (merges past a CR finding) and over-fires (wedges forever) — both documented from real
  escapes (#1406/#1414/#1415, #1332).

**Grandfathering:** the ~269 "grandfather" grep hits are almost entirely *high-integrity ratchet
baselines* (`dup-baseline.md`, `function_size_audit`, etc.) — they allow existing debt and block *new*
debt. That is the correct use of grandfathering: a ratchet, not an amnesty. The DRY/duplication gate is
explicitly WARN-first with a documented graduation trigger (FP rate < 10% over ~20 PRs).

**Net gate-integrity judgment:** very strong. The escape surface is wide but disciplined — the only
mechanism that troubles me is the unconditional `SKIP_MERGE_GATES`, which has no per-merge audit beyond a
`LOG_WARN`. Everything else is named, attested, logged, snapshotted, and self-tested.

---

## 5. Quality-as-Governance — the Novel Lens

This is where Smatchet departs from a conventional QA shop, and where it earns its high score.

### 5.1 The Quality Pillars (5 north-star invariants)

`docs/agent-rules/quality-pillars.md` codifies five invariants in two groups. **UX Pillars 1–3 are
enforceable (auto-fail PRs):** (1) Performance — sustained 144 Hz, frame budget 6.94 ms, 100 Hz floor
(>10 ms outliers spike-tracked at p99); (2) UI never freezes — any op > 100 ms moves to a worker, sync
I/O on the UI thread = code-review CRITICAL, p99 < 100 ms enforced; (3) Never crash — pre-merge sanitizer
build mandatory, RAII enforced, bounds-checked via `at()`, UBSan output during the gate = fail.
**Pillar 4 (accessibility) is explicitly aspirational** — honestly flagged as having "no auto-fail gates
today," with a stated reason (no automated WCAG/keyboard-reachability check exists) rather than a hidden
gap. **Pillar 5 (DRY, the Engineering Pillar) is WARN-first/calibration-phase** with a documented
graduation threshold. This three-tier honesty — *enforceable / WARN-first / aspirational* — is exactly
how a mature QA org should communicate the maturity of each control. It refuses to pretend a control
exists when it doesn't.

### 5.2 Gate-escape postmortems as a self-tightening ratchet — the centerpiece

`docs/self-improvement/postmortems.md` is an **append-only ledger of 51 gate escapes**, each structured as
*What escaped / Root cause (blameless RCA) / Preventing gate / Filed as*. The `### Preventing gate` field
is **mandatory — an entry cannot close without it** — and is either a concrete new gate/lint/test or the
explicit literal `none — override legitimate` (only **7 of 51**, each with a reasoned justification, e.g.
a byte-identical header→cpp body relocation with no testable surface). This is the ratchet's defining
property: **every escaped defect becomes either a new permanent gate or a documented-legitimate override.**

Reading the ledger, these are unmistakably **real defects and real gate holes**, not ceremony:
- **#923** — watcher merged past a red non-required `Coverage` → spawned the meant-to-block allow-list (the
  single most load-bearing gate in the system).
- **#357** — `COMMENTED == pass` shipped 5 unaddressed CR findings → CR gate rewritten to block on
  `Actionable comments posted: N > 0`.
- **#1428** — a daemon on a stale checkout enforced 2-day-old gate logic → fail-closed freshness self-guard
  with 5 Bats cases.
- **merge-watcher daemon crash** — an unhandled `subprocess.TimeoutExpired` (⊄ `RuntimeError`) crashed the
  whole daemon and stranded every registered PR → five gates incl. per-PR and per-cycle backstops + 7 Bats
  regression tests; notably, a *cycle-scope* sibling hole was caught by an **adversarial verification pass
  on the diff before merge** — the "gate, don't trust" pattern catching a second-order bug in the fix for
  the first.
- **#1390/#1409** — behaviour-changing **concurrency-correctness** fixes shipped under `tests-out-of-band`
  because the headless single-threaded doctest rig structurally cannot assert a threading invariant → the
  preventing gate is a *static strict-zone lint* (forbid off-UI-thread `g_ui` request-flag writes) + a
  runtime TSan/native leg, precisely because the class has "no in-rig test home."

The companion machinery makes the loop operational, not aspirational: `postmortem-owed.sh` is a
SessionStart nudge that detects owed postmortems from the lossless `merge-snapshots.jsonl` ledger (115
entries: `pr / mergeCommit / headSha / gates / redChecks / overrideLabels / mergeActor`), which exists
specifically because GitHub overwrites rollup contexts and strips labels post-merge (ADR-0017). The
escape→postmortem→`### Preventing gate`→`categories/<cat>/<date>-<slug>.md` backlog→`applied`→agent-prompt
edit pipeline is a closed loop with an explicit apply threshold ("mentioned by ≥2 agents OR blocks the
same workflow ≥3 times"). I traced several test-category entries from postmortem to *shipped* fix (e.g.
the #1212 tracker-redirect security property → regression test shipped in #1517; the bucket-E
`ReadOnlyMode` vacuous-green fix shipped in #1534). **The ratchet demonstrably turns.**

### 5.3 The agent-eval harness — evaluating the test-authoring agents themselves

`docs/agent-eval/` is an eval harness for the *development* agents (the LLMs that write tests and reviews),
not the product. Three stages: **run** (reconstruct an agent run from a frozen `delegationPacket`), **score**
(each case declares `judge`-kind dimensions graded by an external LLM judge and `objective`-kind dimensions
checked deterministically — `cited_file_line`, `severity_enum`, `finding_count`), and **calibrate**. The
scorer is **advisory (WARN-only)** in the MVP — sensibly, because "blocking on a score requires trusting
the judge." The one piece that **blocks** is `agent-eval-calibrate.py`: it runs the judge over human-labelled
cases and hard-fails (exit 1) when judge–human divergence exceeds bounds (mean |Δ| ≤ 0.15 AND no single
pair > 0.25). The policy file is admirably self-aware: *"a judge that disagrees with humans beyond
tolerance is not trustworthy enough to gate prompt-quality regressions… loosening the bound is a trust
decision."* That single design choice — *block on judge calibration, only WARN on judge scores* — is the
most sophisticated "who validates the validators" answer I have seen in an agent-driven codebase.

---

## 6. Who-validates-the-validators (AI writes + reviews tests — blind spots)

The structural risk is real and the project has *partially*, but not fully, neutralized it:

**The blind-spot risk.** `test-author` writes tests; `code-review` (and CodeRabbit + Bugbot) review them.
Both are LLM agents. A shared model blind spot — a misunderstood invariant, a wrong-but-plausible golden
value — could be written by one agent and waved through by another without a human ever seeing the
divergence. This is the canonical AI-writes-and-reviews failure mode.

**What mitigates it here (genuinely):**
1. **The human is the terminal authority on the highest-risk artefacts.** `AI_POLICY.md` makes autonomy "a
   granted, revocable mode, not a default right," and the **golden-image-approval** contract *forbids* an
   agent from `git add`-ing a reference PNG until the user inspects it and says "looks right." The
   motivating incident (2026-05-19: a golden bootstrapped while a residual-color bug was live, which "would
   have certified the bug as expected behaviour forever") is exactly the AI-enshrines-a-bug failure mode —
   and the response was a human-in-the-loop gate plus a *preferred* "dual-capture-no-golden" pattern where
   "the failure mode is structurally impossible."
2. **`test-rig`'s hard invariant: "Capture expected values by RUNNING — never transcribe a golden from
   source."** This directly attacks the "an LLM hallucinated the expected value from the implementation"
   class.
3. **Judge calibration against human labels blocks** (§5.3) — the validators of the validators are
   themselves validated against ground truth, and that gate is hard.
4. **The 85 gate self-tests assert failure paths.** `test-gate-selftests.sh` ("the gate that gates the
   gates") requires every `--selftest` to carry a `# selftest: asserts-failure` marker proving it exercises
   a *real* failure, killing the "selftest only walks the happy path" class. `test-oob-label-impl.sh`
   asserts every documented escape-hatch label is actually *read* by code (killing prose-only gates).

**Where the blind spot remains open:**
- **Calibration breadth is thin.** The calibration set is **one agent (`code-review`) with one labelled
  smoke case** plus 3 frozen golden cases. `test-author` itself — the agent writing the tests — has **no
  eval cases yet**. So the harness that would catch a systematically-bad test author is built but
  essentially unpopulated. This is the single biggest "who validates the validators" gap.
- **Concurrency invariants have no automated validator at all** (§5.2, #1390/#1409): the headless rig
  can't host them, TSan is nightly-advisory, so the *human* (or a future static lint) is the only check.
- The escape hatches are LLM-applicable: an agent can apply `tests-out-of-band` or, in principle,
  `SKIP_MERGE_GATES`. The mitigation is the snapshot ledger + `postmortem-owed` nudge — *detective*, not
  *preventive*. The loop catches the escape after the fact and converts it to a gate; it does not always
  prevent the first instance.

Net: the project has thought harder about this problem than almost anyone, and has the right *architecture*
(human-terminal on visual/golden artefacts, blocking judge calibration, failure-asserting self-tests). It
has not yet *populated* that architecture broadly enough to claim the blind spot is closed.

---

## 7. Gaps & Risks

1. **`SKIP_MERGE_GATES=true` is an unconditional global bypass** with only a `LOG_WARN` audit. It is the one
   gate hole with no structural backstop beyond caller discipline. (Risk: medium; partially mitigated by
   subagent non-inheritance and the snapshot ledger recording the skipped merge.)
2. **The Pillar-1 absolute perf budget (6.94 ms) is documented but not encoded.** `regression-policy.json`'s
   `mean_abs_ceiling_ms` ships `null/DISABLED` pending a calibration pass; only *relative* regression
   detection (noise floor 0.05 ms) is armed, and the scenario measures UI-thread CPU under headless Mesa, not
   real framerate. The 144 Hz invariant is currently a *guarded direction of travel*, not an enforced ceiling.
3. **Coverage floor parked at 65% (target 70%)**, and the per-file ≥90% gate **fails-open** on missing XML —
   a missing coverage artefact passes rather than blocks. The aggregate ramp to 70 is tracked but open.
4. **Data-race (TSan) coverage is nightly-advisory, not per-PR** (no Windows TSan toolchain). Up to a ~24h
   window for a race to sit on `develop`. Memory safety (ASan/UBSan) *is* per-PR-required, so this gap is
   specifically races, which is the project's hardest-to-test class (§5.2).
5. **Mesa-GL bucket-C/E rendering lanes are advisory (flake-driven).** Only the narrow `Bucket launch-smoke`
   boot check blocks. A real class of UI/threading changes (mobile touch editors, view-switcher) ships under
   manual-only or no deterministic coverage because the CI lane "can't boot the CI exe" — the project's own
   most-cited test-backlog root cause.
6. **`agentic-selftests` (the 85-suite meta-gate) is not yet a branch-protection required context** — it
   relies on the poller's "never merge past ANY red check" rule rather than GitHub enforcement, so a
   non-poller merge path could in principle skip it.
7. **Agent-eval calibration is under-populated** (§6): `test-author` has no eval cases; `code-review` has one
   labelled case.

---

## 8. Scorecard

| Dimension | Score | Rationale |
|---|---:|---|
| **Test breadth** | 9 / 10 | 307 TUs across unit/UI/fuzz/golden/Bats/Lua/plugins/live; offline mocks for every backend; the only thin spot is real-device mobile + visual rendering (CI-lane-blocked). |
| **Test depth** | 8 / 10 | ~9,492 Core assertions; reproducer-first debug contract; "capture by running" anti-transcription rule. Headless rig can't reach threading/visual invariants — a structural depth ceiling the project acknowledges. |
| **Memory/concurrency safety** | 7 / 10 | ASan + UBSan **per-PR required** (strong); RAII/bounds enforced via code-review + cppcheck. But TSan is **nightly-advisory only**, and concurrency-correctness changes recurrently lack any automated home. |
| **Fuzzing / security testing** | 8 / 10 | 6 libFuzzer targets on the right untrusted-byte surfaces with seeded corpora; deterministic build-half is merge-blocking (#1301 fix) while stochastic crashes stay advisory; a 79KB SECURITY_AUDIT.md and per-file ≥90% coverage on security units. Corpora are small (3 seeds each). |
| **Perf-regression testing** | 6 / 10 | Strong *relative*-regression harness (per-scenario baselines, 2-sample confirmation, dedicated agents); `Perf PR-fast` is required. But the **absolute 6.94 ms budget is not yet encoded** (parked null), and measurement is headless-CPU, not real-frame. |
| **Coverage enforcement** | 8 / 10 | Two-gate model: structural Test-delta **hard-blocking day 1** + numeric **65% blocking** with documented WARN-first graduation + per-file 90% floors. Floor below target; per-file gate fails-open. |
| **Release validation** | 8 / 10 | Installer/launch smoke, Mesa-GL boot smoke (blocking), mobile-emulator + mobile-security lanes, credential-gated live smoke, nightly sanitizer/TSan/fuzz/perf-full + auto-issue-filing. Some validation is nightly (post-merge), not pre-merge. |
| **Quality-governance maturity** | 10 / 10 | Quality Pillars; 51-entry blameless escape ledger with a mandatory preventing-gate; lossless snapshot ledger; self-tightening ratchet that demonstrably turns; agent-eval with blocking judge calibration; tiered enforcement honesty (enforceable/WARN-first/aspirational). Best-in-class. |
| **Gate-integrity / anti-escape** | 9 / 10 | Single-source allow-list (#923-derived); fail-closed everywhere; freshness guard (#1428); `safe-merge.sh` as the only sanctioned path; OOB labels stripped post-merge; 85 self-tests that force failure-path assertions and wired hatches. Only the blunt `SKIP_MERGE_GATES` keeps this off 10. |
| **Overall** | **8.4 / 10** | An advanced, self-tightening quality system with genuine defect-prevention — held just short of elite by a parked perf budget, nightly-only race detection, a 65% coverage floor, an unconditional skip, and an under-populated validator-of-validators harness. |

---

## 9. Prioritized Recommendations

1. **Encode the absolute 6.94 ms / 10 ms Pillar-1 budget** (set `mean_abs_ceiling_ms` and the p99 ceiling in
   `regression-policy.json`, complete the perf-gate-revival calibration). Today the headline invariant is
   aspirational in enforcement terms; a relative-only gate cannot catch a slow-but-stable baseline drift.
2. **Land the concurrency-correctness gate** the #1390/#1409 postmortem already specifies: the static
   strict-zone lint (forbid off-UI-thread `g_ui` request-flag writes) plus promoting the TSan subset to a
   *required* per-PR lane on threading-relevant diffs. This closes the single most-cited "no test home"
   escape class and removes the recurring `tests-out-of-band` waivers for behaviour-changing threading fixes.
3. **Populate the agent-eval harness for `test-author`** (and add 5–10 more `code-review` calibration cases).
   The validator-of-validators architecture is excellent but nearly empty; the agent that *writes* tests has
   no eval coverage, which is the largest open blind spot.
4. **Constrain `SKIP_MERGE_GATES`.** Either require a paired reason attestation (mirror the
   `cr-out-of-band` + `cr-disposition` pattern) recorded in the snapshot ledger, or gate it behind an
   interactive confirmation, so the one unconditional bypass leaves the same audit trail every other escape
   does.
5. **Close the coverage floor gap:** execute the 65→70 ramp on a schedule, and change the per-file ≥90% gate
   from **fail-open to fail-closed on missing XML** (a missing coverage artefact on a security-critical unit
   should block, not pass).
6. **Promote `agentic-selftests` to a branch-protection required context** so the 85-suite meta-gate is
   GitHub-enforced, not merely poller-enforced — it currently protects every other gate but is itself not
   structurally required.
7. **Unblock the Mesa-GL bucket-C/E rendering lanes** (or stand up a hardware-accelerated runner). This single
   infra fix would retire the largest cluster of open `[test]` backlog items (mobile touch editors, view
   switcher, hover/tooltip coverage) currently stuck on manual-only validation.
8. **Grow the fuzz corpora** beyond 3 seeds per target and consider a continuous (OSS-Fuzz-style) fuzzing
   budget for the AI-stream and image-header parsers, which sit directly on untrusted network/attachment bytes.

---

*Prepared as a QA Director assessment factoring the agentic-governance meta-layer. All counts verified by
direct inspection of the working tree at `/home/user/Smatchet` on 2026-06-30.*
