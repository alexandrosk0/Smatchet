# Plan — Agent surface extraction into its own repository

> **Slug**: `agent-surface-extraction-repo` (matches this file's basename without `.md`).
>
> **Status**: `active`
>
> **Plan-link convention**: all plan references below use the tier-less form `docs/plans/<slug>.md`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The agentic layer (agent prompts, skills, rule-docs, harness adapters, gate scripts, self-improvement framework) was deliberately built portable — the boundary is documented in `docs/STRUCTURE.md` and `docs/PORTABILITY.md`, with `project.config.json` as the single value seam, and the groundwork shipped in `docs/plans/agentic-layer-project-independence.md`. Reuse today still means "copy + adapt": the layer lives interleaved with the product tree, so it cannot be versioned, released, or consumed independently.

User request (2026-08-29): **move the full agent surface into its own repository**. Decisions taken up front (batched clarification):

1. **Scope — full agent surface, framework only**: `agents/{core,project,_shared,scripts}/`, `docs/agent-rules/`, `docs/harness/`, the self-improvement **framework** (`AGENT_SELF_IMPROVEMENT.md` + category structure spec), the generic gate scripts, the layer-coupled bats suites, and the `AGENTS.md` rulebook content. Smatchet keeps `project.config.json` (+ schema), `AI_POLICY.md`, `CONTEXT-MAP.md`, `docs/plans/`, subsystem leaf docs, project-only scripts, **and the self-improvement entries** (`categories/*`, `postmortems.md`, `applied.md`, the `.jsonl` ledgers) — see grill decision 4 below, which reversed the original "framework **and** entries" scope.
2. **Mechanism — git submodule** mounted at a single directory (`agent-layer/`) in the Smatchet root, pinned by SHA, bumped by explicit PRs.
3. **History — preserved** via `git filter-repo` when seeding the new repo.

After this lands: the agent surface is a standalone **public** repo — **`the-unwilling-agentic-bunch`** (display name "The Unwilling Agentic Bunch") — with its own CI and merge gates; Smatchet consumes it as `agent-layer/` and any other project can consume it the same way.

**Grill outcomes (grill-with-docs run 2026-08-29 — six decisions locked):**

1. **Bats split**: the 61 of 94 `tests/bats/*.bats` suites that reference `agents/` paths (measured: `grep -l 'agents/' tests/bats/*.bats`) move with the layer; the 33 product-coupled suites stay. No shared helper/fixture files exist — each suite is self-contained, so the split is a clean file partition.
2. **Config seam**: dual-root design — `PROJECT_ROOT` (host tree: `docs/plans/`, `backlog/`, `Source/`, self-improvement entries) + `AGENT_LAYER_ROOT` (layer tree). `project.config.json` resolution order: `$SMATCHET_PROJECT_CONFIG` env → superproject root → layer repo root. The layer repo ships its **own real `project.config.json`** and runs the framework on itself (self-hosting first consumer); standalone layer CI sets `PROJECT_ROOT=$AGENT_LAYER_ROOT`.
3. **Pointer-bump CI blind spot closed**: post-flip a bump PR changes only the gitlink, so no path-filtered host lane fires — a new host lane `agent-layer-integration.yml` (path-filtered on `agent-layer` + `.gitmodules`) binds real checks to bump PRs.
4. **Self-improvement entries stay host-side** (reverses original scope point 1): entries are project-specific per `docs/STRUCTURE.md` / `docs/PORTABILITY.md`, are the hottest write path (525 commits since Jun 1 — vs ~500 for the entire rest of the surface), and a submodule working tree is not a safe write target (`submodule update` discards local writes). Framework spec moves; entries never cross the boundary.
5. **Bump cadence — per-merge auto-bump**: a post-merge workflow in the layer repo opens the `chore(agent-layer): bump to <sha>` PR in Smatchet automatically on every layer merge (~6 layer commits/day measured over 3 months justifies automation); `git-janitor` demotes to backstop proposer when a bump was missed.
6. **Repo identity**: `the-unwilling-agentic-bunch`, public — relative `.gitmodules` URL resolves tokenless for forks and CI.

## Approach

**Keep the internal layout of the new repo byte-identical to the current in-tree layout** (`agents/…`, `docs/agent-rules/…`, `docs/harness/…`, `AGENTS.md` at the new repo's root; from `docs/self-improvement/` only the framework doc `AGENT_SELF_IMPROVEMENT.md` moves — the entries, ledgers and `categories/` stay host-side per grill decision 4 / ADR-0025). Every intra-surface relative link and script path then survives the move unchanged; only **cross-boundary** references (surface ↔ product) need rewriting, and those are exactly the "External path contracts" table already maintained in `docs/PORTABILITY.md` plus the workflow/hook invocation paths.

**De-risk with an indirection phase before the move.** Phase A introduces a **dual-root** pair — `PROJECT_ROOT` (host tree: plans, backlog, entries, `Source/`) and `AGENT_LAYER_ROOT` (layer tree), both exported by `project-config.sh`, both defaulting to the repo root pre-flip — and rewrites every consumer (workflows, `scripts/dev/*`, hooks, `setup-harness.sh`, docs where load-bearing) to address each tree through the right root, while the files have not moved. CI proves the indirection green. The actual flip (Phase C) then changes one value (`AGENT_LAYER_ROOT=agent-layer`) plus the `git rm` + `git submodule add`, instead of a big-bang path rewrite entangled with the move itself. Layer scripts that read *host* content (plan index, backlog counts, entry sweeps) resolve via `PROJECT_ROOT`; scripts that read *layer* content resolve via `AGENT_LAYER_ROOT` — the distinction is what lets the layer repo run standalone (`PROJECT_ROOT=$AGENT_LAYER_ROOT`, its own `project.config.json`).

**Two-repo ship-loop is the real ongoing cost** and is designed in, not discovered later: an agent-surface edit becomes (1) a PR in `the-unwilling-agentic-bunch` gated by that repo's own CI (agentic-selftests over the 61 moved bats suites, shell-lint, doc-validation subset, merge-gates.sh reused on itself), then (2) a one-line submodule-pointer-bump PR in Smatchet — **opened automatically by a per-merge workflow in the layer repo** (grill decision 5), with `git-janitor` as backstop proposer and the new `agent-layer-integration.yml` host lane as the bump PR's binding check (grill decision 3). `docs/agent-rules/ship-loops.md` gets a new § Two-repo ship-loop codifying this. The trade-off accepted: per-edit friction up, in exchange for independent versioning/release of the layer and true reuse.

## Files to modify

Grouped by phase. Path lists below are the known consumers (from `docs/PORTABILITY.md` § External path contracts + a workflow grep); **Phase A opens with an exhaustive repo-wide sweep** (`rg -l 'agents/(core|project|_shared|scripts)|docs/agent-rules|docs/harness|docs/self-improvement'`) whose full hit-list is committed as the phase's checklist — the rows here are the anchors, not the closed set.

> **Companion detail docs.** Each row below is the anchor; the expanded form — sub-rows, exact edit sites with line numbers, PR split, and the adversarial findings folded in — lives in a companion file under [`agent-surface-extraction-repo/`](agent-surface-extraction-repo/):
> [`phase-a.md`](agent-surface-extraction-repo/phase-a.md) (Phase A, incl. the A1/A2/A3 PR split) ·
> [`phase-b-c.md`](agent-surface-extraction-repo/phase-b-c.md) (Phase B seed + Phase C flip) ·
> [`phase-d-risks.md`](agent-surface-extraction-repo/phase-d-risks.md) (Phase D process/docs + expanded risks) ·
> [`verification.md`](agent-surface-extraction-repo/verification.md) (existing-utilities detail + the 30-row per-phase verification matrix).
> This plan file stays the authoritative scope + status document; a companion never overrides it. **At archive time the companion directory is `git mv`'d together with this file** (`git mv docs/plans/active/agent-surface-extraction-repo docs/plans/shipped/`), so the relative links above keep resolving.

### Phase A — indirection prep (Smatchet PR, no file moves)

*Detail: [`phase-a.md`](agent-surface-extraction-repo/phase-a.md) — sub-rows 1a–7c and the three-PR split (A1 → A2, A3 independent).*

1. `scripts/dev/project-config.sh` — export the dual-root pair: `AGENT_LAYER_ROOT` (default `$REPO_ROOT`) + `PROJECT_ROOT` (default `$REPO_ROOT`) + `PC_*` twins; `project.config.json` resolution order `$SMATCHET_PROJECT_CONFIG` env → superproject root (`git rev-parse --show-superproject-working-tree`) → layer repo root, so the same script works host-side, from inside the future submodule, and standalone in the layer repo's own CI (`PROJECT_ROOT=$AGENT_LAYER_ROOT` there).
2. `.github/workflows/*.yml` (14 files referencing `agents/`: `agentic-selftests`, `build-and-test`, `codeql`, `cr-oob-review-backfill`, `dependabot-auto-merge`, `doc-validation`, `dup-scan`, `issue-janitor`, `locks-render`, `lock-staleness`, `mobile-security`, `perf-pr-fast`, `plan-lock-gate`, `shell-lint`) — route script invocations through the variable; add `submodules: recursive` to **every** `actions/checkout` step repo-wide (harmless now, mandatory after the flip).
3. `scripts/dev/pre-ship.sh`, `scripts/dev/test-all.sh`, `agents/scripts/project/test-lint-rules.sh` — invoke `agents/scripts/…` via `AGENT_LAYER_ROOT`.
4. `agents/scripts/core/setup-harness.sh` — re-point `link_agents()` (per-file `mklink //H` hardlinks, not a directory junction — `link_dir` still exists at `setup-harness.sh:83`, it just no longer serves `.claude/agents`; its one remaining caller is the `.claude/skills/<name>` materialisation at ~line 313) at `$AGENT_LAYER_ROOT/agents`; codex/cursor/pi mirror generators likewise; hook-command paths it writes into `.claude/settings.json` likewise.
5. `agents/scripts/core/{test-agent-contract.sh,test-backlog-counts.sh,sort-applied-md.sh,test-plan-index.sh,check-harness-provisioned.sh}` — path config vars via the right root: `test-agent-contract.sh` + `check-harness-provisioned.sh` read layer content → `AGENT_LAYER_ROOT`; `test-backlog-counts.sh` + `sort-applied-md.sh` + `test-plan-index.sh` read **host** content (entries + plans stay host-side) → `PROJECT_ROOT`. Extend `check-harness-provisioned.sh` to fail loudly when the layer dir is missing/empty (the "checkout forgot submodules" guard).
6. `scripts/dev/worktree.sh` `cmd_new()` — run `git submodule update --init --recursive` in freshly created worktrees (no-op pre-flip). This is the real creator; `scripts/dev/new-session.sh` (`nsc`) only delegates to it, and `bash scripts/dev/worktree.sh new <slug>` is separately documented, so the hook belongs in `cmd_new()` to cover both entry points.
7. `.gitattributes` — `merge=union` row for `docs/self-improvement/categories/applied.md` stays host-side **permanently** (entries never move — grill decision 4); no layer-repo twin needed.

### Phase B — seed `the-unwilling-agentic-bunch` (new public repo; user creates it — pause exception 3, cross-repo mutation)

*Detail: [`phase-b-c.md`](agent-surface-extraction-repo/phase-b-c.md) — rows 8a–10c, the seed-script contract, and the human preconditions.*

8. New repo seeded by `git filter-repo` from a fresh Smatchet clone: `--paths-from-file docs/seed-paths.txt` (the committed path file is the single canonical form — never bare `--path` argv; Phase B rows 8c/8e). That file carries `agents/`, `docs/agent-rules/`, `docs/harness/`, `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`, `docs/high-integrity/portable-purity-baseline.txt`, `docs/high-integrity/agent-size-baseline.md`, `AGENTS.md`, `scripts/dev/project-config.sh`, `scripts/dev/test-all.sh`, `scripts/dev/test-docs.sh`, plus one `tests/bats/<file>` line per layer-coupled suite (the 61-file list generated at seed time via `grep -l 'agents/' tests/bats/*.bats`, committed alongside the seed script as the audit trail). Entries (`docs/self-improvement/categories/`, `postmortems.md`, `applied.md`, `.jsonl` ledgers) are **excluded** — they stay host-side. History preserved; layout unchanged.
9. New repo CI: `agentic-selftests.yml` (over the 61 moved bats suites), `shell-lint.yml`, and a doc-validation subset (anchors / agent-contract / portable-purity / markdown-links scoped to the moved tree) — all reusing the moved scripts on themselves; branch protection + `merge-gates.sh` self-hosted as its own gate-poller; a **fresh** `.coderabbit.yaml` (no self-improvement auto-exemption — those paths stay in Smatchet, whose `.coderabbit.yaml` keeps the path_filter unchanged).
10. New repo root additions: `README.md` (consumption contract: mount at `agent-layer/`, dual-root semantics, host provides `project.config.json` at superproject root), `LICENSE`, and the layer's **own real `project.config.json`** — the repo is its own first consumer (self-hosting: its CI runs the framework's gates on the framework itself with `PROJECT_ROOT=$AGENT_LAYER_ROOT`), and that file doubles as the reference example for new consumers.

### Phase C — the flip (Smatchet PR)

*Detail: [`phase-b-c.md`](agent-surface-extraction-repo/phase-b-c.md) — rows 11a–16, the flip checklist, and the rollback recipe.*

11. `git rm -r agents/ docs/agent-rules/ docs/harness/` + `git rm docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` + `git rm docs/high-integrity/portable-purity-baseline.txt docs/high-integrity/agent-size-baseline.md` + `git rm` the 61 layer-coupled bats suites + `git submodule add ../the-unwilling-agentic-bunch.git agent-layer` + `.gitmodules` (relative URL — both repos public, resolves tokenless). `docs/self-improvement/categories/` + ledgers + the 33 product-coupled bats suites stay, as do the three deliberately dual-homed runners `scripts/dev/project-config.sh`, `scripts/dev/test-all.sh` and `scripts/dev/test-docs.sh` (Phase B row 8g's `cmp` gate covers the mirrors).
12. `scripts/dev/project-config.sh` (or `project.config.json` § paths) — `AGENT_LAYER_ROOT=agent-layer`; `PROJECT_ROOT` stays the Smatchet root.
12b. New `.github/workflows/agent-layer-integration.yml` (host) — path-filtered on `agent-layer` + `.gitmodules`: checkout with `submodules: recursive`, run `check-harness-provisioned.sh` + `setup-harness.sh` + the host-side gate scripts through the submodule. This is the binding check for pointer-bump PRs (grill decision 3 — without it a bump PR changes only the gitlink and merges on docs-tier CI alone).
13. `AGENTS.md` (root) — becomes a thin stub: harness auto-load entry point, `@agent-layer/AGENTS.md`-style import for Claude Code via the regenerated `.claude/CLAUDE.md`, prose pointer for Codex/others. Root stub stays under the 150-line cap trivially.
14. `docs/harness/claude-code/CLAUDE.md.tmpl` — the tracked template `setup-harness.sh` copies to `.claude/CLAUDE.md`; its import line becomes `@../agent-layer/AGENTS.md`. The template is a tracked file, not a heredoc inside the script.
15. Cross-boundary markdown-link sweep — run `test-markdown-links.sh` from both repos; fix host→layer links to `agent-layer/…` and layer→host links via the documented superproject convention (`../` from submodule root); baseline the residue that must wait for full de-Smatchet-ification.
16. Delta-gate baselines that key on paths — `docs/high-integrity/portable-purity-baseline.txt` and the agent-size grandfather list `docs/high-integrity/agent-size-baseline.md` **move to the layer** (seeded in row 8, `git rm`'d host-side in row 11), so after the flip they are regenerated **layer-side** against layer-relative paths, not host-side; the include-cycle baseline is host-only and untouched. One-time, alongside the flip PR.

### Phase D — process + docs

*Detail: [`phase-d-risks.md`](agent-surface-extraction-repo/phase-d-risks.md) — rows 17–20c plus the expanded risk analysis.*

17. `docs/agent-rules/ship-loops.md` (now in the new repo) — new § Two-repo ship-loop: edit flow, pointer-bump PR shape (`chore(agent-layer): bump to <sha>`), what gates run where, `is-pure-docs-diff.sh` classifying a pointer-only bump as docs-tier (the `agent-layer-integration.yml` lane still binds on it — docs-tier classification skips the build, not the gates).
18. New layer-repo workflow `auto-bump.yml` — on every merge to the layer's default branch, opens the `chore(agent-layer): bump to <sha>` PR in Smatchet (grill decision 5; needs a cross-repo token with `contents+pull-requests` write on Smatchet — user provisions, pause exception 3). `docs/agent-rules/merge-gates.md` + `git-janitor` — janitor demotes to **backstop**: proposes a bump only when the layer is ahead of the pin AND no open bump PR exists (auto-bump missed/failed).
19. `docs/STRUCTURE.md`, `docs/PORTABILITY.md` — rewrite the boundary sections: PORTABLE tier now means "lives in `the-unwilling-agentic-bunch`"; extraction checklist becomes "add the submodule".
20. `docs/agent-rules/process-rules.md` § Concurrent interactive sessions — worktree + submodule discipline (each worktree inits its own submodule checkout; HEAD-drift guard extended to the submodule pointer).

### Phase E — verification + residue (below, § Verification)

## Existing utilities reused

*Detail: [`verification.md`](agent-surface-extraction-repo/verification.md) § Existing utilities reused — splits these into reused-as-is vs reused-with-a-scoped-edit, naming the edit and its line numbers per row.*

- `agents/scripts/core/setup-harness.sh` — already the single place that materializes harness adapters (flat hardlinks, codex/pi mirrors); the flip only changes its source dir.
- `scripts/dev/project-config.sh` — already the one config seam; `AGENT_LAYER_ROOT` extends it rather than inventing a second mechanism.
- `agents/scripts/core/merge-gates.sh` + `tests/bats/merge_gates.bats` — reused verbatim as the new repo's own gate-poller + its tests.
- `agents/scripts/core/test-agent-discovery-fixture.sh` — the proven Gate #1 fixture re-run post-flip to prove `.claude/agents` discovery still works through the submodule.
- `docs/PORTABILITY.md` § External path contracts — the pre-existing consumer inventory this plan's Phase A checklist starts from.
- `agents/scripts/core/is-pure-docs-diff.sh` — extended, not duplicated, for pointer-bump classification.

## Extraction sizing

N/A — this plan moves whole directories between repos; it does not split over-cap files into sinks. Root `AGENTS.md` stub (~30 lines) is far under its 150-line cap; the moved `AGENTS.md` keeps its current size in the new repo where the same cap gate follows it.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — zero product-code changes; no `Source/` files touched.
- **Pillar 2 (UI never freezes)**: no impact — no runtime code.
- **Pillar 3 (never crash)**: no impact — no runtime code.
- **Pillar 4 (accessibility)**: no impact — no UI.

## Perf-review-system gates

N/A — pure docs / agentic-shell / CI-config restructure; no `Source/Core/` files in any phase. (1) PR-fast CI: N/A. (2) Pillar-2 scanner: N/A. (3) Dispatcher drain: N/A. (4) Bucket-E visible-cue: N/A. (5) Marker inventory: N/A.

## Risks / non-goals

- **Two-PR friction for every agent-surface edit** — accepted (the point of extraction is independent versioning); measured traffic ~6 layer commits/day over 3 months; mitigated by batching layer edits, the per-merge auto-bump workflow (Phase D row 18), and the janitor backstop. Keeping entries host-side (grill decision 4) removes the hottest write path (525 commits) from the two-repo dance entirely.
- **CI checkout without submodules silently loses the layer** — mitigated twice: `submodules: recursive` added to every workflow in Phase A (before the flip), and `check-harness-provisioned.sh` fails loudly on an empty `agent-layer/`.
- **Windows + worktrees + submodules** — `git worktree` shares `.git/modules` storage; each worktree still needs `submodule update --init`. `worktree.sh cmd_new()` handles it (Phase A row 6); recovery documented in process-rules (Phase D row 20). **Long-path exposure is open, not mitigated**: the repo does *not* require `core.longpaths` today (zero tracked references), and a per-worktree submodule checkout nests one level deeper than usual (`<main>/.git/worktrees/<wt>/modules/agent-layer`) as a full independent clone. The verification matrix repeats the fresh-clone proof under a deliberately long worktree slug to surface it; if it bites, `git config --global core.longpaths true` becomes a documented setup prerequisite.
- **`project-config.sh` chicken-and-egg** — the layer's scripts need host values; resolved by the resolution order (grill decision 2), with the pre-existing `PC_CONFIG_FILE` override kept as **rung 0, highest precedence**: `PC_CONFIG_FILE` (an explicit file, strictly more specific than a root) → `$SMATCHET_PROJECT_CONFIG` env → superproject root (`--show-superproject-working-tree`) → layer repo root (the layer's own config, standalone mode). If superproject detection proves flaky on Windows worktrees, the env rung is the escape hatch; fall back `AGENT_LAYER_ROOT/..` as last resort. Deviation risk logged here deliberately.
- **Delta gates across the move** — the flip PR is a giant delete in Smatchet and the layer repo's history starts at the filter-repo seed; gates that diff `origin/develop` see deletions only (safe). Layer-side gates re-baseline once in Phase B. Rename-tracking (`git log --follow`) works within each repo.
- **`docs/self-improvement/**` auto-exemption stays host-side** — entries never move (grill decision 4), so Smatchet's `.coderabbit.yaml` path_filter + the `selfImpOnly` Bugbot exemption are untouched; the layer repo's fresh `.coderabbit.yaml` carries no such exemption. A Smatchet pointer-bump PR is NOT auto-exempt and rides normal gates (incl. the new integration lane).
- **Submodule working tree is not a write target** — anything an agent session writes under `agent-layer/` is discarded by the next `submodule update`; process-rules (Phase D row 20) must say so explicitly. Entries staying host-side removes the main historical writer from this hazard.
- **`project-config.sh` forks silently** — the file is deliberately dual-homed (host + layer), so two copies drift invisibly: each repo's tests pass against its own copy. Mitigated by Phase B row 8g — the **layer copy is canonical**, the host copy a byte-identical mirror carried by each bump PR, enforced by a `cmp -s` drift gate in `agent-layer-integration.yml` plus a host `test-docs.sh` step. Detail: [`phase-d-risks.md`](agent-surface-extraction-repo/phase-d-risks.md) § Risks.
- **Fork/clone friction** — resolved: both repos public (grill decision 6), relative `../the-unwilling-agentic-bunch.git` URL in `.gitmodules` resolves tokenless for forks and CI. Revisit only if either repo ever goes private (read-token needed then).
- **Non-goals**: full de-Smatchet-ification of portable prose (stays the tracked follow-up in `docs/self-improvement/categories/` — the move does not require it); moving `docs/plans/` (project history, ~252 code-comment citations pin `docs/plans/shipped/` paths — `docs/STRUCTURE.md` § Plan lifecycle rule 4 forbids the rename); moving the self-improvement **entries** (project-specific + hottest write path — grill decision 4); moving `evaluation/` (dated expert-persona report snapshot, zero machine consumers — stays as project reference material); moving `AI_POLICY.md` / `CONTEXT-MAP.md` / `project.config.json` (root-pinned by tools per `docs/STRUCTURE.md` § Naming); publishing the layer as a Claude Code plugin (possible later shape, out of scope).

## Verification

*Detail: [`verification.md`](agent-surface-extraction-repo/verification.md) § Verification — the 30-row per-phase matrix (command, lane, pass criterion) and the stop conditions.*

- **Bucket A (pure-logic ctest)**: N/A — no C++ touched.
- **Bucket E (ImGui Test Engine)**: N/A — no UI touched.
- **Bash-driver / bats**: post-flip, the 61 moved suites green in the layer repo's `agentic-selftests` lane (on the seed commit and every layer PR) and the 33 remaining product-coupled suites green host-side; `merge_gates.bats` moves with the layer and also runs there; `agents/scripts/core/test-agent-discovery-fixture.sh` green post-flip; `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` green on each phase's PR; `agent-layer-integration.yml` proven to fire + pass on a synthetic pointer-bump PR before auto-bump is enabled.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` once on the flip PR (proves no build-system path referenced the moved tree; expected no-op otherwise).
- **Fresh-clone proof**: `fresh-clone-configure-nightly` equivalent run manually on the flip PR — clone + `submodule update --init` + `setup-harness.sh claude-code` + `check-harness-provisioned.sh` all green; agent discovery verified on Claude Code + Codex per `docs/PORTABILITY.md` checklist step 6.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script for the sub-step list), run host-side each phase and layer-side from Phase B on. A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: **RUN 2026-08-29** — six decisions locked (see § Context, Grill outcomes): bats 61/33 split, dual-root config seam, `agent-layer-integration.yml` bump lane, entries stay host-side (scope reversal vs the original draft), per-merge auto-bump, repo `the-unwilling-agentic-bunch` (public). Terms captured in `docs/CONTEXT.md` (agent layer / layer repo / pointer bump / host); decision recorded in `docs/adr/0025-agent-surface-extraction-submodule.md`.
- **Manual residue**: creating the GitHub repo + branch protection + CodeRabbit install on it is manual by nature (cross-repo auth, pause exception 3) — deferred-automation plan: script the reproducible part as `agents/scripts/core/seed-agent-layer-repo.sh` (filter-repo + CI files + labels) so only the `gh repo create` + app installs stay human; entry filed in `docs/self-improvement/categories/tooling/` when Phase B lands.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **De-Smatchet-ification of portable prose** — follow-up already tracked; the submodule move ships with the existing purity baseline.
- **Claude Code plugin packaging** of `agents/_shared/skills` — candidate later distribution channel; no action now.
- **Second consumer project** — the extraction makes it possible; onboarding one is its own plan.
- **Layer release tagging / semver policy** — start with SHA pins; a tagging convention can follow once the two-repo loop has run for a while.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
