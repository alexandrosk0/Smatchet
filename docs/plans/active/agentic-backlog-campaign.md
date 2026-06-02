# Plan — Agentic self-improvement backlog campaign

> **Slug**: `agentic-backlog-campaign` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

The self-improvement backlog (`docs/self-improvement/categories/`) holds **138 live entries** as of 2026-06-02 (bug 14 · process 27 · tooling 46 · infra 17 · test 19 · security 14 · external 1). They accumulated organically from agent `## Self-improvement` reports across many ship-loops; no campaign has driven them down. This plan batches the 137 actionable entries (one is already resolved — see below) into cohesive, shippable PRs ordered by priority + dependency, so the loop tightens instead of growing.

Already-dead entry: **security P1 "bug-reporter must NOT ship a GitHub token"** is marked `RESOLVED — relay deployed + verified live 2026-05-30`. It needs archiving to `applied.md`, not work. Folded into B0.

After this lands: the live backlog is drained to its irreducible core (external-blocker + tier-3 multi-PR refactors that warrant their own plans), and every P1/P2 with a bounded fix has shipped or has a tracked follow-up.

Out of campaign scope (own plans, tracked tier-3): B12 AppController decomposition (already underway), B13 testability TU-splits, B14 `#define ImGui` sweep.

## Approach

Execute batches **B0 → B11 in strict priority order**, one PR per batch (per `AGENTS.md` § PR batching — one PR per logical feature). Each batch is a cohesive cluster: same subsystem, or same review concern, so CodeRabbit's quota + file-ceiling aren't burned on conceptual fragments.

**Critical path drives the order**: B1 (CI shallow-clone merge-base) must land first — it false-flags every stacked/parallel PR, so any multi-PR campaign is noisy until fixed. B8 (bucket-E unblock) is the test keystone — ~10 deferred coverage entries are blocked on three infra fixes; landing those three converts the dependents from "blocked" to "cheap." Tier-0 P1 batches (B1–B4) ship before any P2 work.

Each batch, on completion: edit the member backlog entries' `Status` to `applied`, move them to `applied.md`, re-run `bash agents/scripts/core/test-backlog-counts.sh --fix` in the same commit (the pre-push gate rejects count drift). Where a member is the trigger for an AGENTS.md / agent-prompt rule, apply that edit too. `code-review` has eval coverage — score base-vs-head before flipping its entries per § Optimize against evals.

## Files to modify

Grouped by batch. `path` links are representative anchors; each batch's PR enumerates exact `path:line`.

**B0 · Backlog hygiene** (pure-docs)
1. `docs/self-improvement/categories/security.md` — archive the RESOLVED relay P1 entry to `applied.md`.
2. `docs/self-improvement/categories/applied.md` + `AGENT_SELF_IMPROVEMENT.md` § Index — `--fix` count sync.

**B1 · CI shallow-clone merge-base** (tooling P1)
3. `.github/workflows/*.yml` (jobs running `test-all.sh` / delta gates) — `fetch-depth: 0` or targeted base fetch.
4. `agents/scripts/project/function_size_audit.py` + `comment_audit.py` — `_merge_base_or_ref` fail-closed option.

**B2 · merge-watcher daemon repair** (tooling P1 + infra P2/P3)
5. `agents/scripts/core/merge-watcher.py` — owner/repo/ORCH_USER export at startup; positional `merge-gates.sh owner repo pr`; return-code→registry-state classify (not blanket GH_API_DOWN); run git ops in isolated clone/worktree (`-C <isolated>`).
6. `agents/scripts/core/merge-watcher-cli.py` — `await <pr> [--until=blocking|terminal]` blocking command + agent-reachable event sink (`.claude/.agent-events.jsonl`).
7. `agents/scripts/core/merge-watcher-install-autostart.ps1` — restore canonical task action post-#644; defensive `Git\bin` PATH prepend.
8. `tests/bats/` — return-code→state mapping coverage.

**B3 · code-color long-text editor + tooltip** (bug P1)
9. `Source/Core/src/SmatchetLongTextEditorUi.cpp` + callstack-tooltip render path — `editor.Colorize(0,-1)` after `SetLanguageDefinition`.

**B4 · AI endpoint allow-list + http:// consent** (security P1)
10. `Source/Core/src/AiEndpointSanitize.cpp` + `ConfigManager.h` — `AiAllowCustomEndpoint{OpenAi,Anthropic}` fields, `EndpointVerdict::RejectedNonProviderHost`, http→non-loopback consent gate.
11. `Source/Core/src/SmatchetPreferencesUi.cpp` — one-time consent dialog.

