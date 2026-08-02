# Smatchet Evaluation — AAA Tools/Pipeline Tech Lead (WITH agents.md)

**Evaluator role:** Tech lead, tools/pipeline team, ~200-dev AAA studio (Perforce, Unreal + proprietary engine, existing Jira/ShotGrid trackers).
**Question:** Adopt / fork / lift-components / lift-methodology / pass — as a foundation for internal tools.
**Lens:** Both the codebase as an embeddable tooling substrate **and** the agentic-governance meta-layer as a portable blueprint for running an AI-augmented tools team.

---

## 1. Executive Summary + Verdict

**Verdict: LIFT — both components *and* methodology. Do not adopt wholesale; do not fork the whole tree; do not pass.**

Smatchet is a single-maintainer, prerelease C++14 desktop app (a tracker-grid client: Jira/GitHub/Linear/Plane) that happens to carry two genuinely studio-relevant assets that are *separable* from the app itself:

1. **A small set of liftable app components** — the Unified Command registry (`Source/Core/src/Commands`), the engine-agnostic ImGui core with a clean C-ABI Unreal bridge (`Source/UnrealPlugins/SmatchetImGuiPlugin/.../SmatchetImGuiHostC.h`), the multi-backend tracker abstraction (`Source/Core/include/ITracker*.h`), and real Perforce annotate/CL depth (`Source/Core/src/P4Annotate.cpp`). These are the parts a tools team could embed in an in-editor Unreal overlay or a CLI/MCP tooling backbone.

2. **A complete, portable AI-team operating system** — the `AGENTS.md` + `AI_POLICY.md` + `docs/agent-rules/**` + `agents/` roster + CI-gate constellation. This is, candidly, the **higher-value artifact for a tools lead**. It is a working, battle-tested (255 postmortems in `docs/self-improvement/postmortems.md`) methodology for running autonomous coding agents under hard merge gates, and `docs/PORTABILITY.md` + `project.config.json` make it explicitly liftable.

The codebase is too small, single-user, and desktop-scoped to *adopt* as a studio platform (no auth/SSO, no multi-user, single-user DPAPI secret model, ~140 KLOC first-party). But the engineering *rigor* is real and the methodology is the part I'd actually carry into my team. **Overall: 7.5/10** as a source of liftable assets; the methodology alone is a 9.

---

## 2. Scope & Method

I read, first-hand (no sub-agents):

- Governance layer: `AGENTS.md` (28 KB), `AI_POLICY.md`, `project.config.json` (+ schema), `docs/PORTABILITY.md`.
- Methodology docs: `docs/agent-rules/{delegation,ship-loops,merge-gates,quality-pillars,cpp-rules,workflow-orchestration}.md`; `docs/self-improvement/{AGENT_SELF_IMPROVEMENT,postmortems}.md`; `docs/harness/capability-adapter.md` + the four harness adapter dirs.
- Codebase substrate: `Source/Core/src/Commands` (registry, `Command.h`, `CommandRegistry.h`), the per-subsystem `AGENTS.md` leaves, `ITracker*.h` interfaces (8 role-split headers), tracker backends (GitHub/Jira/Linear/Plane `*Client.h`), `Source/UnrealPlugins/SmatchetImGuiPlugin` (C-ABI `SmatchetImGuiHostC.h`, `Build.cs`), `P4Annotate.cpp`, `Source/Plugins/Mcp`, the Lua bindings, `CMakeLists.txt` (126 KB), the `.github/workflows/*` set (29 workflows), `.coderabbit.yaml`, `SECURITY_AUDIT.md`.
- Metrics: ~140 KLOC first-party C++/H; 307 doctest `.cpp` + 56 `.bats` test files; 30 perf scenarios; 255 postmortem sections.

Method: pragmatic senior tools engineer. I separate **lift-the-component**, **lift-the-methodology**, and **skip**, and weigh everything against 200-dev-studio reality (Perforce-first, existing Jira/ShotGrid, in-editor tooling needs).

---

## 3. The Codebase as a Tooling Foundation

### 3.1 Embeddability & the engine bridge — strong

