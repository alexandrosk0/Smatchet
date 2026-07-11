# Smatchet — Comparison & Critic Report (Expert 03: Game-Dev Tracker User)

*Meta-analysis of one expert's two-pass evaluation. The expert is a gameplay/engine programmer at a small-to-mid Unreal + Perforce studio, evaluating Smatchet as a prospective **user/buyer** of a tracker tool. Pass A (`without-agents.md`) judged the shipping product only, deliberately ignoring the agentic-governance meta-layer; it scored 6/10. Pass B (`with-agents.md`) judged the same product but additionally read the meta-layer as a maintenance/trust signal; it scored 6.5/10.*

---

## 1) Executive Summary

The single biggest way the meta-layer changed this expert's view is that it converted an **inference into evidence** — but for a buyer, the evidence cut both ways and the net needle-movement was small.

In Pass A, the dev's worry about code durability ("will a refactor silently break the Jira backend?") was a vague early-adopter anxiety. In Pass B, reading `docs/self-improvement/postmortems.md` (a 255-heading append-only blameless ledger), the 28 CI workflows, the armed perf-regression gate, and the per-subsystem `AGENTS.md` map, the dev can now *answer* that worry: "there's a gate and a fixture suite for that." So the governance layer raised confidence specifically in **build quality and regression protection** — the dev calls the postmortem ledger "the single strongest trust signal in the repo."

But the meta-layer is a maintenance/trust signal, **not a feature a user touches**. It does not give the buyer binaries, a GA Unreal plugin, an enforced framerate, or a support contract. And the very same documents that proved the code is well-built also *confirmed and worsened* the dominant risk: `git shortlog` showed 49 commits by one human + 1 by dependabot, and `AI_POLICY.md` self-declares "solo-maintained, prerelease." The governance machinery the dev admired is revealed to be **operated by a single person amplified by autonomous AI agents**. So the meta-layer raised trust in the *code* and lowered trust in the *continuity of the vendor* — and for a buyer betting on workflow continuity, continuity dominates. Net adoption-needle movement: half a point, and the recommendation ("pilot, don't standardize") is verbatim the same in both passes.

---

## 2) Score Delta — 6 → 6.5, and why it is small

The headline overall moved only +0.5. The reason is structural to the **buyer/user persona**: for someone deciding whether to deploy a tool, the value drivers are features, distribution (binaries/installer), platform maturity, and support — not the rigor of the upstream build process. Build-process discipline is something a buyer is *glad* exists but does not pay for directly.

Look at where the points actually moved between scorecards:

- **Tracker functionality: 7 → 8.** This rose because Pass B *counted backends in code* (five concrete implementations including a `LinearFixtureBackend`, plus `linear-live-smoke.yml` CI) versus Pass A's "four backends." This bump is a product re-read, only loosely tied to the meta-layer.
- **Performance: 6 → 6.5.** Marginal; both passes reached the same substantive conclusion (relative gate real, absolute 6.94 ms budget disabled).
- **A new row appeared — "Maintenance-confidence (agentic layer): 6.5"** — which is itself a hedged middle score: governance excellent, bus factor pulls it back down. A 6.5 on the new dimension cannot drag a 6 to anything dramatic.
- **Unreal (6 vs 7), Ease of adoption (4 vs 4), Maturity (5 vs 6)** are essentially flat.

