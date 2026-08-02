# Smatchet Evaluation — Indie Game Studio CEO Lens (WITH agents.md)

*Prepared for: CEO, 15-person indie game studio. Date: 2026-06-30. Decision sought: Adopt / Pilot / Pass on Smatchet as an internal tool, plus a read on the strategic/methodology opportunity its AI-native build represents.*

---

## 1. Executive Summary

**Decision: PILOT (narrow, time-boxed) — do not Adopt studio-wide yet, do not Pass.**

**One-line take on the strategic opportunity:** The governance edifice around Smatchet is the genuinely valuable asset here — a working, MIT-licensed template for running an AI-agent development fleet under human authority — and it is worth more to us as a *methodology to study and selectively copy* than the issue-tracker is as a product to deploy.

Smatchet is a high-performance, engine-agnostic productivity tool: a unified issue-tracker client (Jira, Plane.so, GitHub Issues behind one `ITrackerBackend` interface), with native Perforce annotate, SQLite offline caching, a Lua automation runtime, an MCP server, an AI assistant side-panel, a 56+ command unified registry feeding CLI/palette/MCP/Lua, and — uniquely — the ability to run *embedded inside the Unreal Engine editor* via DX12. For a Perforce-and-Unreal game studio, that feature set lands squarely on real pain. The problem-solution fit is high.

The catch is everything around the product. Smatchet is ~218K lines of C++14, built and maintained almost entirely by **autonomous AI agents** under an elaborate governance system, by what `git shortlog` shows to be **one human author** (Alexandros Konstantonis, 49 of 50 commits; the other is Dependabot). It is **prerelease** (no tags, no releases, no prebuilt binaries), **Windows-first** (MSVC/Clang + Ninja + a long CLI-tool chain to build), and offers **no support, no SLA, and no community**. The same AI-native machinery that lets one person ship ~1,576 PRs of disciplined, gated, sanitized code is also the single largest adoption risk: if the founder stops, who maintains 218K LOC and ~270 gate/automation scripts that assume an agent fleet to operate them?

That tension — exceptional engineering rigor wrapped around extreme key-person risk — is why the answer is *pilot*, not adopt or pass.

---

## 2. Scope & Method

All findings below are evidence-grounded from the repository at `/home/user/Smatchet`. I (with my tech lead) read or sampled:

- **Product surface:** `README.md`, `docs/guides/cli.md`, `docs/guides/mcp.md`, `docs/guides/lua.md`, `BUILD.md`, `Source/` tree (685 first-party `.cpp/.h` files; standalone, Unreal plugin, mobile Android, core, plugins).
- **Governance meta-layer:** root `AGENTS.md` (28 KB rulebook), `AI_POLICY.md` (human-authority charter), the `agents/` tree (26 specialist agent prompts under `agents/core/` and `agents/project/`), `docs/agent-rules/**` (ship-loops, merge-gates, quality-pillars, delegation, process-rules), `docs/harness/**` (Claude Code / Codex / Cursor adapters), `docs/self-improvement/**` (255-entry postmortem ledger, self-improvement framework), `docs/agent-eval/**` (calibration/scoring policy for grading the agents themselves), `project.config.json`, `.coderabbit.yaml`, and all 27 CI workflows.
- **Licensing & security:** `LICENSE` (MIT), `THIRD_PARTY_LICENSES.md`, `SECURITY_AUDIT.md` (79 KB).
- **History:** `git shortlog -sn --all`, commit log, PR-number range.

**Quantified scale of the edifice (load-bearing for the risk read):**
- 218,170 LOC across 1,027 C++ files (1,000+ counting all headers); 685 first-party `Source/` files.
- **307 test `.cpp` files**; **307 Python/shell scripts** repo-wide (177 under `agents/scripts/` alone — the "~270 gate scripts" framing is real).
- **27 CI workflow files** (build/test, coverage gate, two sanitizer lanes ASAN+UBSan, TSan nightly, perf full + perf PR-fast, fuzz smoke, CodeQL, dup-scan, doc-validation, shell-lint, mobile security, plan-lock-gate, and more).
- **255 gate-escape postmortems** — an append-only blameless ledger where every escape produces a *new gate*.
- Git history is squashed (50 local commits) but PR references run up to **#1576**, confirming ~1,500+ merged PRs of throughput in roughly the project's lifetime (first commit 2026-06-21; this is an extraordinarily fast cadence, consistent with an agent fleet).

