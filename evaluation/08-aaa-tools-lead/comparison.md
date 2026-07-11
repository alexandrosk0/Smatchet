# Smatchet — Comparison & Critic Report: AAA Tools/Pipeline Tech Lead (Expert 08)

**Meta-analyst lens:** comparing the same tools-lead's two-pass evaluation — Pass A (without the agentic meta-layer) and Pass B (with it) — to isolate what the meta-layer changed, what survived unchanged, and which pass is more decision-useful for a 200-dev AAA studio deciding whether to lift Smatchet into its internal tooling stack.

**Source documents:**
- Pass A — `evaluation/08-aaa-tools-lead/without-agents.md` (Verdict: LIFT COMPONENTS, **7/10**)
- Pass B — `evaluation/08-aaa-tools-lead/with-agents.md` (Verdict: LIFT — components AND methodology, **7.5/10**)

---

## 1. Executive Summary

The single biggest way the meta-layer changed this tools lead's view is that it **added a second liftable asset** on top of the code. In Pass A the deliverable was a *parts list*: a command registry, an Unreal bridge, a tracker abstraction — subsystems to extract from an application the lead otherwise dismisses as "a tracker client, not a pipeline tool." In Pass B that parts list survives almost verbatim, but it is joined by a peer of equal-or-greater value: the **AI-team operating system** — `AGENTS.md` + `AI_POLICY.md` + `docs/agent-rules/**` + the `agents/` roster + the CI-gate constellation — which Pass B calls "the **higher-value artifact for a tools lead**" and scores a standalone 9 (Pass B §1, §4, §8).

The framing shift is sharp and self-aware. Pass A deliberately ignored the governance tree (it lists the excluded paths explicitly in its §2 and says it "disregarded" the agentic portions of `docs/PORTABILITY.md`). Pass B reads exactly those files and concludes that for a lead whose actual job is *running a small tools team*, a battle-tested methodology for governing autonomous coding agents — backed by 255 postmortems and made liftable via `project.config.json` — is more portable than the C++ app and more relevant than any single subsystem. The meta-layer didn't change what the lead thinks of the *code*; it changed what the lead thinks the *repo is for*. Pass A sees a parts donor. Pass B sees a parts donor that also ships a playbook.

---

## 2. Score / Verdict Delta

| | Pass A (without) | Pass B (with) |
|---|---|---|
| Verdict | LIFT COMPONENTS | LIFT — components AND methodology |
| Overall | **7/10** | **7.5/10** |
| New scorecard row | — | "Agentic-methodology value-to-us: **9**" |

The rise is real but modest (+0.5), and the modesty is the honest part. The methodology row enters the scorecard at 9 — the highest single score in either report, tied with the command registry — yet it only nudges the overall half a point. Three forces explain the restraint:

1. **The methodology is genuinely valuable to a tools team.** Pass B is not hedging: delta-gated grandfathered lint, "never merge past any red check," the postmortem-owed flywheel, and the `on`/`in` loop-mode charter are each called "directly liftable" and one ("steal verbatim") for brownfield repos (§4.4, §4.5).

2. **But the gate plumbing is GitHub-shaped.** Pass B's §5 names the largest porting cost precisely: the merge-gate machinery (GraphQL poller, CodeRabbit, Bugbot, `gh` CLI) "is GitHub-shaped and needs an adapter layer" for a Perforce/Swarm shop. The *methodology* ports cleanly; the *plumbing* does not. That caps the upside.

3. **The app-as-product blockers are unchanged.** Every Pass A disqualifier survives into Pass B intact — single-user, no SSO/RBAC, DPAPI-only secrets, Windows-first, local-scale-only. Both passes hold AAA-scale readiness at **3/10**. The methodology adds a new asset; it does nothing to make the application adoptable. The overall can only rise by the weight of one new (if excellent) lift candidate against an unchanged pile of scope gaps.

A half-point is the correct magnitude: a new top-tier liftable asset, discounted by a real adapter-build cost and by the fact that it doesn't touch the reasons "adopt" was already off the table.

---

## 3. What the WITH Pass Saw That the WITHOUT Was Blind To

By construction, Pass A could not see the governance layer — it excluded `AGENTS.md`, `AI_POLICY.md`, `docs/agent-rules/**`, `docs/self-improvement/**`, `docs/harness/**`, and the agentic portions of `PORTABILITY.md`. Pass B surfaced a coherent second product hiding in those files:

- **Delta-gated, grandfathered lint** (§4.4) — rules fire only on *new* violations vs `origin/develop`; existing violators are baselined; `SMATCHET_DEVIATION(rule=…; reason=…; owner=…; revisit=…)` is the auditable escape. Pass B flags this as the standout transplant for a *brownfield* studio repo: ratchet quality on new code without a boil-the-ocean cleanup of the mountain you inherited.
- **The postmortem flywheel** (§4.5) — 255 blameless RCAs, each with a mandatory "Preventing gate," plus a `postmortem-owed.sh` SessionStart nudge when something ships that a gate should have caught. Pass B reads this both as a 10x mechanism and as a "free risk map" of the codebase's sharp edges.
- **The `AI_POLICY` loop modes** (§4.2) — `on` (action-biased, human-on-the-loop) vs `in` (plan-gated, human-in-the-loop) plus escalate-when-unvalidatable. Pass B calls this a mature articulated answer to "how much rope, where's the stop?" — a question most studios cannot answer at all.
- **`project.config.json` portability** (§5) — the explicit claim that "coupling to *this* project is values, not design," enforced by a `test-portable-purity.sh` guard so the portable/specific boundary can't silently rot. The parameterized file covers build presets, the 6.94ms budget, lint zones, VCS topology, branch protection, the subsystem→agent map, and governance defaults.
- **The capability-tag harness adapters** (§5) — agents declare capability *tags* mapped per harness (Claude Code / Codex / Cursor / Aider / generic), so the roster isn't vendor-locked — directly relevant to a studio running heterogeneous tooling.

The deepest insight Pass B reaches — and Pass A structurally could not — is that **the methodology is more portable than the app.** The C++ substrate is Windows/DX12-first, single-user, and bound to its product; the governance tree is one-config-file-from-reusable. For a tools lead, the most transplantable thing in the repo turns out to be the part Pass A was told to ignore.

---

## 4. What the WITHOUT Pass Got Right — and Sharper for Being Code-Focused

Pass A's narrow lens is a feature, not just a limitation. By staying inside the C++, it produced the more *precise* component analysis, and its conclusions survived Pass B with no downgrades:

- **The command registry as the #1 lift.** Both passes rank `Source/Core/src/Commands` first and score it **9/10**, but Pass A carries the load-bearing detail: the reentrancy contract where `Dispatch` copies the `Command` (and its handler `std::function`) under the mutex and *releases the lock before invoking* so a recursive Lua `commands.invoke` can't deadlock (`CommandRegistry.cpp:283-295, 342-345`); the bounded SAX parse on JSON args (lines 196-213); `BuildJsonSchema()` giving MCP `tools/list` for free (`McpPlugin.cpp:498-501`); one-block registration (`BuiltinCommands_App.cpp:20-89`). Pass B asserts "no-bypass, structured envelope"; Pass A *proves* why the registry is worth lifting.
- **The Unreal bridge specifics.** Pass A's §3 is the deeper engineering read: the C-ABI-not-C++-ABI decision to dodge UBT/MSVC mismatch; the async command queue with explicit free discipline (`SmatchetImGuiHostC.h:69-78`); DX12-into-Slate via `RHIGetGraphicsCommandList` fallback when `GetNativeCommandBuffer()` is null (lines 24-36); the 128-entry SRV heap with slot 0 reserved for the font atlas; passing descriptor handle *values* not pointers across the module boundary. Pass A's "this proves someone shipped it against a real engine, not a toy" is an evidenced judgment Pass B only gestures at.
- **The AAA-scale gaps, fully itemized.** Pass A §8 nails every disqualifier with citations — per-user PAT + Windows DPAPI (`ConfigManager.cpp:455-462`), no OAuth/SAML/LDAP, read-only shell-out P4 (no P4 C++ API / `-G`, no submit/shelve/stream, brittle regex header parsing at `P4Annotate.cpp:437-449`), local-only scale, no RBAC/fleet-config/telemetry. Pass B compresses all of this into a tighter §3.7, but its substance is Pass A's, inherited wholesale.
- **The Lua-not-Python studio-fit cost.** Pass A §6 raises a concrete adoption friction Pass B underweights: AAA TA tooling is Python (Maya/ShotGrid/Houdini), so sol2-Lua is a retraining cost that doesn't interop with existing pipeline libs.

