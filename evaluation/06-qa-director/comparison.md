# QA Director (Expert 06) — Two-Pass Comparison & Critic Report

**Subject:** Smatchet QA evaluation, single expert, two lenses
**Pass A (without-agents):** tangible test/CI artifacts only — **8.0/10**
**Pass B (with-agents):** same estate PLUS the quality-as-governance meta-layer — **8.4/10**
**Meta-analyst date:** 2026-06-30

---

## 1. Executive Summary — how the meta-layer changed the assessment

The single biggest shift between the two passes is a **reframing of what "QA" even is**. Pass A
evaluates Smatchet as a *static test estate*: 307 C++ test TUs, 2,199 `TEST_CASE`s, doctest/CTest,
an ImGui Test Engine UI tier, 6 libFuzzer drivers, golden images, and ten branch-protection gates.
It asks "how good is the coverage, and which gates block?" and answers, fairly, "advanced."

Pass B keeps that estate but adds a lens Pass A was forbidden to use: **quality codified as
governance**. Under this lens Smatchet is not a fixed set of tests but a *self-tightening process*.
The pivotal artifact is `docs/self-improvement/postmortems.md` — a blameless, append-only ledger of
51 gate escapes in which the `### Preventing gate` field is **mandatory and cannot be omitted**.
Every defect that reaches `develop` is treated as a *gate failure, not a person failure*, and the
institutional response is a **new permanent gate**. QA stops being a noun (a test suite you measure)
and becomes a verb (a ratchet that tightens every time something escapes). That is the central change:
Pass B sees a system where every escaped defect becomes a permanent gate, backstopped by 85
`test-*.sh` gate self-tests that keep the gates themselves from rotting. The test estate is now just
the *current snapshot* of a process designed to get stronger over time.

---

## 2. Score delta — 8.0 → 8.4, and why it rose only modestly

The +0.4 is deliberately small, and the report structure justifies why:

- **The governance is a genuine multiplier, not decoration.** Pass B adds two scorecard dimensions
  that did not exist in Pass A — **Quality-governance maturity (10/10)** and **Gate-integrity /
  anti-escape (9/10)** — and rates them at or near the ceiling. The postmortem ratchet, the
  single-source meant-to-block allow-list (`MERGE_GATES_BLOCK_ALLOWLIST_RE`), the freshness guard,
  and the 85 failure-asserting self-tests are real, load-bearing mechanisms. If the score moved only
  on these, it would have moved far more than 0.4.

- **But the underlying test estate was already strong.** Pass A had already awarded breadth 9/10,
  memory/concurrency 8.5/10, coverage 8.5/10. There was limited headroom; the governance layer
  cannot retroactively make the tests deeper than they are.

- **And the SAME real gaps persist in both passes, dragging the multiplier down.** The disabled
  absolute 6.94 ms perf budget, the non-gating UI/visual lane, and nightly-only TSan appear in *both*
  scorecards at essentially the same severity. Pass B's perf score (6/10) is even *lower* than Pass
  A's (6.5/10), because seeing the Quality Pillars made the unenforced Pillar-1 budget look worse,
  not better — the meta-layer raises the bar it is then judged against.

Net: the multiplier is real but it multiplies an already-high base while leaving the same holes
unplugged. A modest, honest +0.4 is the correct magnitude. A larger jump would have been a tell that
the evaluator was scoring the *documentation of discipline* rather than the *discipline*.

---

## 3. What Pass B SAW that Pass A was structurally blind to

Pass A's scope rules explicitly excluded `AGENTS.md`, `docs/agent-rules/**`, `docs/self-improvement/**`,
and `docs/agent-eval/**`, and discounted the ~52 of 56 `.bats` files that test the CI/agentic tooling.
That blindness cost it visibility into five things:

1. **The postmortem ratchet provably catching real defects.** Pass B cites specific incidents with
   their spawned gates: **#923** (watcher merged past a red non-required `Coverage` → birthed the
   meant-to-block allow-list, "the single most load-bearing gate in the system"); **#357**
   (`COMMENTED == pass` shipped 5 unaddressed CodeRabbit findings → CR gate rewritten to block on
   `Actionable comments posted: N > 0`); **#1428** (a daemon on a stale checkout enforced two-day-old
   gate logic → fail-closed `MERGE_GATES_FRESHNESS=block` guard with 5 Bats cases). Pass A could see
   the *gates* on disk but not the *incident lineage* that proves they are reactive-hardened rather
   than speculative.

