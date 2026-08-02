# Smatchet Agentic Infrastructure — A Builder's Field Report (WITH agents.md)

*Surveyor stance: a programmer building AI-agent systems, mining Smatchet's
agentic-governance meta-layer for reusable patterns. Evidence cited inline by
file path.*

---

## 1. Executive Summary + Verdict

Smatchet is a C++/ImGui desktop app for issue trackers (Jira/Plane), but the
artifact worth studying here is not the product — it is the **agentic
development harness** wrapped around it: a single-maintainer's autonomous
software factory built on `AGENTS.md`, `AI_POLICY.md`, an `agents/` tree of 26
specialist subagent prompts, ~15 reusable skills, three saved Workflow
fan-outs, **169 automation scripts** (`agents/scripts/{core,project}`), **56
bats integration suites + 118 shell self-tests**, 19 extracted rule-docs
(`docs/agent-rules/`), 22 ADRs, a 2,013-line postmortem ledger, and a
30-workflow CI fleet whose newer members (`agentic-selftests`, `cr-finding-gate`,
`plan-lock-gate`, `coverage-gate`, `dup-scan`, `lock-staleness`, `locks-render`)
exist solely to police the harness itself.

**Verdict: extremely high inspiration value (8.5/10).** This is one of the most
complete worked examples of "the agent ships, but a gate — not the agent's
self-report — decides whether it merges" that you will find in the open. The
central thesis is sound and rare: *autonomy is cheap to grant and dangerous to
trust, so spend the engineering effort on the verification boundary, not the
agent's good intentions.* Concretely the standout, portable ideas are: the
**merge-gates poller** (`agents/scripts/core/merge-gates.sh`, 1,518 lines) that
re-derives merge-readiness from GitHub's API rather than believing the agent;
the **git-ref compare-and-swap plan-lock** for cross-session concurrency
(`lock-claim.sh`); the **capability-tag → per-harness tool adapter** that makes
one `agents.md` spec portable across Claude Code / Codex / Cursor; and the
**gate-escape → postmortem → preventing-gate → self-improvement** flywheel that
turns every production escape into a new gate.

The cost is equally real and worth naming up front: the meta-layer is enormous,
much of its sophistication is scar tissue from incidents a smaller team would
never hit, and a non-trivial fraction is ceremony whose ROI is unprovable. The
right way to consume Smatchet is as a **pattern catalog to port selectively**,
not a system to clone.

---

## 2. Scope & Method

This was a deep read of the actual prompts, scripts, and gate logic — not just
the index. Files read in full or substantially: `AGENTS.md` (157 lines,
navigation-only by design), `AI_POLICY.md`, `project.config.json`,
`agents/README.md`; the agent definitions `architect.md`, `code-review.md`,
`debug-detective.md`, `tracker-backend.md` (representative of design /
investigator / pause-loop / implementer classes); the rule-docs
`delegation.md`, `ship-loops.md`, `merge-gates.md`, `quality-pillars.md`,
`workflow-fleets.md`, `AGENT-VS-SKILL.md`, `subagent-eval.md`,
`capability-adapter.md`; `AGENT_SELF_IMPROVEMENT.md` + the
`gate-escape-postmortem` skill; the scripts `merge-gates.sh`, `lock-claim.sh`
headers; `.coderabbit.yaml`; and the CI workflows `agentic-selftests.yml`,
`cr-finding-gate.yml`, `plan-lock-gate.yml`. I also enumerated the script/test
inventory and skimmed the shipped product's agentic surfaces (MCP, command
registry, Lua, the `AiAssistant*` source tree).

I evaluated each subsystem against a builder's question: *would I port this, and
what breaks if I do?* — distinguishing genuinely novel/reusable mechanisms from
over-engineering, ceremony, and incident-specific brittleness.

---

## 3. The Agentic Architecture — How It All Fits

The system is a layered control plane. From the top:

**Governance charter (`AI_POLICY.md`).** The apex document, deliberately placed
*above* `AGENTS.md`. Its claims: humans own quality and cost; agent autonomy is
a *granted, revocable mode*; everything must be auditable. It defines **two loop
modes** selected by `SMATCHET_LOOP_MODE` — `on` (human-on-the-loop:
action-biased, commit/push/PR autonomously) and `in` (human-in-the-loop:
execute only within an approved plan, pause on anything the plan didn't settle).
The default lives in `project.config.json` § governance (currently `on`, with
`auto_merge: on`), with a fail-safe to `in` when config is unreadable. The
load-bearing invariant is **"escalate, don't assume"**: before acting the agent
must be able to *autonomously validate* the action (a gate/test/spec confirms
correctness, scope is authorized, cost is bounded); if it cannot, it escalates
via `AskUserQuestion` rather than guessing. This is pause-exception (6) and it
fires in *both* modes. Attribution is candid — the split is adapted from
Ghostty's `AI_POLICY.md`.

**Operating contract (`AGENTS.md`).** Capped at 150 lines and explicitly
*navigation-only* ("if a line accretes detail, move it to its linked section").
It is a dispatch table into the rule-docs, not a rulebook itself. Five operating
principles (autonomous-by-default · gate-don't-trust · delegate · plan-before-ship
· self-tighten) plus the Quality Pillars table, the lint enforcement
contract-card, and stubs that redirect to `docs/agent-rules/*`. This size
discipline is itself gated (see §4).

**Orchestrator + specialist subagents (`agents/`).** A primary orchestrator
loop delegates to read-only investigators (`architect`, `code-review`,
`security-review`, `perf-detective`, `spike-hunter`), read-edit implementers
(8 subsystem specialists: `tracker-backend`, `grid-engine`, `lua-binder`,
`mcp-toolsmith`, `offline-sync`, `unreal-bridge`, `p4-annotate`,
`command-system`, plus `ui-host`, `mechanic`), and maintenance agents
(`build-doctor`, `test-author`, `git-janitor`). Each carries YAML frontmatter
with a **closed capability-tag set**, `triggers:`, `complexity`, `read-only`
flag, `version:`, and `harness-hints.<harness>:` (e.g. `model: opus, effort:
high`). The split rationale (`delegation.md` § Why split) is **context
isolation as the primary token lever** — `build-doctor` never loads
`Source/Core` headers — bigger than per-model price differences.

**The agent output contract (`delegation.md` § Agent output contract).** Six
report classes (Investigator / Design / Diagnostic read-edit / Implementer /
Helper / Maintenance), each with required sections, all closing with
`## Outcome: <state>` (the telemetry key, one of
`applied|halted|failed|partial|aborted`), `## Session context append`, and
`## Self-improvement`. A `SubagentStop` hook parses these for token/tool-trace
telemetry. There's even a **durable finding-append rule**: long-running finders
must append each finding to `build/<run>/findings/<charter>.jsonl` the moment
it's confirmed, so a token-limit death loses only one in-flight finding (born
from a 2026-06-10 fleet that lost 4.1M tokens of confirmed-but-unpersisted
findings).

**Skills + workflows (`agents/_shared/`).** ~15 skills are the *deterministic
mechanics* extracted out of agents per the AGENT-VS-SKILL rubric; 3 saved
`Workflow` scripts are deterministic multi-agent fan-outs.

**The ship-loop (`ship-loops.md`).** The autonomous spine:
`diagnose → [seed plan-lock] → fix → build → [code-review pass] →
[pre-first-push gate] → commit → push → open PR → [gate-check] → squash-merge →
git-janitor cleanup → backlog entry`, run **end-to-end in one turn** with
clarifications batched once at the start. A "do-not-pause checklist" enumerates
the stages where an LLM reflexively over-asks ("should I poll?", "should I
merge?") and forbids it. Six explicit pause exceptions (debug-mode override,
destructive ops, cross-repo mutations, un-authorized actions, visual-validation,
cannot-validate/cost-unbounded).