**Both passes correctly caught the perf-budget nuance — and this is worth flagging as a shared strength.** Pass A states the 6.94ms/144Hz budget is *enforced* in scenario tests (`LongTextOpenLargeAdfScenario.cpp:133` asserts `topMs <= 6.94`) — i.e. enforced *relative*, in-scenario. Pass B sharpens the same point from the policy side (§7): the **relative** regression gate is **armed** in CI (`perf-pr-fast.yml`, baselines committed), but the **absolute** mean ceiling (`mean_abs_ceiling_ms`) ships **`null`/DISABLED** in `docs/perf/regression-policy.json` pending calibration — "don't quote 6.94ms as a guaranteed enforced ceiling." Neither pass over-claimed; together they triangulate the honest reading (regression-gated, absolute-disabled). That is the single best example of the two passes reinforcing rather than contradicting each other.

---

## 5. Contradictions & Tensions

**Is the methodology the real prize, or a distraction from the (limited) reusable code?** This is the productive tension between the passes. Pass B declares the methodology "the higher-value artifact for a tools lead" and scores it 9 — yet the same report concedes the reusable *code* is thin (three or four components from an app it otherwise skips). A skeptic could argue Pass B is seduced: a tools lead's deliverable is *shipped tools*, and a governance playbook ships nothing on its own. But Pass B's own framing answers this — for a lead whose job is *running a team* (not just writing code), a working answer to "how do we trust autonomous agent merges" is exactly the bottleneck a small AAA tools team hits, and the code components plus the methodology are not either/or; they're a substrate plus an operating model for building on it.

**Does lifting AI-built code create maintenance risk?** Pass A is *blind* to this — it praises code quality (RAII, 307 tests, bounded parsing) with no idea the code is agent-authored, because the methodology that reveals that fact was out of scope. Pass B (§6, §7) surfaces it as both a risk and its own mitigation: the code is "substantially agent-authored," bus-factor-1, but the gates *are* the mitigation, and the 255-postmortem ledger is "reassuring *and* a reminder of how many escapes occurred before each gate existed." This is a genuine tension the meta-layer creates: seeing the governance simultaneously *raises* confidence (rigor is structural, not personal) and *reveals* the provenance risk that should lower it. Pass A's higher implicit comfort with the code rests on ignorance Pass B dispels.

**Is GitHub-shaped governance liftable into a P4/Swarm AAA shop?** The sharpest unresolved tension. Pass B is honest that the merge-gate plumbing is GitHub-coupled and needs a Perforce/Swarm adapter — "the largest porting cost." This cuts against the methodology's headline 9: a model whose enforcement layer must be rebuilt for your VCS-of-record is a model you partially re-implement, not lift. Pass B threads this by separating *methodology* (ports cleanly: delta-lint, postmortem loop, loop-modes, delegation packets) from *plumbing* (GitHub-shaped: adapter needed) — a real distinction, but one that quietly concedes the 9 is for the ideas, not the runnable scripts. A AAA shop buying the methodology is buying a design and a porting project, not a turnkey gate system.

---

## 6. Critic's Verdict

**Which pass is more decision-useful for a AAA tools lead?** For the *adopt-vs-lift-vs-pass* gate, Pass A is sufficient and tighter — it correctly establishes that the app is a parts donor, the registry is the #1 lift, and the AAA-scale gaps make "adopt" impossible. Nothing in Pass B overturns that spine. **But for the lead's actual strategic question — "what is the maximum value I can extract from this repo for my team?" — Pass B is decisively more useful**, because it finds an entire second asset class (the operating model) that Pass A was structurally forbidden from seeing, and it correctly identifies that asset as more portable than the code. A tools lead reading only Pass A would lift three subsystems and walk away from the most transplantable thing in the repo.

**Reconciling the two "lift" verdicts:** they are not in conflict — Pass B is a strict superset. "LIFT COMPONENTS" and "LIFT components AND methodology" describe the same code recommendation with one bolted-on extra. The verdicts agree on every component, agree on every skip (shell-P4, the product UI, Windows-only auth), and agree on every AAA-scale gap. Pass B simply adds a methodology lift-list (delta-lint, never-past-red, postmortem flywheel, loop-mode charter, delegation packets) and an "adopt-as-shape" note for `project.config.json` + `PORTABILITY.md`. The +0.5 is the entire delta.

