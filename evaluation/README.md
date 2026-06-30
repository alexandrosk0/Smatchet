# Smatchet — Multi-Expert Evaluation

An independent, from-scratch evaluation of the Smatchet project through **nine expert lenses**. Each expert evaluated the project **twice** — once **ignoring** the agentic-governance meta-layer (`AGENTS.md` files, the `agents/` tree, `AI_POLICY.md`, `docs/agent-rules/**`, `docs/self-improvement/**`, `.coderabbit.yaml`) and once **with that layer fully loaded** — followed by a **comparison & critic** report dissecting what changed between the two passes.

- **9 experts × 3 reports = 27 reports** (~77,000 words), each grounded in `file:line` evidence from the actual codebase.
- Method: parallel independent sub-analyses; each pass was blind to the other so the "with / without `AGENTS.md`" contrast is real, not narrated.
- Scores are each reporter's own 0–10 judgment. **Figures vary between reports** (e.g. postmortem-ledger size is quoted as "43 entries", "51 entries", "255 entries", and "2,013 lines" by different agents counting different things) — treat per-report numbers as that reporter's measurement, not a canonical fact.

## What Smatchet is

A ~160–218K-LOC **C++14 / Dear ImGui** desktop issue-tracker & productivity client: multi-backend tracker (Jira / Plane / GitHub Issues / Linear via `ITrackerBackend`), Perforce annotate, SQLite caching, a unified command registry feeding CLI + command-palette + MCP + Lua, a provider-pluggable AI assistant, an Unreal-Engine DX12 in-editor plugin, and an Android effort — all wrapped in an unusually elaborate **autonomous-AI-agent governance system** (quality-pillar gates, "gate, don't trust" merge gates, a self-tightening gate-escape→postmortem ratchet, ~270 gate scripts, ~28 CI workflows). Per `git shortlog`, the project's own history is **effectively single-human-authored** (≈49 commits by one human + 1 dependabot at the time the experts measured ≈ 98% human; the remaining "Claude"-authored commits in the tree are this evaluation's own report commits, not product code). The human is the sole maintainer; the AI agents are the workforce — hence the recurring **"bus factor of 1"** finding below.

## Master scorecard

| # | Expert lens | Without `AGENTS.md` | With `AGENTS.md` | Blended | Bottom line |
|---|---|:---:|:---:|:---:|---|
| 01 | [White-hat hacker (security)](01-white-hat-hacker/) | 8.0 | 7.5 | **7.5** | Product code well-hardened; the **agentic auto-merge pipeline is the top risk** |
| 02 | [Agentic-infra programmer](02-agentic-infra-programmer/) | 7.5 | 8.5 | **8.0** | Best-in-class "gate, don't trust" patterns to steal; meta-layer is the prize |
| 03 | [Game-dev (tracker user/buyer)](03-game-dev-tracker-user/) | 6.0 | 6.5 | **6.25** | Real product; pilot for P4 crash-triage, don't standardize |
| 04 | [Programming architect](04-programming-architect/) | 8.0 | 8.0 | **8.0** | Strong ports-and-adapters; "architecture as enforced invariant" works |
| 05 | [Technical director](05-technical-director/) | 6.0 | 7.0 | **6.5** | Over-scope made tractable by agents, but two unhedged SPOFs |
| 06 | [QA director](06-qa-director/) | 8.0 | 8.4 | **8.2** | Strong test estate + a genuinely self-tightening quality ratchet |
| 07 | [Indie-studio CEO](07-indie-studio-ceo/) | 4.0 (PASS) | 6.5 (PILOT) | **5.5** | Pass on the tool as a dependency; **copy the methodology** |
| 08 | [AAA tools-team lead](08-aaa-tools-lead/) | 7.0 (lift) | 7.5 (lift) | **7.5** | Lift the command registry + Unreal bridge **and** the AI-team methodology |
| 09 | [UX expert](09-ux-expert/) | 6.0 | 7.4 | **7.0** | Enforced responsiveness is a rare UX win; screen-reader/onboarding gaps remain |

**Average blended score: ~7.2 / 10.** Range 5.5 (indie CEO) → 8.2 (QA director).

## The `AGENTS.md` effect — the headline finding

Loading the agentic-governance layer **moved every score except two**, and the direction is itself the most interesting result:

- **It raised most scores** (agentic-infra +1.0, UX +1.4, tech-director +1.0, indie-CEO +2.5 and a PASS→PILOT flip, QA +0.4, AAA-lead +0.5). The meta-layer supplied the *mechanism* that the code-only passes found missing or unexplained: it explains **how a near-solo maintainer sustains 218K LOC + huge feature breadth** (tech-director), it **is the main attraction** for an agentic-infra builder (the merge-gate poller, delta-gated lint, escape→preventing-gate flywheel), and it **enforces the measurable slice of UX** (6.94 ms frame budget, "no UI block > 100 ms", gated WCAG-AA contrast) that a pure-UI audit cannot see.
- **It *lowered* the security score** (8.0 → 7.5). The hacker's with-pass discovered an entirely new, higher-severity attack surface invisible to the product-only pass: an **LLM that auto-merges its own PRs** (`governance.auto_merge: on`) gated by a poller that GitHub-native / REST / stale-daemon merge paths **demonstrably bypass** — confirmed by real gate-escape postmortems (#1428 / #1438 / #1566 / #1406–1415) — plus **prompt-injection-to-merge** with agent-reachable override labels.
- **It barely moved buyer-facing scores** (game-dev +0.5, architect ±0). For a *user/buyer*, build-process rigor matters less than features, binaries, and support; for the architect, the governance *explained how* health is preserved without changing the verdict on whether the architecture *is* healthy.

**Net:** the meta-layer is simultaneously the project's **biggest asset** (rigor, velocity, self-correction, a portable AI-team blueprint) and a **new class of liability** (auto-merge trust model, a second over-scoped "product" to maintain, bus-factor concentration). Reading it makes you trust the *code* more and the *vendor continuity* less.

## Cross-cutting findings (independently surfaced by multiple experts)

**Strengths (high agreement):**
- **"Gate, don't trust" + self-tightening ratchet is real, not theater** — every escaped defect becomes a mandatory new permanent gate; multiple experts traced specific postmortems (#357, #923, #1428) to shipped gates. (02, 04, 05, 06)
- **Supply-chain hygiene is excellent** — all FetchContent deps pinned to full commit SHAs; the one HTTP-fetched dep (Lua) is SHA-256-verified with mirror fallback; MIT with no copyleft traps. (01, 05)
- **Security hardening above the norm for the attack surface** — token-required loopback MCP with DNS-rebind defense + constant-time compare, a whitelist Lua sandbox, DPAPI-sealed secrets, a bounded JSON parser. (01)
- **Clean load-bearing abstractions** — `ITrackerBackend` ISP, the unified Command registry fanning one definition to five frontends, dual GLFW/DX12 render targets from one source. (02, 04, 08)

**Weaknesses / risks (high agreement):**
- **Bus factor of 1** — a single human + an AI fleet maintaining the app *and* a ~270-script governance layer; no second maintainer, no community. Flagged as the dominant risk by nearly every expert. (03, 05, 07, 08)
- **The marquee "144 Hz / 6.94 ms" perf budget is NOT enforced as an absolute floor** — only relative-regression detection is armed; the absolute ceiling ships `null`/DISABLED in `regression-policy.json`. (03, 06, 08, 09)
- **Native-merge bypass of the custom poller** is a recurring, documented gate-escape class. (01, 05, 06)
- **"Engine-agnostic / cross-platform" is in practice Windows-first** — DPAPI, Win32 hotkeys, Windows-only Whisper, zero macOS, Linux advisory-only; Android is a thin shell. (05, 08)
- **No prebuilt binaries / no releases / no SLA** — build-from-source on Windows/CMake; unadoptable as a turnkey product today. (03, 07)
- **Accessibility / interaction-craft gaps governance doesn't touch** — no screen-reader/AT-API surface, no onboarding/empty states. (09)
- **`AppController` god-object** (1,465-line header, ~115–137 includers) — named, ratcheted, and being decomposed, but still the chief coupling risk; the 126–129 KB monolithic `CMakeLists.txt` contradicts the modularity enforced elsewhere. (04)

## How to read this

Each expert folder contains:
- `without-agents.md` — the code/product-only pass.
- `with-agents.md` — the same lens with the full agentic-governance layer loaded.
- `comparison.md` — a critic report on the delta: score movement, what each pass saw or missed, contradictions, and a blended verdict (this report also critiques the two reports themselves for overclaim / persona drift).

Start with this index, then `02` (agentic-infra) and `05` (tech-director) for the most dramatic with/without contrast, `01` (security) for the only score that *fell* with the meta-layer loaded, and `07` (indie CEO) for the only verdict *flip* (PASS → PILOT).