**Merge gates (`merge-gates.sh` + `merge-gates.md`).** The "gate, don't trust"
heart (§4).

**Self-improvement loop (`AGENT_SELF_IMPROVEMENT.md` + postmortems).** Every
agent emits a `## Self-improvement` section; entries accrue as one-file-per-entry
under `docs/self-improvement/categories/<cat>/<date>-<slug>.md`; gate escapes
trigger blameless postmortems with a *mandatory* preventing-gate field (§4).

**Concurrency substrate (locks + worktrees).** One worktree per session;
git-ref compare-and-swap plan-locks; a three-layer enforcement (seed → contention
deny → server-side `plan-lock-gate.yml`); a `smatchet-merge-watcher` host daemon
(`merge-watcher.py`, 3,310 lines) that takes over post-PR merging (§6 risk).

The data flow is a closed loop: human prompt → (captured + redacted into PR
`## Intent`) → orchestrator → specialists (isolated context) → PR → gates →
merge → snapshot ledger → (on escape) postmortem → new gate → tighter loop.

---

## 4. Pattern Catalog — The Genuinely Reusable Ideas

### 4.1 Merge-gates poller: re-derive trust from the source of record
**What.** `merge-gates.sh` polls *four independent conditions* on a PR via one
GraphQL call before any squash-merge: (1) CI — every required check terminal-green
**plus a curated "meant-to-block" allow-list** of non-required checks
(`Coverage|Sanitizer|Perf PR-fast|Android security gate|Fuzz smoke|Intent
section`) that block exactly like required ones; (2) CodeRabbit verdict; (3) zero
unresolved non-bot human comment threads; (4) Cursor Bugbot inline findings.
**Where.** `agents/scripts/core/merge-gates.sh` (1,518 lines), spec in
`merge-gates.md`.
**Why valuable.** This is the canonical instantiation of *don't trust the
agent's "all green" claim — re-compute it from GitHub*. The allow-list closes the
#923 escape class (an agent merged past a red **non-required** Coverage check
because GitHub auto-merge only waits on *required* contexts). Critically, the
sanctioned merge is an **exit code, never advisory text** — the doc hammers
"never gate a merge on an `echo`" after the #1180 incident where a printed
"FAIL" couldn't stop an unconditional `gh pr merge --admin`. `safe-merge.sh` /
`safe-admin-merge.sh` wrap the merge so the green assertion *is* the gate.
**How to port.** Adopt the principle directly: any LLM with merge authority must
call a deterministic pre-merge poller whose pass is an exit code consumed by the
merge command, and that poller must check the conditions GitHub's native
auto-merge *doesn't* (advisory bot reviews, unresolved human threads, non-required
red checks). You do not need 1,518 lines — that size is scar tissue (§6).

### 4.2 Quality Pillars + tiered, delta-gated lint
**What.** Five north-star invariants (`quality-pillars.md`): perf ≤ 6.94 ms,
no UI freeze > 100 ms, never-crash, accessibility (aspirational), DRY. Below
them, an **enforcement contract-card** in `AGENTS.md` lists ~18 gated rule-ids
(`no-raw-new`, `no-glfw-in-core-headers`, `function-too-long` 120/200,
`app-controller-fan-in` ratchet, `duplication`, `bare-json-parse-untrusted`,
`agent-too-long`…) each scoped to a zone, **delta-gated vs `origin/develop`**
(only *new* violations fail; existing ones grandfathered), with a uniform escape
hatch: `SMATCHET_DEVIATION(rule=<id>; reason=…; owner=…; revisit=…)`.
**Where.** `AGENTS.md` § Enforcement contract-card; `dup_audit.py`,
`appcontroller_fan_in_audit.py`, `agent_size_audit.py`.
**Why valuable.** This is how you **codify taste into gates without freezing a
brownfield codebase.** Delta-gating is the key insight: a hard absolute cap on a
real codebase is un-shippable, so you gate the *derivative*, not the *level*.
The structured-deviation grammar makes exemptions cheap, auditable, and
expiring (`deviation-overdue` is itself a gate).
**How to port.** The delta-gate-vs-mainline + structured-deviation-with-owner-
and-revisit pattern ports to any lint/quality regime. It's the single highest-
ROL idea here for an existing codebase.