Crucially, every **dominant blocker** is unchanged or reinforced: no prebuilt binaries (gates non-engineer adoption in both), beta Unreal plugin (`IsBetaVersion: true`, `EnabledByDefault: false`, 0.6.7 in both), disabled absolute perf budget (both), and bus factor (Pass A *suspected* "single-author/internal-project smell" from the `docs/perforce/SETUP.md` runbook's one-box/one-user signals; Pass B *confirmed* it from `git shortlog`). When the things that actually block a purchase don't move, the score can't move much. The +0.5 is the dev saying "I trust the code more, but I can't act on a tool I can't install and can't be sure will be maintained."

---

## 3) What the WITH pass saw that the WITHOUT was blind to

1. **Regression protection as a concrete artifact.** Pass A admired the perf *instrumentation* (`SMATCHET_UI_PERF_SCOPE`, `perf.snapshot`) but treated durability of the *features* as an open question. Pass B read `perf-pr-fast.yml`, `perf-compare.py`, and `regression-policy.json` (thresholds `mean_delta_pct: 10`, `p99_abs_ceiling_ms: 10`) and the test-to-source ratio (~307 test files vs ~297 source), turning "I hope it's tested" into "the relative gate is armed and has caught noise-level deltas."

2. **The postmortem ledger as evidence of discipline.** This is the cleanest net-new insight. Pass B read actual entries — the 2026-06-27 #1566 "Perf PR-fast CANCELLED merged via human native-merge" RCA and the #1438/#1428 intent-gate bypass entries — and recognized a system that responds to a gate-escape with *a new gate*, not a one-off fix. Pass A could not see this at all.

3. **The merge-gate mechanics.** `merge-gates.sh` checking CI + CodeRabbit + Cursor Bugbot + unresolved comments in one GraphQL call, with an audited "meant-to-block allow-list" and explicit `*-out-of-band` override labels. Invisible to Pass A.

4. **Bus factor — now confirmed and *worse*.** This is the most important asymmetry. Pass A flagged the risk softly and almost charitably ("it tells me this is a focused tool built by someone who actually does this work"). Pass B made it the **dominant risk**: not just one maintainer, but one maintainer **operating an intricate AI harness** (merge-watcher daemons, freshness guards, loop modes, token budgets), where the postmortems themselves reveal the *automation* misfiring at the edges (stale daemon allow-lists, native-merge bypassing the poller). So the same meta-layer that raised code-quality trust simultaneously revealed a *harder* succession problem: a new engineer must climb a "harness-comprehension curve before touching product code," and "there is no evidence of a second human who understands the system." The governance is partly bus-factor insurance (auditable, externalized rules, forkable under MIT-ish `LICENSE`) — but the honest reading "cuts both ways," and the aggravating side is heavier for a buyer.

---

## 4) What the WITHOUT pass got right that survived — and was sharper for being feature-focused

Because Pass A was forbidden the meta-layer, it spent its full attention on what the user actually receives, and it produced the most decision-relevant findings in either document:

- **The P4 callstack-driven annotate differentiator.** Pass A's deepest, sharpest section: the crash-callstack-to-blame *pipeline* — paste/auto-pull a stack (`CallstackTrackerFieldId`, auto-detecting a field named "callstack"), parse frames, apply depot↔workspace path-remap rules, run `P4AnnotateLine` per frame on a background worker with a bounded `P4ChangelistDescribeCache` (512 entries), pin to `@changelist`, render with syntax highlighting and P4V/`p4vc` launch-outs. Pass A nailed the *why*: "who broke this line and in which CL" is the most common triage question, and nothing native does the callstack-wide sweep. Pass B mentions annotate but describes it more flatly ("the high-value 20%") — **Pass A's feature focus produced the better articulation of the single reason to install the tool.**

- **Beta Unreal plugin.** Both flag it; both read the metadata identically. This survived intact.

- **The aspirational perf claim.** Pass A already caught that the 144 Hz / 6.94 ms budget is "a guaranteed-pass no-op today," self-described as PARKED, and that the scenario path measures UI-thread CPU under headless software GL (Mesa) — "does NOT capture real framerate." This is essentially the same finding Pass B reaches with the policy file.

- **No prebuilt binaries.** Pass A's framing — "First contact is a build, not a double-click. That alone gates non-engineer adoption" — is the crispest statement of the #1 adoption blocker in either report. Survived and remained the governing constraint.

Pass A also produced sharper *product-shortfall* detail that Pass B partly dropped: no boards/sprints/swimlanes, plain-text-only `ticket.add_comment`, weak notifications, one-backend-at-a-time switching with `trackerType` "restart required," and the CLI requiring a running app instance (MCP endpoint `127.0.0.1:42360`). These are exactly the things that keep a team on Jira/Linear-web, and the feature-only lens surfaced them more thoroughly.

---

## 5) Contradictions & Tensions — does agentic rigor increase or decrease buyer trust?

This is the central tension, and Pass B holds both poles honestly rather than resolving them:

- **Increases trust** — for code quality and regression safety. The test ratio, armed gates, and learning-from-escapes postmortem loop are "better than most commercial internal tooling I've evaluated."
- **Decreases trust** — for supportability and continuity. "Weird, AI-built, single human" is precisely the profile a risk-averse studio fears. The dev concludes the agentic layer is, beyond raising code-quality confidence, "the vendor's internal plumbing — and its bus factor is the thing I'd negotiate hardest on."

For a *user/buyer* persona these do not cancel cleanly, because they apply to different decision axes. Code-quality trust reduces the risk of *using* the tool day-to-day; continuity trust governs the risk of *betting your workflow* on the vendor long-term. A pilot only needs the former; standardization needs the latter. That is exactly why both passes land on the same verdict — "pilot, don't standardize" — and why the score barely moved.

**Does meta-layer access make the disabled-perf-budget finding stronger?** Yes, materially. Pass A relied on the project's *plan/prose docs* ("PARKED," "guaranteed-pass no-op today") — credible but secondhand. Pass B read the **machine-checked source of truth**: `regression-policy.json` shows `mean_abs_ceiling_ms` is `null`/DISABLED with an in-file comment ("pending perf-gate-revival step-5 calibration"), *while* the relative gate (`perf-pr-fast.yml`) is genuinely armed. So Pass B can make the more precise claim — "144 Hz is a target with a *relative* guard, not a hard absolute ceiling" — distinguishing what is enforced from what is aspirational. The governance access upgraded a documentation-based assertion into a config-verified one. (Notably, the dev reads this honesty as trust-*raising*: "the fact they wrote it down rather than overclaiming raises my trust." So even the strengthened negative finding nets out positive on the trust axis — another reason the score rose only slightly rather than falling.)

A genuine **tension between the two passes**: Pass A scored Maturity 5 with "parked perf gate" cited as a demerit; Pass B effectively rewards the *same* parked state as evidence of honesty (Maturity 6). The underlying fact is identical; the meta-layer reframed a product gap as a character reference. A buyer should be alert that this is a subtle pro-vendor drift introduced by reading the governance docs.

---

## 6) Critic's Verdict — which pass better serves a studio's buy decision

**For the actual buy/deploy decision, Pass A (without) is the more decision-useful document, and Pass B is the better *trust-calibration* document.** A studio's purchase hinges on: can my people install it (no — build from source), does it do something they can't get elsewhere (yes — P4 callstack annotate + Unreal overlay), and is it mature enough to depend on (not yet). Pass A answers all three with sharper feature evidence and the crispest blocker framing ("first contact is a build, not a double-click"). The meta-layer, for a *user* persona, is largely **the vendor's internal plumbing** — the dev says so explicitly in Pass B. It is decision-relevant in exactly two narrow ways: (a) it confirmed the bus factor that Pass A only suspected, and (b) it let the dev verify (not just believe) that the code won't silently rot. Everything else in the governance layer — agent rosters, loop modes, merge-gate GraphQL — is interesting context a buyer would never pay for.

So the meta-layer is **not a pure distraction**, but it is *over-weighted relative to its buyer-impact* in Pass B. The new "Maintenance-confidence" scorecard row (6.5) gives the agentic machinery a full dimension of influence over the overall score, when for a buyer its real contribution is narrower: one confirmed risk and one resolved anxiety.

**Critique of Pass A:** Its discipline is its strength — it stayed on the product and produced the best feature analysis. Its weakness is that by design it could only *guess* at the bus factor (the "Brick"/`alexk`/one-depot smell) and at code durability; it left the buyer's two biggest non-feature questions (will it be maintained? is it tested?) unanswered. It also slightly under-weighted that "single-author internal-project smell" deserved to be a top-line risk, not a bullet in section 4.

**Critique of Pass B:** It is more *complete* and more honest about risk, but it lets the governance layer pull focus. It dilutes the P4-annotate differentiator (Pass A's strongest insight) into a one-liner, and it imports a mild pro-vendor framing by crediting honest documentation as a trust-raiser even where the underlying state (disabled perf gate, beta plugin) is a product gap. Its single best contribution — confirming the AI-built, one-maintainer bus factor with `git shortlog` — is genuinely decision-relevant and would have been the most important thing for the studio to hear.

---

## 7) Synthesis — combined recommendation and blended score

**Combined adoption recommendation:** *Run a contained engineer pilot now; do not standardize.* Put one or two engine programmers (who already build the editor from source) on a sandboxed pilot of **GitHub/Jira backend + P4 callstack annotate, standalone build first, Unreal overlay second**, on real studio crash data. Validate auth/SSO/custom-field round-tripping and shared view definitions. Pair it with — do not replace — existing Jira/Linear-web for planning. Treat it as an engineer's triage companion, not a studio-wide tracker.

The pilot flips toward broader adoption only when **all** of the dominant blockers clear, in priority order for a buyer: (1) **a tagged release with signed binaries + installer** (the packaging scripts already exist — just publish artifacts); (2) **a second maintainer or a support/SLA/escrow arrangement** directly addressing the AI-operated, single-human bus factor; (3) **the Unreal plugin exits beta** with a stated supported-UE-version matrix; (4) **the absolute perf gate armed** so 144 Hz is enforced, not aspirational; and (5) **tracker parity** (Markdown comments, boards/sprints, notifications, multi-backend side-by-side) to actually pull a team off web SaaS.

**Blended score: 6.25 / 10.** The product is a genuinely differentiated *engineer's tool* for a P4+Unreal studio (callstack annotate is best-in-class; the in-editor overlay is novel), built with more rigor than its 50-commit history suggests — but it is prerelease, distributes only as source, ships a beta engine plugin, has a disabled absolute perf budget, and rests on a single AI-amplified maintainer. The meta-layer earned the half-point of upside (verified code quality) and simultaneously justified withholding more (confirmed bus factor), which is exactly why the two passes converge near 6 and on the identical verdict: **pilot, don't standardize.**