This is the most directly useful piece for an Unreal shop. The core is deliberately engine-agnostic, with a **dual render target**: GLFW/GL3 standalone (`SmatchetStandalone`) and **DX12-in-Unreal** (`SmatchetCore_DX12`, an `EXCLUDE_FROM_ALL` target compiled into the plugin). The bridge is a clean **C ABI** (`SmatchetImGuiHostC.h`): opaque `SmatchetImGuiHostHandle`, lifecycle/init/frame/input/command entry points, host-supplied callbacks (`SmatchetOpenUrlFn`, attachment viewer) and a renderer enum that already anticipates **PS5/Xbox** backends (`SMATCHET_RENDERER_BACKEND_PS5/XBOX`). The `Build.cs` wires it to UE 5.6 modules (Slate, RHI, RenderCore) and pins `IMGUI_USE_WCHAR32=1` to match the CMake `ImGuiLib`.

What this buys a tools team: a **proven pattern** for an in-editor ImGui overlay that shares one core between a standalone tool and an in-Unreal panel, with the no-GLFW-in-core-headers discipline enforced by a lint gate (`no-glfw-in-core-headers`, absolute-zero, `Source/Core/include/**`) so the DX12 build never breaks. That gate alone is a lesson worth stealing.

Caveat: the bridge is **DX12/Windows-first**. PS5/Xbox are enum stubs, not implementations. A studio on a proprietary console renderer would write its own backend behind the same C ABI — feasible, but it's a port, not a drop-in.

### 3.2 The Unified Command registry — the standout platform asset

`Source/Core/src/Commands` is the most studio-relevant code in the repo. **One `Command` struct feeds five frontends** (documented at `Command.h:4-12`): CLI subcommand, in-app palette, MCP `tools/call`, Lua `commands.invoke`, and the Unreal in-process bridge. The registry is thread-safe, copies the handler under-lock before invoking (reentrancy contract in `CommandRegistry.h`), returns a structured error envelope rather than throwing across the boundary, and feature-gates optional surfaces (ADR-0010). The subsystem `AGENTS.md` enforces **"one registry, no bypass"** — every front-end dispatches through `CommandRegistry::Dispatch`, and a binding reaching the underlying service directly is flagged.

For a tools backbone this is exactly the right shape: register a pipeline operation once, get it in the CLI, the in-editor palette, the MCP tool list (so an agent can drive it), and Lua scripting **for free**, with uniform audit-logging and a source-aware destructive-confirm predicate (`RequiresExplicitConfirm`). A studio "tools platform" usually grows five inconsistent entry points to the same operation; this design forecloses that. **This is the #1 lift-the-component candidate.**

### 3.3 Perforce depth — real, but VCS-of-record-agnostic

P4 shows up in two distinct roles. As a **product feature**: `P4Annotate.cpp` (467 lines) + `P4AnnotateParse.cpp` + `P4ClPreview`/`P4vLaunch` UI — a real `p4 annotate`/`describe`/changelist blame surface. As **agent infrastructure**: a dual-VCS topology (`AGENTS.md` § Dual-VCS) where git/GitHub is the ship-line and Perforce is an *opt-in local layer* for shelves, `+l` locks, and task streams (`SMATCHET_AGENT_VCS=p4`, ADR-0008). That's notable — most AI-coding harnesses are git-only, and this one has thought about Perforce-native agentic WIP, which is *directly* my studio's reality.

The honest read: the **annotate parser** is liftable as a starting point for a P4 blame tool, and the **p4-gated ship-loop** is a genuinely rare bit of prior art for running agents in a Perforce shop. But neither is deep P4 integration at studio scale (no streams config populated, `p4_streams: []`); it's annotate + shelve/lock primitives, not a pipeline-grade P4 client.

### 3.4 Extensibility — Lua/sol2, MCP, custom ImGui windows

Three extension surfaces, all routed through the command registry: **Lua** (sol2, ~50 binding sites across `AppController_LuaBindings*.cpp`, with `docs/guides/lua.md`), **MCP** (`Source/Plugins/Mcp`, JSON-RPC, with `docs/guides/mcp.md` — so the same commands an agent uses are scriptable by *external* agents), and custom ImGui windows. For TAs and tools-devs this is the right menu: script a pipeline check in Lua, expose it as an MCP tool for an agent, surface it in the palette. The feature-gating (`SMATCHET_WITH_*`) means a "light" build can ship without AI/MCP/Whisper, which matters for a locked-down studio deployment.