### 4.3 WARN-first → blocking graduation, gated on a measured FP rate
**What.** New gates ship as advisory WARN, then graduate to blocking only after
a measured false-positive rate (`dup_audit` graduated to blocking after FP < 10%
over ~20 PRs; coverage graduated 2026-06-04). `unused-symbol`,
`pr-numbered-temporal-comments`, `interface-doc` are currently WARN-first.
**Why valuable.** A gate that cries wolf gets `SKIP`-ed into irrelevance. Earning
trust before blocking is the discipline most homegrown lint regimes skip.
**How to port.** Trivially — add a WARN/BLOCK flag and a graduation criterion to
every new check. Pair with §4.7's eval idea for prompt-quality gates.

### 4.4 Capability-tag → per-harness tool adapter (portability from one spec)
**What.** Each agent declares closed capability tags (`semantic-code-search`,
`file-edit`, `shell`…); `capability-adapter.md` maps each tag to concrete tools
per harness (Claude Code `Edit`, Codex `apply_patch`, Cursor built-in, generic
`sed`). `setup-harness.sh` materializes flat `.claude/agents/*.md` junctions from
the canonical tree; `harness-hints.<harness>:` carries per-harness model routing.
**Where.** `docs/harness/capability-adapter.md`, `agents/README.md`.
**Why valuable.** One agents.md spec drives three harnesses without per-harness
prompt forks. Graceful degradation is explicit (no semantic search → text-search
fallback named inline). This is a clean abstraction boundary most "we use Claude
Code" shops never build, and it's the thing that future-proofs an agent fleet
against tool churn.
**How to port.** Adopt the capability-tag indirection even if you only target one
harness today — it decouples agent prose from tool names.

### 4.5 Git-ref compare-and-swap plan-locks (cross-session concurrency)
**What.** `lock-claim.sh` pushes a tiny commit holding `claim.json` to
`refs/locks/<slug>`. Atomicity is free: pushing to create an existing ref is
rejected non-fast-forward, so first-push-wins with no lock server. Three layers:
eager non-blocking *seed* from a plan's §Files-to-modify at ship-loop start;
*contention deny* when ≥2 sessions are live; server-side `plan-lock-gate.yml`
that reds a PR whose changed files overlap a different branch's write_set.
Auto-release via a `lock-slug:` line in the PR body matched by `lock-cleanup.yml`.
**Where.** `agents/scripts/core/lock-claim.sh`, `plan-lock-gate.yml`.
**Why valuable.** Distributed mutual exclusion for parallel agent sessions using
**only git's existing CAS semantics** — no Redis, no lock service, survives
restarts, fully auditable. The worktree-per-session rule
(`process-rules.md`) prevents the classic "sibling's `checkout` rug-pulls my
HEAD" failure.
**How to port.** If you run multiple concurrent agent sessions against one repo,
this is the cheapest correct concurrency primitive available. Port nearly
verbatim.