---

## 3. Value Proposition vs Our Pain

Our studio's day-to-day tooling pain, scored against what Smatchet actually does:

| Our pain | Smatchet's answer | Fit |
|---|---|---|
| Issues split across Jira + GitHub; context-switching tax | One client, `ITrackerBackend` abstraction over Jira / Plane.so / GitHub Issues; per-backend views, fields, offline drafting | **Strong** |
| Perforce is our source of truth; tooling rarely integrates it | Native P4 annotate with syntax highlighting in-app | **Strong** (rare) |
| Artists/designers live in Unreal; won't alt-tab to a browser tracker | Runs *embedded in the Unreal editor* (DX12) — issues where the work happens | **Strong / differentiated** |
| Slow tracker UIs, flaky on poor connections | SQLite local cache, near-instant loads, offline capability | **Strong** |
| Want AI assist without leaking data to random SaaS | AI side-panel with pluggable providers (OpenAI/Anthropic/Ollama/DeepSeek), DPAPI-protected keys, local Whisper dictation option; MCP server to expose tooling to our own agents | **Good** |
| Scriptable automation for our pipeline | Lua 5.3 runtime + 56-command registry callable from CLI/MCP/Lua/palette | **Good** |

The **Unreal-embedded, Perforce-aware, multi-tracker** combination is genuinely unusual. Most trackers are SaaS web apps; almost none live inside the Unreal editor or speak P4 natively. For a Perforce-on-Unreal shop, this is the differentiating hook, and it maps onto real friction we feel weekly.

**Caveats on the value:** it is single-user-desktop, not a team server — there's no hosted backend, no shared state beyond what the upstream trackers already provide; Smatchet is a *client*, so it doesn't replace Jira/GitHub, it overlays them. The mobile Android port exists but is clearly early (P1.x slices still landing). Localization is English/French only. None of this is disqualifying for a desktop power-tool, but it sets expectations: this is a developer/lead productivity client, not a studio-wide PM platform.

---

## 4. Total Cost of Ownership

**License cost: $0.** MIT. No per-seat fees, ever. This is the headline TCO win and it is real.

**Acquisition/build cost (the hidden tax):** There are **no prebuilt binaries and no releases**. Every seat is a from-source CMake build. Prerequisites (from `BUILD.md`): CMake 3.24+, Ninja, Git for Windows (with a PATH ordering gotcha vs the WSL `bash.exe` launcher), and **either** Visual Studio 2022 (MSVC, pinned toolset 14.38) **or** Clang/LLVM via `clang-cl`. The dev/agent scripts additionally want `gh`, `jq`, Python 3.11+, clang-format/clang-tidy/cppcheck, `bats`, and `shellcheck`. FetchContent pulls all third-party libs automatically (ImGui, SQLiteCpp, cpr, nlohmann/json, sol2, Lua 5.3.6, etc.), which is good — zero manual dependency downloads — but the first configure-and-build on a clean Windows box is a real afternoon, not a `winget install`.

**Estimated internal costs (rough, our rates):**
- *Initial build/validation spike:* 1 senior engineer × ~3–5 days to get a clean build, sign it, and smoke-test against our Jira + GitHub + Perforce. Call it **$3–6K**.
- *Per-seat rollout:* Without a signed installer we maintain, each new seat is friction. We'd need to produce our own signed build artifact (the repo ships a signing guide and installer smoke-test guide, so the path exists) — another **~2–3 days** of one-time pipeline work, then near-zero marginal per seat.
- *Ongoing maintenance:* This is the variable that dominates TCO. We either (a) pin a known-good commit and treat it as a frozen internal tool — low ongoing cost but we inherit any unpatched bugs/security issues — or (b) track `develop` and periodically re-validate against an extremely fast-moving, agent-driven branch with no stability tags — higher cost, ongoing.

