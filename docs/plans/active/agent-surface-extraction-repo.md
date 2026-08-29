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

1. **Scope — full agent surface**: `agents/{core,project,_shared,scripts}/`, `docs/agent-rules/`, `docs/harness/`, `docs/self-improvement/` (framework **and** entries), the generic gate scripts, and the `AGENTS.md` rulebook content. Smatchet keeps `project.config.json` (+ schema), `AI_POLICY.md`, `CONTEXT-MAP.md`, `docs/plans/`, subsystem leaf docs, and project-only scripts.
2. **Mechanism — git submodule** mounted at a single directory (`agent-layer/`) in the Smatchet root, pinned by SHA, bumped by explicit PRs.
3. **History — preserved** via `git filter-repo` when seeding the new repo.

After this lands: the agent surface is a standalone repo (working name **`smatchet-agent-layer`**, rename-able before creation) with its own CI and merge gates; Smatchet consumes it as `agent-layer/` and any other project can consume it the same way.

## Approach

**Keep the internal layout of the new repo byte-identical to the current in-tree layout** (`agents/…`, `docs/agent-rules/…`, `docs/harness/…`, `docs/self-improvement/…`, `AGENTS.md` at the new repo's root). Every intra-surface relative link and script path then survives the move unchanged; only **cross-boundary** references (surface ↔ product) need rewriting, and those are exactly the "External path contracts" table already maintained in `docs/PORTABILITY.md` plus the workflow/hook invocation paths.

**De-risk with an indirection phase before the move.** Phase A introduces one variable — `AGENT_LAYER_ROOT` (exported by `project-config.sh`, default: repo root) — and rewrites every consumer (workflows, `scripts/dev/*`, hooks, `setup-harness.sh`, docs where load-bearing) to address the surface through it, while the files have not moved. CI proves the indirection green. The actual flip (Phase C) then changes one value (`AGENT_LAYER_ROOT=agent-layer`) plus the `git rm` + `git submodule add`, instead of a big-bang path rewrite entangled with the move itself.

**Two-repo ship-loop is the real ongoing cost** and is designed in, not discovered later: an agent-surface edit becomes (1) a PR in `smatchet-agent-layer` gated by that repo's own CI (agentic-selftests, shell-lint, doc-validation subset, merge-gates.sh reused on itself), then (2) a one-line submodule-pointer-bump PR in Smatchet. `docs/agent-rules/ship-loops.md` gets a new § Two-repo ship-loop codifying this, and `git-janitor` learns to propose pointer bumps. The trade-off accepted: per-edit friction up, in exchange for independent versioning/release of the layer and true reuse.

## Files to modify

Grouped by phase. Path lists below are the known consumers (from `docs/PORTABILITY.md` § External path contracts + a workflow grep); **Phase A opens with an exhaustive repo-wide sweep** (`rg -l 'agents/(core|project|_shared|scripts)|docs/agent-rules|docs/harness|docs/self-improvement'`) whose full hit-list is committed as the phase's checklist — the rows here are the anchors, not the closed set.

### Phase A — indirection prep (Smatchet PR, no file moves)

1. `scripts/dev/project-config.sh` — export `AGENT_LAYER_ROOT` (default `$REPO_ROOT`) + `PC_AGENT_LAYER_ROOT`; add superproject-root detection (`git rev-parse --show-superproject-working-tree` fallback) so the same script works from inside the future submodule.
2. `.github/workflows/*.yml` (14 files referencing `agents/`: `agentic-selftests`, `build-and-test`, `codeql`, `cr-oob-review-backfill`, `dependabot-auto-merge`, `doc-validation`, `dup-scan`, `issue-janitor`, `locks-render`, `lock-staleness`, `mobile-security`, `perf-pr-fast`, `plan-lock-gate`, `shell-lint`) — route script invocations through the variable; add `submodules: recursive` to **every** `actions/checkout` step repo-wide (harmless now, mandatory after the flip).
3. `scripts/dev/pre-ship.sh`, `scripts/dev/test-all.sh`, `agents/scripts/project/test-lint-rules.sh` — invoke `agents/scripts/…` via `AGENT_LAYER_ROOT`.
4. `agents/scripts/core/setup-harness.sh` — `link_dir ".claude/agents" "$AGENT_LAYER_ROOT/agents"`; codex/cursor/pi mirror generators likewise; hook-command paths it writes into `.claude/settings.json` likewise.
5. `agents/scripts/core/{test-agent-contract.sh,test-backlog-counts.sh,sort-applied-md.sh,test-plan-index.sh,check-harness-provisioned.sh}` — path config vars via `AGENT_LAYER_ROOT`; extend `check-harness-provisioned.sh` to fail loudly when the layer dir is missing/empty (the "checkout forgot submodules" guard).
6. `scripts/dev/new-session.sh` (`nsc`) — run `git submodule update --init --recursive` in freshly created worktrees (no-op pre-flip).
7. `.gitattributes` — `merge=union` row for `docs/self-improvement/categories/applied.md` stays valid pre-flip; post-flip twin added in the new repo (Phase B).

### Phase B — seed `smatchet-agent-layer` (new repo; user creates it — pause exception 3, cross-repo mutation)

8. New repo seeded by `git filter-repo` from a fresh Smatchet clone: `--path agents/ --path docs/agent-rules/ --path docs/harness/ --path docs/self-improvement/ --path AGENTS.md` (+ the generic `scripts/dev/` gate scripts identified portable in `docs/PORTABILITY.md`: `merge-gates` pair already under `agents/scripts/core/`, plus `project-config.sh` copy — see Deviations risk below). History preserved; layout unchanged.
9. New repo CI: `agentic-selftests.yml`, `shell-lint.yml`, and a doc-validation subset (anchors / agent-contract / backlog-counts / portable-purity / markdown-links scoped to the moved tree) — all reusing the moved scripts on themselves; branch protection + `merge-gates.sh` self-hosted as its own gate-poller; `.coderabbit.yaml` carried over (the `docs/self-improvement/**` auto-exemption path_filter now lives here, where those paths now are).
10. New repo root additions: `README.md` (consumption contract: mount at `agent-layer/`, host must provide `project.config.json` at superproject root), `LICENSE`, minimal `project.config.json.example`.

### Phase C — the flip (Smatchet PR)

11. `git rm -r agents/ docs/agent-rules/ docs/harness/ docs/self-improvement/` + `git submodule add <url> agent-layer` + `.gitmodules` (relative URL so forks/CI resolve).
12. `scripts/dev/project-config.sh` (or `project.config.json` § paths) — `AGENT_LAYER_ROOT=agent-layer`.
13. `AGENTS.md` (root) — becomes a thin stub: harness auto-load entry point, `@agent-layer/AGENTS.md`-style import for Claude Code via the regenerated `.claude/CLAUDE.md`, prose pointer for Codex/others. Root stub stays under the 150-line cap trivially.
14. `.claude/CLAUDE.md` template inside `setup-harness.sh` — import path `../agent-layer/AGENTS.md`.
15. Cross-boundary markdown-link sweep — run `test-markdown-links.sh` from both repos; fix host→layer links to `agent-layer/…` and layer→host links via the documented superproject convention (`../` from submodule root); baseline the residue that must wait for full de-Smatchet-ification.
16. Delta-gate baselines that key on paths (`portable-purity-baseline.txt`, agent-size grandfather list `docs/high-integrity/agent-size-baseline.md`, include-cycle baseline untouched) — regenerate where the move re-keys entries; one-time, in the flip PR (layer-side copies live in the new repo after Phase B).

### Phase D — process + docs

17. `docs/agent-rules/ship-loops.md` (now in the new repo) — new § Two-repo ship-loop: edit flow, pointer-bump PR shape (`chore(agent-layer): bump to <sha>`), what gates run where, `is-pure-docs-diff.sh` classifying a pointer-only bump as docs-tier.
18. `docs/agent-rules/merge-gates.md` + `agents/scripts/core/git-janitor.sh` — janitor proposes (never auto-merges) a pointer bump when the layer's default branch is ahead of the pinned SHA.
19. `docs/STRUCTURE.md`, `docs/PORTABILITY.md` — rewrite the boundary sections: PORTABLE tier now means "lives in `smatchet-agent-layer`"; extraction checklist becomes "add the submodule".
20. `docs/agent-rules/process-rules.md` § Concurrent interactive sessions — worktree + submodule discipline (each worktree inits its own submodule checkout; HEAD-drift guard extended to the submodule pointer).

### Phase E — verification + residue (below, § Verification)

## Existing utilities reused

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

- **Two-PR friction for every agent-surface edit** — accepted (the point of extraction is independent versioning); mitigated by batching layer edits and the janitor pointer-bump proposal (Phase D).
- **CI checkout without submodules silently loses the layer** — mitigated twice: `submodules: recursive` added to every workflow in Phase A (before the flip), and `check-harness-provisioned.sh` fails loudly on an empty `agent-layer/`.
- **Windows + worktrees + submodules** — `git worktree` shares `.git/modules` storage; each worktree still needs `submodule update --init`. `nsc` handles it (Phase A row 6); recovery documented in process-rules (Phase D row 20). Risk of long-path issues on Windows: `core.longpaths` already required by the repo.
- **`project-config.sh` chicken-and-egg** — the layer's scripts need host values; the host provides `project.config.json` at superproject root. Superproject detection in Phase A row 1 resolves it; if `--show-superproject-working-tree` proves flaky on Windows worktrees, fall back to `AGENT_LAYER_ROOT/..`. Deviation risk logged here deliberately.
- **Delta gates across the move** — the flip PR is a giant delete in Smatchet and the layer repo's history starts at the filter-repo seed; gates that diff `origin/develop` see deletions only (safe). Layer-side gates re-baseline once in Phase B. Rename-tracking (`git log --follow`) works within each repo.
- **`docs/self-improvement/**` auto-exemption semantics move repos** — the CR/Bugbot exemption now applies in the layer repo (where those paths live); a Smatchet pointer-bump PR is NOT auto-exempt and rides normal gates. Called out so nobody re-adds the path filter host-side.
- **Fork/clone friction** — submodule URL must be a relative `../smatchet-agent-layer.git` in `.gitmodules` so forks and CI tokens resolve it; if the layer repo is private, CI needs a token with read access (user decision at Phase B).
- **Non-goals**: full de-Smatchet-ification of portable prose (stays the tracked follow-up in `docs/self-improvement/categories/` — the move does not require it); moving `docs/plans/` (project history, ~252 code-comment citations pin `docs/plans/shipped/` paths — `docs/STRUCTURE.md` § Plan lifecycle rule 4 forbids the rename); moving `AI_POLICY.md` / `CONTEXT-MAP.md` / `project.config.json` (root-pinned by tools per `docs/STRUCTURE.md` § Naming); publishing the layer as a Claude Code plugin (possible later shape, out of scope).

## Verification

- **Bucket A (pure-logic ctest)**: N/A — no C++ touched.
- **Bucket E (ImGui Test Engine)**: N/A — no UI touched.
- **Bash-driver / bats**: `tests/bats/merge_gates.bats` green in BOTH repos post-flip; `agents/scripts/core/test-agent-discovery-fixture.sh` green post-flip; `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` green on each phase's PR; `agentic-selftests` lane green in the layer repo on its seed commit.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` once on the flip PR (proves no build-system path referenced the moved tree; expected no-op otherwise).
- **Fresh-clone proof**: `fresh-clone-configure-nightly` equivalent run manually on the flip PR — clone + `submodule update --init` + `setup-harness.sh claude-code` + `check-harness-provisioned.sh` all green; agent discovery verified on Claude Code + Codex per `docs/PORTABILITY.md` checklist step 6.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script for the sub-step list), run host-side each phase and layer-side from Phase B on. A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before Phase A implementation starts; record the outcome here. Required — not yet run at authoring time.
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