### 4.6 Gate-escape → postmortem → preventing-gate flywheel
**What.** A *gate escape* (anything that reached `develop` a gate should have
caught: a non-SUCCESS check at merge, an override label, a `Revert`, an overdue
deviation) auto-raises a SessionStart nudge (`postmortem-owed.sh`). The
`gate-escape-postmortem` skill runs a blameless RCA whose **mandatory
`### Preventing gate`** field names a concrete new gate — *the entry cannot
close without it*. That gate is filed as a normal self-improvement entry and
applied via the existing threshold loop. A merge-time **snapshot ledger**
(`merge-snapshots.jsonl`, 115 lines, ADR-0017) captures override-labels + red-
checks *at the merge-decision instant* — the only lossless record, since GitHub
overwrites `statusCheckRollup` on re-run.
**Where.** `gate-escape-postmortem/SKILL.md`, `postmortems.md` (2,013 lines),
`AGENT_SELF_IMPROVEMENT.md`.
**Why valuable.** This is a **self-tightening** system: every escape mechanically
produces a new gate, so the same class can't escape twice. The "incident finder
is separate from the applier, and there's no second apply-system" design avoids
the usual postmortem-rot where action items die in a doc.
**How to port.** Adopt the *mandatory preventing-gate field* + *escapes auto-
nudge* even without the rest. It's the discipline, not the tooling, that matters.

### 4.7 Eval-driven prompt edits (perf-gate pattern, one level up)
**What.** Prompt edits to agents with eval coverage (currently `code-review`
only) are scored base-vs-head over a curated case set; malformed artifact →
FAIL, quality regression → WARN (advisory until judge-vs-human calibration
exists; the calibrator *blocks* if the judge disagrees with humans beyond
tolerance).
**Where.** `docs/agent-rules/subagent-eval.md`, `docs/agent-eval/`,
`scripts/dev/agent-eval-*.py`.
**Why valuable.** It treats *agent decision quality* as a measurable regression
surface — "a prompt edit ships on data, not judgment" — mirroring the perf gate.
The honesty about *not* blocking until the judge is calibrated is exactly right.
**How to port.** Aspirational but directionally correct: as your agent fleet
matures, a small golden-case eval set per agent is how you stop prompt-edit
roulette. Start advisory.

### 4.8 Prompt/contract size discipline as a gate
**What.** `AGENTS.md` ≤ 150 lines (navigation-only), agent prompts ≤ 250
(soft-warn 150), extraction sinks (`docs/agent-rules`, skills) soft-warn-only
~400. Enforced by `agent_size_audit.py --diff` (delta-gated, grandfathered).
The AGENT-VS-SKILL rubric defines the reasoning/recipe seam: judgment stays in
the agent, verbatim mechanics extract to skills.
**Why valuable.** Fights the universal failure mode where a master prompt
accretes into an unreadable, un-followable wall. Making it a *gate* — not a
guideline — is what makes it stick.
**How to port.** Cap your system/agent prompts and gate the cap; extract recipes
to loadable skills. High ROI, low cost.

### 4.9 Intent capture: human prompt → PR `## Intent` (provenance seam)
**What.** A `UserPromptSubmit` hook appends each prompt — *pre-redacted* through
`redact-intent.py` (secrets, home-dir usernames, emails stripped) — to a
branch-keyed log; at PR time the orchestrator synthesizes one `## Intent` line.
A blocking `Intent section` gate enforces it.
**Why valuable.** Closes the traceability loop (which human ask produced this
diff?) with redaction as a first-class concern. The fallback discipline for
non-hook harnesses is spelled out.
**How to port.** Cheap and auditable; port if provenance matters.

---

## 5. The Shipped-Product Agentic Surfaces (brief)

The product itself is agent-shaped, which makes the harness dogfood-able.

- **Unified command registry** (`Command.h`, `CommandRegistry.h`,
  `docs/guides/cli.md`). One `Command` struct feeds **five front-ends — CLI, Palette
  (Ctrl+Shift+P), MCP, Lua `commands.invoke`, and the Unreal bridge**.
  `CommandRegistry` is a thread-safe `name→Command` map with alias table, fuzzy
  match, and a single `Dispatch` chokepoint that copies the handler under lock
  before invoking (reentrancy-safe for Lua recursion). `CommandContext`
  (`Command.h:148`) carries `App`, a `Source` enum (Cli/Palette/Mcp/Lua/Unreal/
  Internal), `ConfirmedDestructive`, `DryRun`, `TimeoutMs`. The structured
  envelope is `CommandResult`/`CommandError` (`Command.h:69–92`) with a stable
  kebab-case `ErrorCode` enum serialized by `ToWireJson` to
  `{ok, command, data?}` or `{ok:false, …, error:{code,message,suggestions}}`,
  and CLI exit codes map 1:1 to error codes. A uniform `RequiresExplicitConfirm`
  guard blocks any unconfirmed destructive call **across all five sources** — a
  textbook single-chokepoint trust boundary.