**Support/SLA cost: there is none.** No vendor, no paid support tier, no community forum, one maintainer. If it breaks on our config, we fix it or we wait. Budget for self-support.

**Security-handling TCO:** This tool touches **Perforce and Git credentials and tracker API tokens**. Keys are DPAPI-protected on Windows, and there is a serious 79 KB `SECURITY_AUDIT.md`. That audit is reassuring on *intent and process* but its provenance is itself AI (41 Opus agents auditing + skeptic re-verification) — see §6. We would want our own security pass before letting it hold production P4/Git credentials, which is real cost.

**Net TCO read:** Acquisition is cheap-ish but not free in engineering time; license is free forever; the dominant, uncertain cost is **self-maintenance of an orphan-able codebase**. For a 15-person studio with a tight budget and no spare platform engineer, that maintenance exposure is the line item to fear.

---

## 5. Build vs Buy vs Adopt vs Paid Alternatives

**Build it ourselves:** Re-implementing even the core (multi-tracker client + P4 annotate + Unreal-embedded UI + offline cache) is easily 6–18 engineer-months. For a 15-person studio that is absurd opportunity cost — that's a shipped game feature or two. **Building is off the table.**

**Buy/SaaS alternatives:**
- *Jira / Linear / GitHub Projects:* mature, supported, hosted — but **none embed in Unreal or speak Perforce natively**, which is exactly our differentiated pain. They solve tracking, not the alt-tab-out-of-the-editor problem.
- *Perforce's own Helix Swarm / P4 tooling:* solves P4 review but not unified cross-tracker issues or in-editor presence.
- *Unreal marketplace plugins:* fragmented, mostly per-tracker, none with this breadth, and many are paid + unsupported themselves.

So the *capability* Smatchet offers is not readily buyable. The honest competitive framing: paid SaaS gives us **support and stability** we lack with Smatchet; Smatchet gives us **a capability (Unreal-embedded P4-aware multi-tracker) that no paid product offers**, at $0 license, in exchange for owning the maintenance.

**Adopt (pin a build):** The pragmatic middle path. Take a vetted commit, build it once, freeze it, and use it as a power-tool for our leads/programmers who live in Unreal+P4. Low commitment, captures the differentiated value, caps the risk. This is what the Pilot recommendation operationalizes.

---

## 6. Risk Assessment — including AI-built / bus-factor / key-person risk

**Key-person / bus-factor risk: SEVERE, and it is the deciding risk.** `git shortlog -sn --all` shows **one human** behind 218K LOC, ~270 scripts, 27 CI workflows, and a self-modifying governance system. The maintainer is a solo founder driving an AI fleet. If they stop — burnout, hire, lost interest, life — we inherit a codebase whose *operating model assumes the fleet*. The gates, postmortems, and self-improvement loop are tuned for agents to run them; a conventional human team can read the C++ but would struggle to keep the elaborate harness alive. This is not normal OSS bus-factor (where a community can pick up the pieces); there is **no community** (the AI_POLICY explicitly notes "no outside human contributors while solo").

