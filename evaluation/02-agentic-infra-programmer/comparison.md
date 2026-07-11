# Smatchet — Comparison & Critic Report (Expert 02: Agentic-Infra Programmer)

*Meta-analysis of one expert's two-pass evaluation. Pass A ("without-agents")
read only the shipped product's agentic surfaces — command registry, in-process
MCP server, streaming AI assistant, Lua. Pass B ("with-agents") read the same
product surfaces but ALSO the full agentic-governance meta-layer — the
orchestrator/subagent tree, ship-loops, merge-gates, quality pillars,
self-improvement flywheel, and harness portability.*

---

## 1. Executive Summary

The single biggest way the AGENTS.md layer changed this expert's assessment is
that **it relocated the project's center of gravity.** In Pass A, the thing worth
stealing is a *product mechanism*: the typed-`Command`-struct-as-tool-definition
with a single `Dispatch` chokepoint that fans out to five frontends. The verdict
is "outstanding tool *plumbing*, but no actual agent *loop*" — and the headline
complaint is that the in-app assistant cannot call the very tools the same
process exposes over MCP. In Pass B, the product surfaces are demoted to a single
brief section (§5, "solid, dogfood-able, not novel") and the real artifact becomes
the **development harness** wrapped around the product: a "gate, don't trust"
software factory whose thesis is *autonomy is cheap to grant and dangerous to
trust, so spend the engineering on the verification boundary, not the agent's good
intentions.*

Crucially, the meta-layer did **not** refute Pass A — it *answered* it. Pass A's
loudest gap ("there's no agent loop, no eval harness, no reliability discipline")
turns out to be true *of the product* but false *of the harness*: the harness has
the ship-loop (the agent loop Pass A wanted), a subagent-eval harness (the eval
Pass A wanted), and merge-gates + WARN-first graduation (the reliability discipline
Pass A wanted) — just aimed at the *development* agents, not the in-product
assistant. The with-agents pass therefore reads less like a correction and more
like discovering that the building has a second, larger floor the first survey
never entered.

## 2. Score Delta

**Overall: 7.5/10 (without) → 8.5/10 (with).** +1.0.

The two scorecards measure *different dimensions* — they are not the same rubric
re-scored, which matters for interpreting the delta. There is no clean
per-dimension subtraction because only one dimension overlaps. Mapping them:

| Dimension | Without | With | Delta | Why it moved |
|---|---:|---:|---:|---|
| Product agentic surfaces (registry/MCP/Lua/assistant) | ~8 (avg of 9/7/8/9/8) | **7/10** | **−1** | Same code, *lower* score — see below |
| Delegation / subagent model | — | 9/10 | new | Only visible with the `agents/` tree |
| Ship-loop autonomy | — | 8/10 | new | The "agent loop" Pass A said was missing |
| Gate / quality enforcement | — | 9/10 | new | Delta-lint + exit-code-not-echo merge gate |
| Self-improvement loop | — | 9/10 | new | Escape→postmortem→preventing-gate flywheel |
| Harness portability | — | 8/10 | new | Capability-tag adapter |
| Concurrency model | — | 8/10 | new | Git-ref CAS plan-locks |
| **Overall inspiration** | **7.5** | **8.5** | **+1.0** | Meta-layer adds a whole high-scoring subsystem |

The most interesting movement is the one dimension that overlaps: the **shipped
product surfaces scored LOWER in the with pass (7/10) than the average of their
Pass-A sub-scores (~8).** This is not a contradiction in the code; it is a
*reference-frame effect.* In isolation, the command registry is a 9/10 marvel of
"define once, expose everywhere." Sitting next to the merge-gates poller, the
self-tightening flywheel, and the capability adapter, the same registry reads as
"solid, dogfood-able, **not novel**" (§5/§7). The meta-layer raised the bar the
product is judged against. So the +1.0 overall is *understated* about how much the
expert's enthusiasm shifted: the product didn't get better or worse, but it stopped
being the story.

The overall *moved up* rather than down because the new subsystem scores (9, 9, 9,
8, 8, 8) are individually higher than the product's average, and the expert weights
"is there genuinely novel, portable, rare inspiration here" most heavily — and the
harness is rarer than the registry.