**B5 · AI assistant correctness + security** (AiAssistantController cluster)
12. `Source/Core/src/AiAssistantController.cpp` — single turn-start `TrackerConfig` snapshot; `RefreshProviderForTurn` fail-closed on null rebuild; `ComposeSystemPrompt` attribute-escape; first-send consent modal gate.
13. `Source/Core/src/AiSseParser.cpp` — `Flush` drops partial frame instead of synthesising `\n\n`.
14. `Source/Core/src/AgentsMdLoader.cpp` — filename-suffix + canonical-root + symlink-reject path validation.
15. `Source/Core/src/AppController_LuaBindings.cpp` — `ai.prompt` rate-limit + per-session consent toast.
16. `Source/Core/src/Config/ConfigManager*.cpp` — `SanitizeConfigStringValue` (CR/LF/NUL strip) at persist for header-bound fields.

**B6 · Tracker write-correctness + injection**
17. `Source/Core/src/Tracker/TrackerFieldPayloadPure.cpp` + `TrackerFieldValueUtils.cpp` — route all option id/value resolution through one id-first helper.
18. `Source/Core/src/Tracker/TrackerHttpUtils.cpp` — `JqlQuoteLiteral()`; `cpr::Redirect(false,...)` (or same-host restrict) on tracker helpers.
19. `Source/Core/src/Tracker/JiraIssueSearch.cpp` / `PlaneIssueSearch.cpp` — key escaping at interpolation sites; Plane empty-page aligned to Jira `fetchedPages>0`.
20. `Source/Core/src/TicketSyncService.cpp` — age-out / two-consecutive-empty guard so legit-empty reconverges.

**B7 · CI lint + supply-chain hardening** (one security/CI PR)
21. `.github/workflows/build-and-test.yml` — bucket-A C++ lint step (lint-catch-all + curated cppcheck over diff); delete dead `windows-msvc-no-agentic` job; pin all `uses:` to SHAs; workflow `permissions: contents:read` + per-job overrides; Mesa TOFU SHA256.
22. `.github/dependabot.yml` — github-actions weekly.
23. `agents/scripts/project/test-lint-rules.sh` — parallelize `scan_narrowing` clang-tidy fan-out (`xargs -P`).
24. `CMakeLists.txt:377` — Lua tarball `EXPECTED_HASH SHA256=`.

**B8 · Bucket-E unblock (test keystone)**
25. spawn/MCP-ready/test-queue lifecycle — deterministic spawn-warmup gate to kill the intermittent `failed:N passed:0` flake.
26. `tests/CMakeLists.txt` — `/EHsc` on `SmatchetTests` so `CHECK_THROWS*` compiles locally.
27. `scripts/dev/perf-run.sh` / `scenario.run` — headless file-result mode bypassing the spawn socket handshake (for worktree-isolated perf capture).

**B9 · git hygiene tooling**
28. `scripts/dev/git-leftover-audit.sh` (new) — read-only loss/residue map keyed on `gh pr` state vs `origin/develop`.
29. `scripts/dev/worktree-prune.sh` (new) — ff-pull + delete `[gone]` branches per worktree; refuse on dirty.
30. `agents/core/git-janitor.md` + `--light` periodic merged-only prune.

**B10 · Docs/process one-liners** (pure-docs, one PR)
31. `AGENTS.md` + `docs/agent-rules/*` + `docs/plans/active/_plan-template.md` + agent prompts — ~12 forcing-rule edits (enumerated in § Risks/non-goals member list).

**B11 · Mechanical code cleanups** (one PR)
32. 11 empty-catch markers; `new`→`make_unique` (AiClientFactory); `IAiClient ~ = default`; AiClientFactory switch `default:`; AiTypes sentinel comments; SSE/NDJSON `LOG_WARN` redaction ×3; PR-numbered temporal-comment sweep; `merge_gates.bats` `LC_ALL=C.UTF-8`.

## Existing utilities reused