### 3.5 Multi-tracker backend abstraction — could we add ShotGrid?

Yes, and cheaply. The tracker surface is **ISP-split into role interfaces** (`ITrackerIssueReader`, `ITrackerIssueMutations`, `ITrackerActivity`, `ITrackerFieldCatalog`, `ITrackerCollaboration`, `ITrackerConnectivity`, `ITrackerBackendFactory`) rather than one fat interface, and four backends already implement it (GitHub, Jira, Linear, Plane). `ITrackerIssueReader.h` shows the pattern that makes a *new* backend cheap: optional capabilities (changed-since query, key-only fetch, existence probe) ship **backend-agnostic default implementations** that fall back to a full fetch, so a minimal ShotGrid/internal backend works correctly (if heavily) the day it's written, and you override hot paths later. Adding a ShotGrid or proprietary-tracker backend is a known, bounded exercise — implement the factory + reader/mutations, reuse the field-catalog and JQL-suggest plumbing. This is the second-strongest lift-the-component story.

### 3.6 Code quality to fork — high, with caveats

C++14-hard (deliberately, for Unreal compat — `string_view`/`optional`/`variant`/structured-bindings/`if constexpr` all *banned* and lint-enforced), RAII-mandatory (no raw `new`/`delete`, absolute-zero gate), clear subsystem modularity with per-directory `AGENTS.md` leaves, a layered include-DAG enforced by an `include-cycle` gate, and an `app-controller-fan-in` ratchet gate that caps coupling to the god-object. The CMake is 126 KB but principled (interface targets, dual-target exclusion, feature flags). 307 unit-test files + 56 bats + 30 perf scenarios + a `SECURITY_AUDIT.md` that found *no RCE* and characterizes the threat model as local/DoS. This is genuinely forkable C++ — better-disciplined than most studio tools code I've inherited.

Caveats: `AppController.cpp` is 2,835 lines (the fan-in gate is managing, not eliminating, a god-object), and the codebase is **AI-built** (see §7).

### 3.7 AAA-scale gaps — the disqualifiers for "adopt"

- **Single-user, local-desktop.** Secrets are per-user **DPAPI-encrypted** in a local config (`ConfigManager.cpp` — `github_pat`/Linear key with plaintext fallback). No server, no shared state, no multi-seat.
- **No auth/SSO/RBAC.** Grep for OAuth/SSO/SAML/RBAC over the public headers returns nothing relevant. There is no identity layer — it's a personal client holding a personal token.
- **No large-dataset story.** SQLite-backed local cache, streamed fetch with pagination caps; fine for one user's view, unproven against a studio's 100k-issue tracker with hundreds of concurrent clients.
- **Windows-first.** DX12 bridge, DPAPI, MSVC toolset pin (`14.38`). Cross-platform mobile work exists but the in-editor path is Windows.

None of these are *bugs*; they're scope. They are exactly why the answer is **lift**, not **adopt**: you take the command registry, the bridge pattern, and the tracker abstraction into a platform *you* build the multi-user/auth/scale layer around.

---

## 4. The Agentic Methodology as a Liftable Blueprint — the Novel Lens

This is where Smatchet stops being "a tracker app" and becomes interesting to a tools lead who wants to 10x a small team. The `AGENTS.md` system is a **complete operating model for an autonomous-agent tools team**, and it is the single most valuable thing in the repo for me.

### 4.1 The roster + delegation model

`agents/core/` holds 18 portable generic roles (architect, code-review, debug-detective, perf-detective, spike-hunter, build-doctor, security-review, test-author, git-janitor, several janitors). `agents/project/` holds 9 subsystem-bound specialists (tracker-backend, grid-engine, lua-binder, mcp-toolsmith, unreal-bridge, p4-annotate, command-system…). The **delegation packet** discipline (`delegation.md` §"Orchestrator delegation packet") is the real IP: before handing work to an agent, the orchestrator assembles a compact packet (write-set scope, inline context excerpts, exhaustive symbol inventory done *once* and passed down, invariant pre-resolution, a plan-lock pre-flight to avoid concurrent-write collisions). This is how you keep N parallel agents from stepping on each other — and it's reified in scripts (`locks-show.sh`, `lock-claim.sh`) and ref-based locks, not just prose. For a tools team running agents at scale, this packet pattern is directly transplantable.