## 3. What the With-Agents Pass Saw That Without Was Blind To

These are net-new and could not have been seen without the meta-layer:

- **The actual agent loop.** Pass A's central indictment — "this is tool-plumbing,
  not an agent; there's no tool-call loop" — is resolved by `ship-loops.md`:
  `diagnose → plan-lock → fix → build → review → gate → commit → push → PR →
  gate-check → squash-merge → cleanup`, run end-to-end in one turn with a
  "do-not-pause checklist." The loop exists; it just operates on the *codebase*,
  not inside the product's chat panel.
- **The merge-gates poller** (`merge-gates.sh`, 1,518 lines) — re-deriving
  merge-readiness from GitHub's GraphQL API rather than trusting the agent's "all
  green," with the load-bearing detail that *the sanction is an exit code, never
  advisory `echo`* (the #1180 incident). This is the with-pass's marquee finding
  and has no analog in Pass A.
- **Delta-gated tiered lint + structured `DEVIATION(reason/owner/revisit)`** —
  gating the *derivative* not the *level* so a brownfield codebase can be held to
  taste without freezing. The with-pass calls this "the single highest-ROI idea
  here for an existing codebase."
- **The self-tightening flywheel** — gate escape → mandatory `### Preventing gate`
  field → new gate. Pass A explicitly listed "an eval harness / behavioural
  scoring" as a top gap; Pass B finds both the `subagent-eval` harness AND a
  mechanism that converts every production escape into a permanent new check.
- **Git-ref compare-and-swap plan-locks** — serverless distributed mutual exclusion
  for parallel agent sessions using only git's non-fast-forward rejection. Entirely
  invisible to a product-only read.
- **Capability-tag → per-harness adapter** — one `agents.md` spec driving Claude
  Code / Codex / Cursor. This is the with-pass's answer to portability, a concept
  Pass A never raised.
- **The governance charter** (`AI_POLICY.md`) — loop modes (`on`/`in`), the
  "escalate, don't assume" invariant, fail-safe-to-`in` — the *mental model* for
  bounded autonomy that Pass A had no window into.

Net: Pass B unlocked roughly six of the eight items Pass A had filed under "what a
serious agentic platform would add" — but located them in the harness, not the
product. That is the single most important cross-pass finding.

## 4. What the Without-Agents Pass Got Right That Survived

Pass A's product-level findings are **untouched and re-confirmed** by Pass B's §5,
often verbatim: the single `Dispatch` chokepoint, the `Source`-enum trust tagging,
the stable kebab-case `ErrorCode`/`ToWireJson` envelope, the registry-*is*-the-MCP-
surface derivation via `BuildJsonSchema()`, the MCP hardening (loopback-only,
DNS-rebind defence, bounded parse, token auth), and the sandboxed instruction-limited
Lua layer. Pass B adds nothing the product read got wrong.

More than survive — Pass A judged some things **more precisely** *because* it wasn't
distracted by the meta-layer:

- **The depth of the registry deep-dive.** Pass A §3.1–§3.4 and §4 dissect the
  C++14 portability constraint (`json_fwd.hpp`, `shared_ptr<json>` boxing because the
  header compiles into both MinGW-standalone and MSVC-under-Unreal), the shallow
  schema vocabulary ceiling (no nested objects, no `oneOf`, a `Json` param degrades
  to bare `{"type":"object"}`), the per-dispatch full-struct copy allocation, and
  the absence of machine-readable *result* schemas. Pass B's §5 compresses all of
  this to two sentences. For a programmer who wants to *port the registry*, Pass A is
  strictly more useful.
- **The streaming/threading analysis.** Pass A §3.7 — generation counters,
  per-turn cancel atoms, `MainThreadDispatcher` hand-off, 4 MiB bounded buffer,
  fail-closed provider rebuild, deferred-fetch context sentinel
  (`"__SMATCHET_DEFERRED__"`) — is genuinely copy-worthy engineering that Pass B
  reduces to "TSan coverage on the worker→UI hand-off." Pass A owns this terrain.
- **The sharpest single diagnosis in either report:** *the in-app assistant cannot
  call the tools.* `IAiClient::SendStreaming` is text-in/text-out; `AiChatRequest`
  has no `tools` field; the architecture "has all the pieces for an agent loop and
  wires exactly none of them together internally." Pass B *never restates this* —
  arguably a blind spot of the with pass (see §5). Pass A's focus is precisely what
  let it land that observation.

So the without pass is not merely "the part before the good part." It is the
authoritative product-engineering read, and on the one question both passes could
have answered — *can the product itself act?* — only Pass A answers it cleanly.

## 5. Contradictions & Tensions

The two passes mostly tell **two stories rather than overturn one verdict**, but
there are real tensions:

1. **Same code, different score (the registry: 9 → 7).** Not a factual
   contradiction — a framing one. Pass A scores the registry in absolute terms; Pass B
   scores it relative to the dazzling harness. A reader who only saw the numbers
   might wrongly infer the with-pass found a flaw. It didn't; it found a more
   impressive neighbor. This is the cleanest example of the meta-layer *changing the
   measuring stick* rather than the measured thing.

2. **The "missing agent loop" — resolved or dodged?** Pass A's thesis is "no agent
   loop." Pass B finds an agent loop — but a *different* one (a CI/ship loop over the
   repo, not a tool-call loop inside the product). The with pass **never explicitly
   reconciles this**: it does not return to Pass A's strongest point and say "the
   internal tool-call loop is still missing; the loop I found is orthogonal." A
   careful reader must do that reconciliation themselves. Both statements are true
   simultaneously — the product still can't act on its own tools; the *harness* can —
   but the with pass leaves the seam unstitched. That is a genuine blind spot: the
   with pass got so absorbed in the harness that it dropped Pass A's single best
   product critique.