- `agents/scripts/core/test-backlog-counts.sh --fix` — rewrites the § Index counts after every add/archive (B0 + every batch close).
- `RedactProviderErrorBody` (`smatchet::ai::pure`) — reused by B5 SSE/NDJSON redaction.
- `coderabbit-current-head.sh` / `merge-gates.sh` — B2 wraps, doesn't reinvent.
- `tests/ui/views_columns_reorder.test.cpp` — scaffold shape for every B8-dependent bucket-E TU.
- `scripts/dev/agent-eval-{run.sh,score.py}` — score `code-review` prompt edits base-vs-head before applying.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: B3/B5/B6 touch UI + Source/Core hot paths — perf-gate per affected scenario at slice boundary (B5 streaming, B6 grid). Most batches are tooling/CI/docs — no impact.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: B4/B5 add no new UI-thread sync I/O (consent modal is a flag read; rate-limit is a timestamp compare). B8 perf-result mode is off-UI-thread.
- **Pillar 3 (never crash)**: B5 RefreshProviderForTurn fail-closed + B6 JQL escaping + B7 CI lint (catch-all in CI) all *reduce* crash/UB surface. No new crash paths.
- **Pillar 4 (accessibility)**: B4/B5 consent dialogs must be keyboard-navigable (backlogged-but-honour). No regression.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

- **B3** — fires: code-color render path; scenario `code-syntax-coloring` / blame tokenizer. Run gate-check before PR.
- **B5** — fires: `ai-chat-history-render` streaming scenario.
- **B6** — fires: `priority-grid-scroll` (option-resolution touches grid display path).
- **B0/B1/B2/B7/B9/B10** — N/A: no `Source/Core/` diff (docs / CI / scripts).
- **B4** — N/A on perf: validator + config fields, not a hot path.
- **B8** — N/A: test harness, but it *restores* the perf-gate's runnability in worktrees.
- **B11** — N/A: mechanical, no behavioural hot-path change.

## Risks / non-goals

**Risks**
- B1 must land first; merging any tier-1 batch before it risks the documented false-flag cascade. Mitigation: hard-order B1.
- B5 is the largest single PR (8 members, one TU cluster); if it nears the CR file-ceiling, split AiAssistantController-correctness from the security trio along the natural seam. Accepted with split-fallback.
- B7 Mesa TOFU SHA256 is trust-on-first-use, not publisher-attested (upstream ships no checksum). Accepted; documented inline.
- B2 daemon isolation is the riskiest infra change (running git ops underfoot). Mitigation: dedicated clone, bats coverage for state mapping.

**B10 member list** (the ~12 doc edits): baseline.md don't-edit rule (`imgui-draw-pattern.md`); git-audit diff-vs-origin callout; verify-file-claims-before-asserting (SI § workflow); 67 KB file-size cap + split recipe (AGENTS.md § Project rules); dual-target plan-template §; grep-before-naming-TUs (plan-template § Files); slice-coordination inline-shape rule; `git merge` not `rebase` default + catch-up-sync sub-rule; post-squash `branch -D` fast-path (git-janitor.md); `unique_ptr<incomplete-type>` bullet; INTERFACE-target linkage check (delegation packet); slice→agent routing reject test-rig for prod-header slices; architect "name the chokepoint shim" checklist item; CR "✅ Addressed" verify-the-diff rule.

**Non-goals**
- Tier-3 (B12 AppController decomposition, B13 TU-splits, B14 macro sweep) — own plans; not driven here.
- The external-blocker entry (Claude Code SDK worktree base) — upstream, untouchable.
- Parked P3 retrospective code-review nits (JoinUrl, ListClipper, chunk-coalesce, etc.) — fold opportunistically into the next PR that legitimately opens the file; not chased standalone.

## Verification