### 4.2 The autonomous ship-loop

`ship-loops.md` defines a one-turn `diagnose → fix → build → code-review → push → open PR → gate-check → squash-merge → cleanup` loop with an explicit **do-not-pause checklist** (don't ask "should I poll CI?", "should I address CR findings?", "should I merge?"). The point — stated plainly — is that drip-stepping every stage creates N round-trips for a task the user already authorized. The governance charter (`AI_POLICY.md`) bounds this with **two loop modes** (`on` = action-biased human-on-the-loop; `in` = plan-gated human-in-the-loop) and an **escalate-when-unvalidatable** invariant. This is a mature answer to the central question of running agents: *how much rope, and where's the stop?* Most studios have no articulated answer; this is one I could adapt on day one.

### 4.3 "Gate, don't trust" merge gates

The merge-gate poller (`merge-gates.sh`, `merge-gates.md`) checks CI + CodeRabbit + unresolved-comments + Bugbot via one GraphQL call before any squash-merge, with a hard rule: **never merge past ANY red check, required or not**. The sophistication here is earned from incident — e.g. the postmortem for PRs #1237/#1232/#1227/#1220/#1198 where non-required-but-blocking checks (`Sanitizer`, `Coverage`) were *in-flight* at merge time and slipped through a pending-count blind spot; the fix binds a `$blocking` set and counts pending over it. That's exactly the class of failure an AI-merge pipeline hits, and they've already paid for the lesson. The `safe-merge.sh`-not-bare-`--auto` rule (GitHub auto-merge waits only on *required* contexts, so it can merge before CodeRabbit even posts) is another scar I'd inherit gladly.

### 4.4 Quality-pillar lint enforcement

Five pillars (Performance ≤6.94ms, UI-never-freezes, Never-crash, Accessibility, DRY) with a **tiered, delta-gated, grandfathered** lint system: rules fire only on *new* violations vs `origin/develop`, existing violators are baselined, and `SMATCHET_DEVIATION(rule=…; reason=…; owner=…; revisit=…)` is a cheap, auditable escape hatch. Strict zones (Tracker/Sync/Persistence/Config/Commands/Mcp) vs light zones (Ui/Standalone). This is a genuinely good model for introducing rigor into a *legacy* studio codebase without a boil-the-ocean cleanup — delta-gating means you ratchet quality on new code while grandfathering the mountain you inherited. I would steal this pattern verbatim for a brownfield tools repo.

### 4.5 The self-improvement postmortem loop

**255 postmortem sections** in `postmortems.md`, each a blameless RCA whose *mandatory* `### Preventing gate` names a new automated gate. `postmortem-owed.sh` raises a SessionStart nudge whenever something shipped that a gate should have caught (an override label, a red check at merge, a revert). This is the flywheel: every escape becomes a new gate, so the system tightens itself. The volume and specificity are the strongest signal in the whole repo that this is a *lived* methodology, not aspirational documentation.

### 4.6 Is it 10x, or over-engineered ceremony?

Both, depending on team size. For a **2-4 person tools team running agents heavily**, this is plausibly a 10x multiplier: the gates let you trust autonomous merges, the delegation packets let you parallelize, and the postmortem loop compounds. For a **200-dev studio with existing Jira/ShotGrid/Perforce process**, lifting the *whole* ceremony would collide with established review culture and is overkill. The right move is **selective lift**: the delta-gated lint model, the merge-gate "never past red," the postmortem-owed flywheel, and the loop-mode charter. Skip the ref-based plan-locks and the harness-adapter machinery unless you actually run multi-agent fan-out.

---

## 5. Governance Portability into Our Studio

`docs/PORTABILITY.md` makes an explicit claim: the framework is **liftable via `project.config.json`** — "coupling to *this* project is values, not design." The portable tree (`agents/core/`, `agents/_shared/`, `docs/agent-rules/`, `docs/harness/`, the self-improvement framework, generic scripts, `AGENTS.md` shape) is copy-as-is; you rewrite *one file*. A `test-portable-purity.sh` guard enforces that portable files never hardcode a project literal, so the boundary can't silently rot.

How real is this? **Substantially real, with effort.** `project.config.json` parameterizes everything that matters: build presets/targets, the 6.94ms perf budget, lint zones + rule ids, VCS topology (`primary: git`, `optional_layer: p4`), branch-protection required contexts, coverage threshold, visual-validation globs, the subsystem→agent map, agent size budgets, and the governance defaults (`loop_mode: on`, `auto_merge: on`). A reuser's checklist (PORTABILITY §"Extraction checklist") is concrete: copy portable tree → rewrite config → `setup-harness.sh` → author `agents/project/` → run purity gate → verify discovery on each harness.

**Harness adapters** (`docs/harness/capability-adapter.md`) are the genuinely portable part: each agent declares capability *tags* (`semantic-code-search`, `file-edit`, `shell`…) and a table maps tags→tools per harness (Claude Code / Codex / Cursor / Aider / pi / generic). My studio runs heterogeneous tooling; an agent roster that isn't hard-wired to one vendor's tool names is a real portability win.

**The honest friction for *my* studio:** the gate scripts assume **GitHub** (GraphQL poller, CodeRabbit, Bugbot, `gh` CLI). My ship-line is Perforce + (likely) an internal review tool. The `vcs.optional_layer: p4` + p4-gated ship-loop prove they've *thought* about Perforce-primary, but the merge-gate machinery would need a real Perforce/Swarm adapter — that's the largest porting cost. The *methodology* (delta-gated lint, postmortem flywheel, loop-mode charter, delegation packets) ports cleanly; the *gate plumbing* is GitHub-shaped and needs an adapter layer.

---

## 6. What the Meta-Layer Signals About Forkability/Maintainability

The agentic layer is a strong **proxy for code quality and maintainability** — which is the question that matters if we'd fork. Signals:

- **Discipline is enforced, not exhorted.** The lint gates, the include-cycle DAG, the fan-in ratchet, the comment-noise gates, the C++14-hard ban-list — all are *mechanized*. Code that survives this regime is unusually consistent.
- **The postmortem ledger is an honest defect-history.** 255 entries of "here's what broke and the gate we added" is the kind of artifact you almost never get with an acquisition; it's a free map of the codebase's sharp edges.
- **ADRs (22) document decisions**, including the awkward solo-maintainer ones (ADR-0013 solo-no-required-review) — they don't hide the single-maintainer reality.
- **Security was actually audited** (`SECURITY_AUDIT.md`, 79 KB, scoped, with a stated threat model and a "no RCE found" conclusion).

Net: the meta-layer signals a codebase that is **more forkable than its single-maintainer status would suggest**, because the rigor is structural rather than personal. A new team picking it up inherits the gates, and the gates encode the maintainer's hard-won judgment.

---

## 7. Risks

- **Bus factor = 1.** This is a solo-maintained prerelease project (ADR-0013, `AI_POLICY.md` § Scope: "solo-maintained"). The governance is designed to *survive* that (auditable trail, gates encode judgment), but if we adopt rather than lift, we own a single-author codebase with no second human who's read it end to end.
- **AI-built code we'd inherit.** The commit log and the entire methodology make clear the code is substantially agent-authored. The gates are precisely the mitigation — but forking means trusting that the gates caught what matters. The 255 postmortems are reassuring *and* a reminder of how many escapes occurred before each gate existed.
- **Complexity / ceremony cost.** The methodology is dense. `AGENTS.md` alone is 28 KB of tightly cross-referenced rules; onboarding a human onto the *governance* (not the code) is a real cost. For a studio with existing process, importing all of it would be net-negative; importing selectively is the only sane path.
- **GitHub coupling in the gate layer.** As above — the highest porting cost for a Perforce shop sits in the merge-gate plumbing.
- **Perf budget is partly aspirational.** The 6.94ms target: the **relative** regression gate (mean_delta_pct 10%, p99 ≤10ms) is **armed** in CI (`perf-pr-fast.yml`, baselines committed), but the **absolute** mean ceiling (`mean_abs_ceiling_ms`) ships **`null`/DISABLED** in `docs/perf/regression-policy.json` pending calibration. So "≤6.94ms enforced" is *enforced as no-regression-from-baseline*, not as a hard absolute floor. Honest, documented — but don't quote 6.94ms as a guaranteed enforced ceiling.

---

## 8. Scorecard (/10)

| Dimension | Score | Notes |
|---|---|---|
| Embeddability / engine bridge | 8 | Clean C-ABI dual-target (GLFW + DX12-in-Unreal); PS5/Xbox are enum stubs; Windows-first. |
| Command-registry platform value | 9 | One struct → CLI/palette/MCP/Lua/Unreal, no-bypass-enforced, structured envelope. Best lift. |
| Perforce integration | 6 | Real annotate/CL blame + p4-gated agent ship-loop (rare prior art), but not pipeline-scale; streams unconfigured. |
| Extensibility (Lua/MCP/ImGui) | 8 | Three surfaces, all registry-routed; feature-gated; well-guided. |
| Code quality / forkability | 8 | Disciplined C++14, RAII-enforced, layered, audited; god-object `AppController` managed not solved; AI-built. |
| AAA-scale readiness | 3 | Single-user, local DPAPI secrets, no auth/SSO/RBAC, no multi-seat, unproven at large datasets. |
| Agentic-methodology value-to-us | 9 | Delta-gated lint, "never-past-red" gates, postmortem flywheel, loop-mode charter, delegation packets — directly liftable. |
| **Overall** | **7.5** | Lift components + methodology; don't adopt/fork/pass. |

---

## 9. What We'd Lift vs Adopt vs Skip

### Lift — app components
1. **The Unified Command registry** (`Source/Core/src/Commands`) — as the architectural template for our tools backbone: register once, surface in CLI/palette/MCP/Lua. Highest-value code lift.
2. **The C-ABI ImGui-in-Unreal bridge pattern** (`SmatchetImGuiHostC.h` + `Build.cs` + dual-target CMake) — for an in-editor overlay sharing one core with a standalone tool. Reimplement the renderer for our console backend behind the same ABI.
3. **The ISP-split tracker abstraction** (`ITracker*.h`) — as the template for a backend layer if we build a tracker bridge; add a ShotGrid/internal backend the documented way (default-impl optional capabilities, override hot paths).
4. **The P4 annotate parser** (`P4AnnotateParse.cpp`) — as a starting point for a P4 blame tool, not a finished one.

### Lift — methodology (the higher-value haul)
1. **Delta-gated, grandfathered lint with `DEVIATION` escapes** — to introduce rigor into our brownfield tools repos without a boil-the-ocean cleanup.
2. **"Never merge past any red check" + `safe-merge` discipline** — adapted to our review tool; the in-flight-non-required-check blind-spot lesson is pre-paid.
3. **The postmortem-owed self-improvement flywheel** — every escape mints a new gate; `postmortem-owed.sh`-style nudge.
4. **The loop-mode governance charter** (`AI_POLICY.md`) — `on`/`in` modes + escalate-when-unvalidatable, as our explicit "how much autonomy, where's the stop" policy.
5. **The delegation-packet discipline** — if/when we run multi-agent fan-out.

### Adopt (as-is, lightly)
- `project.config.json` + `PORTABILITY.md` extraction model — as the *shape* for our own portable-governance repo, even if we rewrite every value.
- The capability-tag harness-adapter table — so our agent roster isn't vendor-locked.

### Skip
- The app as a product (it's a personal tracker client, not a studio tool).
- The GitHub-specific gate *plumbing* (GraphQL poller, CodeRabbit/Bugbot wiring) — replace with a Perforce/Swarm/internal-review adapter.
- Ref-based plan-locks and the full multi-harness machinery unless we actually run concurrent agents at scale.
- Adopting/forking the whole tree (bus-factor-1, AAA-scale gaps).

**Bottom line for a 200-dev studio:** the app gives us three or four good *components* to embed; the agentic layer gives us a *playbook* for running an AI-augmented tools team that is more valuable than the app and more portable than its GitHub plumbing suggests. Lift both, build the multi-user/auth/scale and Perforce-gate layers ourselves, and treat the 255-postmortem ledger as the free risk map it is.