**AI-built provenance — cuts both ways, honestly:**
- *Reassuring:* The discipline is real and verifiable, not theater. Five enforced "Quality Pillars" (perf ≤6.94 ms/frame, no UI freeze >100 ms, never-crash/sanitizer-clean, accessibility, DRY) are backed by *actual* CI lanes (ASAN, UBSan, TSan, perf budgets, fuzz, dup-scan, coverage). The 255-entry postmortem ledger shows a genuine "gate, don't trust" feedback loop — e.g. PR #1566 merged past a CANCELLED perf check via native-merge, and the response was a new branch-protection rule + a detection refinement, not a hand-wave. The strict C++14 contract (banned `optional`/`variant`/structured bindings for Unreal compat), RAII-only (`no-raw-new` gate), and the `bare-json-parse-untrusted` ingress gate are sophisticated, defensible engineering calls. This is *better* governed than most human teams' code.
- *Worrying:* The security audit and much of the verification are themselves AI-produced. "0 of 33 findings refuted" by a skeptic agent is a good signal but not an independent human/third-party audit. The governance documents are dense, self-referential, and occasionally read as an end in themselves — the system spends enormous energy governing its own process. For a non-AI-native team, that machinery is **opaque and fragile**: break one assumption (e.g. the agent that triages CodeRabbit findings) and the merge pipeline stalls in ways a human can't easily debug.

**Security risk of the tool itself:** It handles P4/Git/tracker credentials. DPAPI storage + a serious audit is good hygiene; prerelease status + no third-party audit + no patch SLA is the offsetting concern. Mitigatable with our own review and least-privilege tokens.

**Maturity/stability risk:** No releases, no tags, no semantic-version stability promise; `develop` moves at agent speed (~1,500 PRs). Anything we pin is a moving target if we want fixes.

**Lock-in risk: LOW.** MIT, vendored deps via FetchContent, standard C++14, clean `ITrackerBackend` seam. If we adopt and it's abandoned, we own a buildable snapshot. That genuinely caps downside.

---

## 7. The Strategic Signal — is the AI-native model a reason to trust+bet, and could we adopt the methodology?

This is where the evaluation gets interesting, and where the *real* opportunity sits.

**Is the heavy governance evidence of a project to trust/bet on — or an over-engineered solo experiment?** Honestly, **both at once.** The throughput (1,500+ gated PRs), the breadth (standalone + Unreal + Android mobile + MCP + Lua), and the *demonstrated* self-correction (postmortem→new-gate loop with 255 real entries) are evidence of a credible, fast-moving, well-tested system — not vaporware. A solo experiment that produces sanitizer-clean, perf-budgeted, fuzzed C++ at this cadence is a serious proof point for AI-native development. **But** the same evidence — a one-person edifice of 270 scripts whose value depends on continuously feeding an agent fleet — is exactly what makes it unbettable as a *dependency*. We would not bet the studio's tooling roadmap on a single founder we can't hire or contract for support.