- **Bucket A (pure-logic ctest)**: B6 option-resolution colliding-set regression; B4 endpoint-verdict cases; B5 AiSseParser Flush-drops-partial; B11 no new asserts (mechanical).
- **Bucket E (ImGui Test Engine)**: unblocked by B8 — the ~10 deferred coverage TUs (AI assistant flows, description tooltip markdown, code-color long-text) become writable. Each ride-along with its feature batch where the feature is touched.
- **Bash-driver / bats**: B2 return-code→state mapping; B7 lint-step over a deliberate violation fixture; B9 audit-script classification.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` for every Source/Core-touching batch (B3/B4/B5/B6).
- **Manual residue**: any bucket-E TU that can't run headless stays a tracked tooling entry (don't silently drop) — but B8 is the explicit attempt to remove that residue.

## Out of scope (flagged, not designed)

- Coverage-threshold advisory→blocking flip (tooling, needs 2-week soak baseline) — time-gated, not work-gated; flip when the soak window closes.
- Whisper Phase-H default-flip decision (infra P2) — product decision (binary size vs UX), needs the user's size-measurement verdict; ask at the next whisper-touching session.
- DX12 backbuffer readback for bucket-C (infra P2) — `unreal-bridge` territory, multi-hour DX12 resource-state work; own slice.

## Implementation log

- `bedda57e` · #691 · B0 — plan doc + archived 3 already-shipped P1s (CI merge-base #688, bug-reporter relay, merge-watcher GH_API_DOWN). Squash-merged to develop.
- `32040e3e` · #693 · Phase-0 staleness sweep — archived 11 more already-shipped entries (1 P1 + 8 P2 + 2 P3) with per-entry tree evidence.
- #701 · **B4** — AI-endpoint host allow-list + insecure-http consent (the lone live P1): `EndpointPolicy` host-pin + consent verdicts, single-source `AiEndpointPolicy`, migration grandfather, consent UI, 18 tests. Survived a CR round (loopback-bypass tightening + host-exact grandfather). Archived the source security P1.
- #B10 · **B10** — encoded 14 deferred process forcing-rules into their home docs (AGENTS.md, process-rules.md, delegation.md, git-janitor.md, architect.md, _plan-template.md, imgui-draw-pattern.md, SI workflow) + archived all 14 source entries. Sub-plan: `b10-docs-forcing-rules.md`.

## Deviations from plan

- **Phase 0 (staleness sweep) became the dominant first deliverable, not B1-execution.** Verifying premises before coding revealed **14 prioritized entries already shipped** — including 5 of 6 P1s (B1 CI merge-base, B2 merge-watcher GH_API_DOWN, relay, tooling long-running-polls, bug code-color). Executing them as written would have produced redo-work PRs. This is the "verify file-claims before acting" process rule paying out at campaign scale.
- **B6 and B11 shrank; B7 partially cleared.** Tracker option-resolution + Plane empty-page (B6) done; AiClientFactory-raw-new + 11-empty-catch (B11) done; CI dead-job + workflow-permissions (B7) done. Remaining members of those batches stay live.
- **Sweep coverage is scoped to code-defect entries** (staleness detectable by grep/tree-check). Process-rule proposals (process.md, B10) and coverage-gap entries (test.md, most B8 dependents) do not go stale via code drift — they remain live by nature and were not re-verified one-by-one.

## Re-scoped live remainder (post-sweep)

Single live P1: **B4 AI-endpoint host allow-list**. Revised live batches:

- **B4** (P1) — AI-endpoint allow-list + http→non-loopback consent. `AiEndpointSanitize.cpp` still only checks scheme.
- **B5** — AI assistant cluster, all live: torn-config (4 `ConfigManager::Load()`/turn), `RefreshProviderForTurn` fail-open, `ComposeSystemPrompt` escape, `AgentsMdLoader` path-traversal, `ai.prompt` rate-limit, first-send consent modal, CR/LF persist-strip, `AiSseParser::Flush` partial-frame.
- **B6** (shrunk) — JQL injection, tracker `cpr::Redirect(true,true)` ×5 cross-host auth forwarding.
- **B7** (shrunk) — Lua tarball `EXPECTED_HASH`, action SHA-pins (23 floating `@v`), Mesa TOFU SHA256, narrowing-scan parallelize, C++ lint in CI.
- **B8** — bucket-E unblock keystone (spawn flake + `SmatchetTests` `/EHsc` + perf-run worktree handshake) → ~10 dependent coverage TUs.
- **B9** — `git-leftover-audit.sh` → `worktree-prune.sh` + `git-janitor --light`.
- **B10** — ~12 doc/process forcing-rule edits (live by nature).
- **B11** (shrunk) — `IAiClient ~ = default`, AiTypes sentinel comments, SSE/NDJSON `LOG_WARN` redact ×3, `merge_gates.bats` `LC_ALL`, PR-numbered comment sweep.
- **Tier-3 (own plans)** — B12 AppController decomposition (1263 LOC, live), B13 TU-splits, B14 `#define ImGui` sweep (10+ TUs, live).

## Verification (actual)

- B0 + sweep: pure-docs. `test-backlog-counts.sh` 8/0 on both. #691 squash-merged clean (gates passed via watcher). #693 open.