3. **Persona drift in the with pass.** The brief positions this expert as a
   programmer mining *agentic infrastructure for inspiration.* Pass A stays tightly
   in-persona (reusable C++ patterns, "what I'd steal vs skip"). Pass B drifts toward
   *engineering-management / process critique* — ROI, ceremony tax, bus factor,
   maintenance liability, "a second product with no separate maintainer." This is
   valuable and honest, but it is partly a different reviewer. The infra-*builder*
   persona is best served by the portable mechanisms (§4 of the with pass); the long
   §6 critique is a software-org skeptic talking. Worth flagging as drift, even though
   the drift produces the report's most honest material.

4. **Overclaim check.** The with pass is mildly seduced by scale — it leads with
   raw counts (169 scripts, 56 bats suites, 118 self-tests, 2,013-line ledger, 30
   CI workflows) in a way that risks *equating volume with value.* To its credit it
   then spends all of §6 puncturing exactly that ("much of its sophistication is scar
   tissue," "ceremony with unprovable ROI," "do not clone the mass"). So the report
   self-corrects, but the opening inventory does some rhetorical heavy lifting the
   evidence later walks back. Pass A has the opposite tendency and is the more
   disciplined document line-for-line.

## 6. Critic's Verdict — Which Pass Is More Useful for This Persona

**For an agentic-infra builder, the with-agents pass is more useful and more
trustworthy — but only because of what this persona is.** For a generic C++ engineer
wanting a clean tool-registry, Pass A is the better artifact: deeper, more precise,
more directly portable. But the brief's persona is someone *building agent systems*,
and for that reader the meta-layer is not a sideshow — it is the main attraction. A
"gate, don't trust" merge poller whose sanction is an exit code, delta-gated lint with
expiring structured deviations, git-ref CAS plan-locks, and an escape→preventing-gate
flywheel are exactly the patterns this persona came for. Pass A simply does not contain
them. So the with pass wins on *relevance density* for the stated reader, even though
Pass A wins on *per-claim rigor.*

**Is the gap a sign the value is "front-loaded in docs" rather than in shipped code?**
This is the sharpest question, and the honest answer is *partly, but not as a
weakness.* The +1.0 delta comes almost entirely from the meta-layer, which is
predominantly **prose-and-scripts** (`AGENTS.md`, rule-docs, `.sh` pollers, CI YAML),
not C++ product code. A skeptic could say "the inspiration is in the README, not the
binary." But two things rescue it from being mere docs-ware:

1. **The harness is executable, not aspirational.** `merge-gates.sh` (1,518 lines),
   `lock-claim.sh`, `merge-watcher.py` (3,310 lines), 56 bats suites, seven CI
   workflows that *gate the harness itself* — these are running code with their own
   test suite, not a wish-list. The with pass verifies this by reading the scripts,
   not just the index (§2 Scope). The value is in *governance code*, which for an
   agentic-infra builder is the product.
2. **The product dogfoods the same discipline.** Pass A independently confirms the
   *product* embodies the same trust-boundary philosophy (single chokepoint, confirm-
   across-all-sources, consent+redaction on outbound AI). So the docs are not
   describing a system the code ignores; the two reinforce each other. That coherence
   is itself the strongest evidence the value is *not* hollow front-loading.

The fair verdict: the project's value for this persona is "front-loaded in
*infrastructure*" — which happens to be docs+scripts rather than C++ — and that is
appropriate, because agentic infrastructure *is* mostly the verification boundary, not
the model call. Where the skeptic is right: the with pass cannot prove the *ROI* of the
mass (it says so itself, repeatedly), and a chunk of the meta-layer is incident-specific
scar tissue (`merge-gates.md` as "incident archaeology") whose inspiration value is "port
the principle, skip the 1,400 lines of CodeRabbit-shape classification." So the honest
framing is: **the patterns are real and code-backed; the *quantity* is over-fit to one
maintainer's incident history.** Both passes, read together, make this clear; neither
alone does.

## 7. Synthesis — The Blended Bottom Line

This expert should walk away with a two-tier takeaway:

- **Tier 1 — the product (Pass A's domain).** Steal the `Command`-struct + single
  `Dispatch` chokepoint + derived schema/help, the canonical wire envelope with stable
  error codes, the "reach ≠ authority" confirm model, the small `IAiClient` strategy
  interface with its factory test-override seam, and the generation-counter / per-turn-
  cancel-atom streaming hand-off. Know the ceiling: shallow schema vocabulary, no
  result schemas, no reliability layer, and — the defining gap — *the in-product
  assistant cannot call its own tools.* That gap is real and the with pass never
  retracts it.

- **Tier 2 — the harness (Pass B's domain, and the bigger prize for this persona).**
  Port the gate-don't-trust merge poller (exit-code sanction, meant-to-block
  allow-list), delta-gated lint with expiring structured deviations, the escape →
  mandatory-preventing-gate flywheel, git-ref CAS plan-locks, the capability-tag
  harness adapter, prompt-size-as-a-gate, and the `AI_POLICY` charter with revocable
  loop modes and escalate-when-unvalidatable. Then *do not clone the mass*: most of the
  line count is scar tissue over-fit to one maintainer's CodeRabbit/Bugbot incidents,
  the autonomous trunk-merge it's built around still leaks at the tail, and the whole
  factory is a second product with a bus-factor of one.

The unifying insight that neither pass states alone but both imply together: **Smatchet
puts its trust-boundary discipline in two places — the product's `Dispatch` chokepoint
and the harness's merge gate — and they are the same idea at two scales** (one definition
of "who may do what, validated once, no per-source bypass"). For an agentic-infra builder
that is the most transferable lesson in the entire repository, and you need *both* passes
to see it.

**Blended overall: 8/10.** The with pass's 8.5 is the right number for the persona named
in the brief (the harness is the attraction and it is genuinely rare). I shave half a
point because (a) the score is partly inflated by scale the report itself can't justify
on ROI, (b) the with pass drops Pass A's single best product critique without
reconciling it, and (c) the inspiration is real but quantity-over-fit, so a clean-eyed
builder extracts ~9 patterns and discards the bulk. Eight is the score for "exceptional,
rare, code-backed patterns wrapped in a maintenance liability you must not copy whole" —
which is exactly what the two passes, read together, describe.