2. **The 85 gate self-tests** (`test-gate-selftests.sh`, "the gate that gates the gates") that
   *require* every `--selftest` to carry a `# selftest: asserts-failure` marker, killing the
   "selftest only walks the happy path" failure class. Pass A counted bats suites but discounted them
   as "meta-layer, discounted"; Pass B recognizes them as the immune system of the gate architecture.

3. **The agent-eval validator-of-validators harness** (`docs/agent-eval/`) — run/score/calibrate
   stages where the LLM judge is itself calibrated against human labels, and `agent-eval-calibrate.py`
   **hard-fails (exit 1)** when judge–human divergence exceeds mean |Δ| ≤ 0.15 / no pair > 0.25.
   Pass B calls this "the most sophisticated 'who validates the validators' answer I have seen."

4. **The honest maturity tiers.** Quality Pillars are explicitly graded enforceable (1–3) /
   WARN-first (5, DRY) / aspirational (4, accessibility, "no auto-fail gates today"). Pass B credits
   this three-tier honesty as exactly how a mature QA org should communicate control maturity.

5. **A NEW risk frame Pass A could not even pose: who validates the AI that writes AND reviews the
   tests?** This is Pass B's §6. `test-author` writes tests; `code-review` + CodeRabbit + Bugbot
   (all LLMs) review them — a shared blind spot could be authored and waved through with no human in
   the loop. Pass A, judging only artifacts, has no vocabulary for this; it is a risk that only
   becomes visible once you look at the agents producing the artifacts.

---

## 4. What Pass A got RIGHT that survived into BOTH passes

This is the most reassuring finding of the comparison: **the two passes, using different lenses,
independently flagged the same three top gaps.** Convergence under different methods is strong
evidence the gaps are real and not lens artifacts.

- **Disabled absolute 6.94 ms / 144 Hz perf budget.** Pass A §4/§6: `mean_abs_ceiling_ms: null`,
  "documented but not enforced — only relative drift is gated." Pass B §7.2: "documented but not
  encoded… ships `null/DISABLED`… the 144 Hz invariant is currently a guarded direction of travel,
  not an enforced ceiling." Identical finding, identical severity, both scored ~6/10 on perf.

