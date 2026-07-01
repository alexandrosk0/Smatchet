# Smatchet — Multi-Expert Evaluation

An independent, from-scratch evaluation of the Smatchet project through **nine expert lenses**. Each expert evaluated the project **twice** — once **ignoring** the agentic-governance meta-layer (`AGENTS.md` files, the `agents/` tree, `AI_POLICY.md`, `docs/agent-rules/**`, `docs/self-improvement/**`, `.coderabbit.yaml`) and once **with that layer fully loaded** — followed by a **comparison & critic** report dissecting what changed between the two passes.

- **9 experts × 3 reports = 27 reports** (~77,000 words), each grounded in `file:line` evidence from the actual codebase.
- **Plus a follow-up "Pass C" (`no-comments.md`)** for the three deep code-reading lenses (hacker, architect, AAA tools-lead): the same code re-judged against a **comment-stripped mirror** of `Source/**` (all 23,471 `//` + 1,124 `/* */` removed, line numbers preserved) to isolate how much each read leaned on the code's own self-documentation. See [§ Pass C](#pass-c--how-much-did-the-comments-matter) below.
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

## Pass C — how much did the comments matter?

A third pass re-ran the three deepest code-reading lenses against a **comment-stripped mirror** of `Source/**` (comments replaced with blank space, line numbers preserved so `file:line` still resolves). This isolates a single variable — the code's own inline self-documentation — against each expert's code+comments (`without-agents`) baseline. Reports: `01/no-comments.md`, `04/no-comments.md`, `08/no-comments.md`.

| Expert | Overall (code+comments → no-comments) | Self-documentation sub-score | Verdict on comment-dependence |
|---|:--:|:--:|---|
| White-hat hacker | 8.0 → **8.0** | auditability-without-comments **9/10** | Threat model survives stripping — invariants live in identifiers (`ConstantTimeStringEquals`) and runtime strings/enum tables ("blocked to prevent SSRF") |
| Programming architect | 8.0 → **8.0** | legibility-without-comments **7.5/10** | Structure/types carry intent; comments load-bearing only at the god-object |
| AAA tools-lead | 7.0 → **7.0** | forkability-without-comments **7/10** | All three lift-candidates remain reusable blind; ABI/threading seams degrade |

**Finding: stripping comments moved no overall score.** The architecture and security posture are what the code *is* — encoded in naming, types, RAII, `k`-constants, the `*Pure`/`I*Deps`/`*Fixture` conventions, and (for security) descriptive runtime strings — none of which the strip could touch. Comments turn out to be **load-bearing only where C++ types cannot express intent**, and that dependence concentrates in exactly the risky places:

- **Concurrency / init-ordering invariants** — `AppController.h` was **56% comment lines** (825/1,465); stripped, it collapses to ~150 bare declarations losing every threading-affinity and init-order contract (architect).
- **Lock-lifetime contracts** — the deleted `CommandRegistry::FindLocked` comment warned its returned pointer is invalidated by concurrent `Register`; blind, the name misreads as "this locks," masking a latent use-after-free that `McpPlugin.cpp:592` only survives via an invisible startup-only-registration invariant (tools-lead).
- **C-ABI seams** — the Unreal bridge's `void* rendererResource0/1/2` init slots go semantically opaque, adding a 2–4 day reverse-engineering tax (tools-lead).
- **Accepted-risk vs oversight** — blind, you can't tell whether the AI-endpoint SSRF sanitizer's IP-literal-only coverage (no post-DNS re-check) is a deliberate trade-off or a gap; only dynamic testing resolves it (hacker).

**Takeaway:** comments here matter for *safe change*, not for *comprehension of what exists* — and they cluster at the concurrency/ABI/god-object seams, which is precisely where a maintainer (human or AI) should tread carefully. A useful, actionable signal rather than a red flag.

## How to read this

Each expert folder contains:
- `without-agents.md` — the code/product-only pass.
- `with-agents.md` — the same lens with the full agentic-governance layer loaded.
- `comparison.md` — a critic report on the delta: score movement, what each pass saw or missed, contradictions, and a blended verdict (this report also critiques the two reports themselves for overclaim / persona drift).
- `no-comments.md` *(experts 01, 04, 08 only)* — the Pass C re-evaluation against comment-stripped code, with an explicit delta vs the code+comments pass and a "self-documentation /10" sub-score.

Start with this index, then `02` (agentic-infra) and `05` (tech-director) for the most dramatic with/without contrast, `01` (security) for the only score that *fell* with the meta-layer loaded, and `07` (indie CEO) for the only verdict *flip* (PASS → PILOT).