**Critique of Pass A:** Its discipline is its weakness. By excluding the governance tree it (a) never learns the code is AI-built and so cannot price the provenance/bus-factor-1 risk Pass B foregrounds, and (b) misses that the most liftable artifact in the repo isn't C++ at all. Its perf claim ("enforced") is *almost* over-stated — saved only because it correctly scoped "enforced" to in-scenario relative assertions, which Pass B confirms.

**Critique of Pass B:** It risks letting the methodology's elegance inflate its practical score. The 9 for "methodology value-to-us" is for a system whose enforcement layer is admittedly GitHub-shaped and must be re-platformed onto Perforce/Swarm before a single gate runs in this studio — a caveat Pass B states but doesn't fully discount in the number. It also somewhat soft-pedals the Lua-not-Python friction that Pass A names concretely. And by reading the methodology as a maintainability proxy, it leans on the 255 postmortems as a positive signal while under-weighting that the same ledger is evidence of how much agent-authored code needed gating after the fact.

---

## 7. Synthesis — Combined Recommendation

The two passes compose into one coherent recommendation: **LIFT, on two tracks, and build the studio-scale layers yourself.**

**Lift — app components (high confidence, both passes agree):**
1. **The Unified Command registry** — `Source/Core/src/Commands/*` + `Command.h` + `Json/BoundedJsonParse` + the `IMainThreadPoster`/`MainThreadDispatch` seam. The #1 lift in both reports (9/10). One definition → CLI + palette + MCP + Lua + Unreal. Take it near-as-is as the studio tools-command backbone.
2. **The Unreal C-ABI ImGui bridge** — `SmatchetImGuiPlugin` (C-ABI host + `ISmatchetImGuiRenderBackend` + command bridge + DX12-into-Slate). The template for in-editor tooling; write a render backend for the proprietary engine behind the same ABI (weeks, not days; gated on the engine exposing a native command list + descriptor heap at present).
3. **The ISP-split `ITrackerBackend` abstraction** — to wire internal/ShotGrid trackers as first-class backends with default-impl optional capabilities; override hot paths later.
4. **Reference-only:** the P4 annotate parser (`P4AnnotateParse.cpp`) as a *starting point* for a blame tool, and the MCP server security model (rebind defense, constant-time token, bounded parse, SSE caps) as a verbatim pattern for an internal MCP-tool host.

**Lift — methodology (Pass B's addition, selective):**
1. **Delta-gated grandfathered lint with `DEVIATION` escapes** — for introducing rigor into brownfield studio tools repos without a full cleanup.
2. **"Never merge past any red check" + `safe-merge` discipline** — adapted onto the studio's review tool; the in-flight-non-required-check blind-spot lesson is pre-paid.
3. **The postmortem-owed flywheel** — every escape mints a gate.
4. **The `AI_POLICY` loop-mode charter** (`on`/`in` + escalate-when-unvalidatable) — the explicit autonomy policy.
5. **`project.config.json` + `PORTABILITY.md` as the *shape*** for a studio's own portable-governance repo, and the capability-tag harness-adapter table to avoid vendor lock.

**Build yourself (unchanged across both passes):** SSO/RBAC, multi-user shared services, fleet config + telemetry, a real P4 C++ API / `-G` layer for pipeline-grade Perforce, and — the critical methodology caveat — a **Perforce/Swarm adapter for the merge-gate plumbing**, since the lifted governance enforcement is GitHub-shaped.

**Skip:** the app as a product (tracker-client UI, views editor, issue-create), shell-out P4 for real pipeline use, Whisper/AI side-panel, Windows-only signing/DPAPI/installer assumptions, localization/mobile, and the ref-based plan-locks + full multi-harness machinery unless actually running concurrent agent fan-out.

**Final blended score: 7.5/10 — LIFT (components + methodology).** Pass B's number is correct and supersedes Pass A's; the half-point premium over Pass A is exactly right — a new top-tier liftable asset (the operating model), discounted by a genuine GitHub→Perforce gate-adapter build and unchanged app-as-product disqualifiers. The repo is a strong *parts donor that also ships a playbook*: lift three or four C++ components, selectively lift the AI-team operating model, treat the 255-postmortem ledger as a free risk map, and build the multi-user/auth/scale and Perforce-gate layers on top.