- **UI/visual tier is non-gating.** Pass A §4/§6 ("26 UI tests + 3 goldens run `continue-on-error`
  on flaky Mesa GL… merge-blocking power is near zero" — the highest-leverage gap). Pass B §7.5
  ("Mesa-GL bucket-C/E rendering lanes are advisory (flake-driven)… can't boot the CI exe"). Both
  trace it to the same root cause: flaky Mesa software GL.

- **TSan is advisory / nightly-only.** Pass A §4/§6 ("the only real data-race gate… nightly-only…
  narrow curated subset"). Pass B §7.4 ("nightly-advisory, not per-PR… no Windows TSan toolchain…
  up to a ~24h window for a race to sit on `develop`"). Both note ASan/UBSan *are* per-PR-required,
  so the residual gap is specifically races.

Both passes also independently reconciled the **same workflow ambiguity**: an older
`sanitizer-nightly.yml` comment claims "per-PR ASan was rejected as too slow," but both evaluators
checked `project.config.json` and confirmed per-PR ASan (MSVC) + UBSan (Clang) *are* now required
contexts. Same correction, reached twice. Pass A's artifact-level reading was sound and durable.

---

## 5. Contradictions & tensions

**Does the governance ADD confidence or is some of it ceremony?** Pass B confronts this head-on and
mostly defends the weight: "51 postmortems, 50 backlog category files, a 1,519-line gate poller…
unusually heavy, but it is *load-bearing* weight: nearly every rule traces to a cited incident ID."
The honesty tiers and the fact that **only 7 of 51** postmortems close with `none — override
legitimate` (each justified) argue against ceremony. But there is residual tension the report itself
admits: the **agent-eval harness is built but nearly empty** — `test-author` has *no* eval cases,
`code-review` has *one* labelled case plus 3 frozen goldens. So the most architecturally impressive
anti-blind-spot mechanism is, today, more *promise* than *coverage*. That is the seam where "genuine
multiplier" shades toward "scaffolding scored as if populated."

**Does AI-writes-AND-reviews-tests create a blind spot the static-estate view can't see?** Yes — and
this is the deepest tension. Pass A's lens *cannot detect* it, because a test estate that looks
complete can have been authored and approved entirely by correlated LLMs. Pass B exposes the real
mitigations (human-terminal golden-image approval forbidding an agent from `git add`-ing a reference
PNG until the user says "looks right"; the 2026-05-19 incident where a golden "would have certified
the bug as expected behaviour forever"; `test-rig`'s "capture by RUNNING — never transcribe a golden
from source" rule; blocking judge calibration). But it also concedes the blind spot is **not closed**
— concurrency invariants have *no* automated validator at all, and the escape hatches are
LLM-applicable.

**The escape-hatch tension.** Both passes note `tests-out-of-band`; Pass B goes further and inventories
the whole set, including the blunt **`SKIP_MERGE_GATES=true`** global bypass ("the one real weak link,"
audited only by a `LOG_WARN`). Critically, Pass B shows that `tests-out-of-band` **recurs legitimately**
for behaviour-changing *concurrency* fixes (#1390/#1409) precisely because the headless single-threaded
doctest rig structurally cannot host a threading assertion. So the same out-of-band hatch that looks
like a coverage hole in Pass A is, in Pass B, revealed as a *structural* consequence of the test-rig's
limits — a more sympathetic but also more worrying reading, because it means a whole defect class
ships waivered by design.

---

## 6. Critic's verdict

**Which pass is more decision-useful for a QA director?** For a QA director making a *go/no-go on
release confidence*, **Pass B is more decision-useful**, because the most important QA question in an
agent-driven codebase — "what happens to a defect *after* it escapes, and who checks the AI that wrote
the test?" — is invisible to Pass A. Pass A tells you the estate is strong today; Pass B tells you
whether the estate will *stay* strong and where the systemic blind spot lives. That said, Pass A is the
better *audit baseline*: it is disciplined about counting only tangible artifacts and is therefore
immune to being dazzled by process documentation. The ideal QA director reads A first to anchor on
artifacts, then B to understand the system around them.

**Is "quality as governance" the future or over-process?** On this evidence it is closer to the
future than to over-process — but conditionally. The ratchet *demonstrably turns* (postmortem →
preventing gate → backlog → shipped fix, traced in #1517, #1534), and the gates that gate the gates
are real. The over-process risk is concentrated in exactly one place: mechanisms scored for their
*architecture* before they are *populated* (agent-eval), and weight that would be ceremony in a
human team but is arguably necessary discipline for autonomous agents.

**Overclaim audit.** Both reports are unusually careful, but two overclaims deserve flagging:
- **Pass B, "top-decile / best-in-class" + a 10/10 governance score.** Awarding a perfect 10 to a
  governance dimension whose flagship validator-of-validators harness is admittedly near-empty is the
  report's one genuine overclaim. A 9 would have been defensible; the 10 reads as scoring the design,
  not the deployed coverage. The report's own §6 ("built but essentially unpopulated") contradicts the
  10/10.
- **Pass A, "better than the project itself documents" (backend HTTP transport).** This is *correct*
  and well-evidenced (`TrackerHttpFaults.test.cpp` vs the stale gap doc), but it is stated more
  triumphantly than the single catalog-path test warrants; transport fault injection exists for *some*
  paths, not the whole backend surface.

Neither overclaim is disqualifying; both reports are honest about their own residual gaps, which is
rare and to their credit.

---

## 7. Synthesis — combined QA bottom-line and blended score

Smatchet is an **advanced test estate wrapped in a best-in-class, self-tightening quality-governance
loop**, with a small, *honestly documented*, and *lens-invariant* set of real soft spots. The two
passes agree on the artifacts and the gaps; they differ on whether you also credit the process that
turns gaps into gates. That process is genuine — incident-traced, self-tested, and ratcheting — but it
is not yet uniformly populated (agent-eval), and it does not yet plug the three gaps both lenses flag:
the parked 6.94 ms absolute perf budget, the non-gating Mesa-GL UI/visual lane, and nightly-only race
detection. The deepest residual risk is the one only the with-agents lens can see: an LLM that both
writes and reviews tests, behind a validator-of-validators harness that is architecturally right but
nearly empty, plus a blunt `SKIP_MERGE_GATES` bypass and an out-of-band hatch that waivers an entire
concurrency defect class by structural necessity.

The governance layer earns a real but bounded premium over the artifact-only view: it makes the system
*credibly self-improving*, which is worth more than any single additional test suite, but it cannot
substitute for the three enforcement gaps it has so far only *documented*.

**Blended QA score: 8.2 / 10.** (Pass A 8.0 anchors the tangible estate; Pass B 8.4 credits a genuine,
incident-traced governance multiplier; the blend sits just above midpoint because the multiplier is
real and load-bearing, but is partly scored on architecture-not-yet-populated and leaves the same three
gaps both passes independently flagged.)