- **MCP server** (`Source/Plugins/Mcp/McpPlugin.cpp`, `docs/guides/mcp.md`). Runs
  in-process (feature-gated, off by default), HTTP + SSE, speaking **both
  JSON-RPC and a REST tools-call shape**. Tools are not hand-registered:
  `tools/list` enumerates the *entire* command registry with each command's
  `inputSchema` from `BuildJsonSchema()` — the registry *is* the MCP tool
  surface. Security posture (`McpPlugin.cpp:99,148–197`) is serious:
  **loopback-only by default, `X-Smatchet-Token` auth (require-token-on-loopback
  default-on, hardened in #1566/#1576), DNS-rebinding Host/Origin defence, 1 MiB
  bounded JSON parse, destructive calls blocked with `confirm-required` unless
  `__confirm:true`, `__dry_run` preview, every call audit-logged.** The `run_lua`
  tool sits behind a separate dangerous opt-in.
- **Lua automation** (`docs/guides/lua.md`, sol2 + Lua 5.3). Scripts register cell
  renderers, ImGui windows, context-menu actions, MCP tools, and call
  `commands.invoke`. **The sandbox removes `os`/`io`/`package`/`require`/
  `dofile`/`load`** (no file/shell access) and enforces **instruction-limit
  debug hooks** (100k for setup/action/MCP callbacks, 10k for per-cell
  renderers). Hot-path cost (~50–60× C++) is bounded by a record-replay model:
  Lua runs at most once per cell per refresh, C++ replays the recording every
  other frame.
- **Shipped AI assistant** — a genuine in-product LLM chat side panel
  (`SmatchetAiAssistantUi.cpp`, `AiAssistantController.cpp`) with streaming
  replies (`AiSseParser`/`AiNdjsonParser`), persisted history, per-turn
  model/effort overrides, and — notably — **a workspace-context disclosure
  consent gate before any provider POST** (`AiOutboundConsent`), plus
  `AiEndpointPolicy` sanitization, `AiErrorRedact`, and `AiLuaPromptRateLimit`.
  **Multi-provider** (OpenAI, Anthropic, DeepSeek, Ollama;
  `AiAssistantController.cpp:34–129`), with the system prompt layered from
  global/project `agents.md`. A separate Whisper plugin adds push-to-talk
  dictation that can auto-send into the assistant. The feature carries the same
  rigor as everything else — **TSan coverage on the streaming worker→UI hand-off**
  (#1567–#1570). (Caveat worth flagging: the default Anthropic model id in the
  catalog, `claude-sonnet-4-6` at `ConfigManager.h:313`, is not a real Anthropic
  model id — the catalog entry looks like a placeholder.)

The takeaway for a builder: the *product* models the same trust-boundary
discipline as the *harness* — one dispatch chokepoint with a uniform destructive-
confirm guard, a tokened/loopback-bounded MCP port with confirm/dry-run/audit, a
genuinely sandboxed + instruction-limited Lua layer, and consent + redaction on
outbound AI. The two reinforce each other, and the registry-as-MCP-surface +
destructive-confirm-across-all-sources pattern is itself portable.

---

## 6. Critique — Over-engineering, Ceremony, Brittleness

**The meta-layer is a second product with no separate maintainer.** 169 scripts,
56 bats suites, 118 self-tests, seven CI workflows *just to police the harness*,
a 2,013-line postmortem ledger. Every one of these is itself code that can break,
needs updating, and competes for the single maintainer's attention. The
`agentic-selftests.yml` lane exists because the harness's own test suite "gated
nowhere" — i.e. the governance system had already grown complex enough to need
governance. There's a real risk of **infinite regress**: gates guarding gates
guarding gates. A two-person team adopting this wholesale would spend more time
maintaining the factory than the product.

**`merge-gates.md` is incident archaeology, not a spec.** The CodeRabbit gate
section alone enumerates ~12 CR signal-shape outcomes (`STALE_RESOLVED`,
`NONE+size-skip`, `crReviewSkipped`, grace windows, the
`cr-out-of-band` + mandatory `cr-disposition:` pairing) each tagged with the PR
number that motivated it (#357, #408, #421-425, #923, #976, #980, #1124, #1271,
#1332, #1428…). This is impressive resilience *and* a maintenance liability: the
gate logic is now a fossil record of every way a specific external bot behaved
on a specific day. Much of it is **brittleness imported from depending on
CodeRabbit + Cursor Bugbot semantics** that those vendors can change without
notice — each change is a potential new escape and a new postmortem.

**LLM-with-merge-authority is the load-bearing risk.** The whole edifice exists
*because* an LLM can squash-merge to `develop` autonomously (`auto_merge: on`).
The gates are good, but the failure modes are sobering: the doc itself documents
multiple incidents where the agent merged past a real finding (the #1332
"auto-merge beats CodeRabbit" race, the #923 non-required-red escape, the #1428
stale-checkout-enforced-stale-gate-logic escape). Each was caught *after* shipping
and fixed with another gate. The honest read: **gates reduce but do not eliminate
the tail risk of autonomous merge**, and the tail is where the expensive
incidents live. The `MERGE_GATES_FRESHNESS=block` guard (the gate refuses to pass
if its own blob differs from `origin/develop`) is clever, but the fact that it
was *needed* shows how deep the rabbit hole goes.

**Cost and latency.** The ship-loop polls CI + 2 review bots at 60s intervals up
to 60 polls (default 1-hour budget) per PR; the merge-watcher daemon runs
continuously. The Quality-Pillar perf gates, sanitizer builds, coverage, and
dup-scan all run per-PR. For a solo maintainer with low review availability this
is the right trade (machine time for human time), but the **per-PR wall-clock and
token cost is high**, and the `workflow-fleets.md` doc is an entire essay on
fleets that died burning 4.1M tokens with zero durable output. Multi-agent
fan-out here is treated as genuinely dangerous, with a mandatory `--strict`
preflight and a `PreToolUse` hook that *blocks* an unpinned fan-out — appropriate,
but it tells you the failure modes are frequent and expensive.

**Ceremony with unprovable ROL.** The subagent-eval harness covers exactly one
agent (`code-review`) and is advisory. The `## Self-improvement` section on every
agent is "empty is the common case and fine" — so most of the time it's pure
overhead in the output contract. The six-class output contract, the progress-
marker protocol, the session-scratchpad append protocol, agent versioning — each
is defensible in isolation, but collectively they form a tax on every single
delegation whose aggregate benefit is asserted, not measured.

**What could collapse.** (a) Vendor drift — CodeRabbit or Bugbot changes its
comment format and a swath of gate logic silently mis-classifies. (b) The
single-maintainer bus factor — this system encodes one person's hard-won
judgment; it would be very hard for a successor to safely modify
`merge-gates.sh`. (c) Gate-logic staleness on the merge-watcher daemon (already a
documented escape class). (d) The self-improvement loop assumes escapes are rare
enough to postmortem each one — at higher throughput the postmortem queue could
itself become the bottleneck.

---

## 7. Scorecard

| Dimension | Score | Note |
|---|---|---|
| Delegation model (roles, output contract, capability tags) | **9/10** | Clean class taxonomy, context-isolation rationale, closed capability tags, harness-hints. Slightly heavy contract tax. |
| Ship-loop autonomy design | **8/10** | The do-not-pause checklist + 6 pause exceptions + loop modes are excellent; autonomy is well-bounded. Docked for the autonomous-merge tail risk it can't fully close. |
| Gate / quality enforcement | **9/10** | Delta-gated tiered lint + structured deviations + meant-to-block allow-list + exit-code-not-echo are best-in-class. The marquee strength. |
| Self-improvement loop | **9/10** | Mandatory preventing-gate, escape auto-detection, snapshot ledger, no-second-apply-system. Genuinely self-tightening. |
| Harness portability | **8/10** | Capability-tag adapter + setup-harness + degradation fallbacks. Real, though Claude Code is clearly the first-class citizen. |
| Concurrency model | **8/10** | Git-ref CAS locks + worktree-per-session + 3-layer enforcement is elegant and serverless. Complex to operate. |
| Product agentic surfaces | **7/10** | Unified registry chokepoint + tokened MCP + consented/redacted AI assistant; solid, dogfood-able, not novel. |
| **Overall inspiration value** | **8.5/10** | A rare, complete worked example of gate-don't-trust autonomy. Mine the patterns; don't clone the mass. |

---

## 8. What I'd Port vs What I'd Skip

**Port (high confidence):**
1. **Gate-don't-trust merge poller** with a *meant-to-block allow-list* and an
   *exit-code* (never echo) merge assertion. The single most important idea.
2. **Delta-gated tiered lint vs mainline + structured `DEVIATION(reason/owner/
   revisit)`** — codifies taste onto a brownfield codebase without freezing it.
3. **Gate-escape → mandatory-preventing-gate → self-improvement flywheel.** Cheap
   to adopt as discipline; compounding payoff.
4. **Capability-tag → per-harness adapter.** Decouples agent prose from tool
   names even on a single harness.
5. **Git-ref CAS plan-locks** for any multi-session concurrency. Serverless,
   auditable, correct.
6. **Prompt/contract size caps as gates** + the agent-vs-skill reasoning/recipe
   seam. Keeps prompts followable.
7. **AI_POLICY governance charter separated from the operating contract**, with
   the *escalate-when-unvalidatable* invariant and revocable loop modes. The
   right mental model for autonomy.
8. **WARN-first → blocking graduation** on every new gate.
9. **Intent capture with redaction** for prompt→diff provenance.

**Skip / defer (until scale justifies):**
1. **The full 1,518-line merge-gates.sh.** Port the *principle*; most of the
   line count is CodeRabbit/Bugbot incident-specific classification you won't
   share. Start with ~150 lines: required checks + non-required allow-list +
   unresolved-human-threads.
2. **The 169-script / 56-bats meta-harness mass.** Adopt incrementally; do not
   stand up seven harness-policing CI workflows on day one.
3. **The subagent-eval harness** until you have multiple mature, frequently-
   edited agents and a reason to distrust prompt edits. One-agent advisory
   coverage isn't yet pulling its weight.
4. **Per-delegation ceremony** (progress markers, session-scratchpad append,
   six-section output contract) — adopt the `## Outcome:` telemetry line and the
   `## Self-improvement` line; defer the rest until you feel the pain they solve.
5. **Autonomous merge to the trunk** (`auto_merge: on`) — keep a human on the
   final merge until your gate suite has earned the trust Smatchet's has (and
   note even Smatchet's repeatedly leaked). Auto-*open-PR* + auto-poll, manual
   merge, is the safer default.
6. **The Perforce dual-VCS layer + p4-gated ship-loop** — pure incidental
   complexity unless you actually run Perforce.

**Bottom line.** Smatchet proves that an LLM-driven software factory can be made
*auditable and self-tightening* by spending engineering effort on the
verification boundary instead of the agent's good behavior. Take the gates, the
delta-lint, the escape-flywheel, the capability adapter, and the governance
charter. Leave the incident-fossil mass and the autonomous trunk-merge until your
own gate suite has bled enough to deserve them.