**The methodology is the asset.** Strip away the tracker, and what Smatchet demonstrates is a **reusable, MIT-licensed blueprint for governing AI agents that write production code**:
- A clean two-layer split: `AI_POLICY.md` (the human-authority charter: humans own quality+cost, autonomy is granted/revocable, two loop modes — human-*on*-the-loop vs human-*in*-the-loop, escalate-when-you-can't-validate) above `AGENTS.md` (the operating contract: how to build). That separation of *who's in control* from *how to build* is genuinely smart and directly portable.
- An explicitly **portable harness** (`docs/PORTABILITY.md` + `project.config.json` + `test-portable-purity.sh`): the framework is designed to be lifted into another project by copying `agents/core/`, `docs/agent-rules/`, `docs/harness/`, and rewriting one config file. They *built it to be reused.* The `AI_POLICY.md` pattern is itself credited as adapted from Ghostty — these are good lineages.
- Concrete, copyable mechanisms: merge-gates poller, delta-gated lint rules with `SMATCHET_DEVIATION` escape hatches, the gate-escape-postmortem skill, per-subsystem specialist agent prompts, an agent-eval/calibration scoring policy for grading the agents.

**Could we adopt it?** Selectively, yes — and this is the highest-ROI move in this whole evaluation. We are 15 people on a tight budget; an AI-native methodology that lets a small team ship disciplined tooling fast is *exactly* our kind of leverage. We would **not** import the full 270-script edifice (that needs an AI-native posture and a maintainer we don't have). But the patterns — the policy/contract split, loop modes, delta-gated lint with cheap deviations, the postmortem→new-gate discipline, the portable `project.config.json` boundary — are extractable into our own (smaller) harness for our own tools and game-code chores. The meta-opportunity is larger than the tracker: **Smatchet is a free, working reference implementation of how a tiny team runs an AI dev fleet, and reading it is cheap.**

The caution: the rigor genuinely *de-risks the code Smatchet itself ships* (sanitizers/perf/fuzz don't lie). It does **not** de-risk *our adoption* of Smatchet-as-a-product, because de-risking depends on the fleet+founder continuing to run. For methodology transfer the founder-dependence doesn't bite us — we'd run our own, smaller loop. So: **trust the methodology, study it, copy the good parts; don't bet operational dependence on the project's continuity.**

---

## 8. Scorecard

| Dimension | Score /10 | Rationale |
|---|---|---|
| Problem–solution fit | **9** | Unreal-embedded, P4-aware, multi-tracker, offline, AI/MCP — hits our exact pain; rare combination |
| ROI / cost | **7** | MIT ($0 license) is huge; offset by from-source build cost and uncertain self-maintenance |
| Adoption ease | **4** | No binaries/releases, Windows-first, long toolchain, no installer we don't build ourselves |
| Risk profile (higher = safer) | **3** | Severe bus-factor (1 human), prerelease, no SLA, AI-self-audited; MIT + clean seams cap the downside |
| Differentiation | **9** | Unreal-embedded P4-aware tracker is not buyable elsewhere |
| Licensing safety | **10** | Clean MIT; vendored deps documented; no lock-in |
| Strategic / methodology upside | **9** | Free, portable, working AI-native governance blueprint — the real prize |
| **Overall** | **6.5/10** | A high-value, differentiated tool and an outstanding methodology reference, gated by severe key-person/maintenance risk that argues for a contained pilot, not broad adoption |

---

## 9. Decision & Conditions

**Decision: PILOT — contained, time-boxed, two parallel tracks.**

**Track A — the tool (4-week pilot, 1–2 leads):**
1. Pin a specific vetted commit on `develop`; do **not** track the live branch.
2. One engineer produces a clean, signed internal build and smoke-tests it against our actual Jira + GitHub Issues + Perforce (the repo ships signing + installer-smoke-test guides — use them).
3. Run our own lightweight security review before any production P4/Git credentials touch it; use least-privilege, revocable tokens; confirm DPAPI key storage behaves.
4. Put it in front of 2–3 leads who live in Unreal+P4 for 3–4 weeks. Measure: does the in-editor, P4-aware, unified-tracker experience save real time vs alt-tabbing to Jira/GitHub?

*Go/no-go for wider rollout:* proceed only if (a) the build/sign pipeline is repeatable by us unassisted, (b) the security review is clean, and (c) leads report concrete time savings. If the founder's cadence visibly stalls during the pilot, downgrade to "frozen pinned internal tool" rather than ongoing adoption — never make it load-bearing for the studio without a maintenance owner on our side.

**Track B — the methodology (parallel, ~3 engineer-days, higher strategic ROI):**
1. Have our tech lead read `AI_POLICY.md`, `AGENTS.md`, `docs/agent-rules/` (ship-loops, merge-gates, delegation), and `docs/PORTABILITY.md` as a reference design.
2. Prototype a *minimal* version of the policy/contract split + delta-gated lint + postmortem-on-escape loop for one of our own internal tools or our game-code chores.
3. Decide whether AI-native development is leverage our small team should invest in — using Smatchet as the free worked example.

**Pass conditions (when to walk away entirely):** if Track-A security review surfaces credential-handling problems we can't quickly remediate, *or* if we cannot produce a repeatable build ourselves, drop the tool. Even then, **keep Track B** — the methodology costs almost nothing to study and is the most durable value in this evaluation.

**Bottom line:** Adopt the *ideas* now (cheap, high-leverage); pilot the *tool* carefully (differentiated value, real key-person risk); bet operational dependence on neither the founder nor the fleet until there's a maintenance story we control.
