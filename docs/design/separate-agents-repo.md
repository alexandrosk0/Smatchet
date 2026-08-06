# Separate `agents/` into a standalone repo

## Context

`agents/` (26 agent definitions + `_shared/` skills, templates, token-tracking — ~42 files, ~5,000 LOC) is currently tracked inside the Smatchet repo. The content is increasingly reusable across projects: the workflow patterns in `code-review`, `security-review`, `mechanic`, `architect`, `handoff-implementer`, `pr-iterator`, and the entire skills + token-tracking machinery are generic. Today every project that wants the same loop has to copy-paste these files and drift independently.

User goal: lift the generic core into a standalone repository so it can evolve once and be consumed by Smatchet and future projects. Smatchet-specific instructions (paths, subsystems, build presets, pillar invariants) stay in the Smatchet repo as **overlays** that extend the generic agents via inheritance.

## Design decisions (from clarification)

1. **Full extract** of every agent into a generic upstream repo, but **split** each agent into a generic base (upstream) plus a Smatchet overlay (this repo). Subsystem-specialist agents `inherit` from the generic core.
2. **Git submodule** mounted at `agents/` keeps every existing path reference (`AGENTS.md`, 45 referrers, 2,849 occurrences) working unchanged. No rename of `agents/` itself.
3. **Phased rollout** — three slices, each shippable independently. Phase 1 in detail below; Phase 2 + 3 sketched.

## Architecture

### New repo: name TBD (placeholder `agent-lib` below — confirm before bootstrap)

Naming constraint: the repo hosts **generic, reusable** agents. A Smatchet-branded name (`smatchet-agent-lib`) contradicts the reuse goal. Candidates: `claude-agent-library`, `coding-harness-agents`, `agent-lib`, `agents-md-library`. License TBD (MIT assumed in steps below; confirm before bootstrap).

Layout mirrors today's `agents/` tree:

```text
<agent-lib-name>/
  README.md
  AGENTS.md                    # generic agent-spec doc (lifted from Smatchet's AGENTS.md core)
  <agent>.md                   # 26 generic agent files
  _shared/
    skills/
      grill-with-docs/         # generic
      scratchpad-recall/       # generic
      perf-instrument/         # generic-core, Smatchet macro/preset names replaced with placeholders
      perf-measure/            # same
      perf-gatekeeper/         # same
    templates/
      AgentDebug.h.tmpl        # generic instrumentation template
    token-tracking/            # generic: hook scripts + JSONL parser
```

### Smatchet-side overlay tree: `agents-local/`

Tracked inside Smatchet. Each file is the **Smatchet-specific delta** for a corresponding upstream agent:

```text
agents-local/
  <agent>.smatchet.md          # overlay containing Smatchet paths, subsystems, invariants
  _shared/
    skills/
      perf-instrument/SMATCHET-NOTES.md   # build preset, scope prefix
      perf-measure/SMATCHET-NOTES.md
      perf-gatekeeper/SMATCHET-NOTES.md
      grill-with-docs/SMATCHET-NOTES.md   # already exists today; moves here
```

Not every upstream agent needs an overlay. Purely generic agents (`mechanic`, `code-review`, `security-review`, `p4-blame`, `perf-detective`) ship with no overlay; the upstream file is read as-is.

### Merge mechanism (build-time concat)

`agents/scripts/core/setup-harness.sh` gains a synth step. For each generic agent `<name>` in `agents/`:

```text
.claude/agents/<name>.md  :=  agents/<name>.md  +  ("\n\n---\n\n## Smatchet project addendum\n\n" + agents-local/<name>.smatchet.md  if exists)
```

The synthesised file is a regular file inside `.claude/agents/` (today this is a junction into `agents/`; switching to synthesised regular files is the load-bearing change). The Codex adapter receives the same treatment if/when needed (currently codex reads `agents/` + `AGENTS.md` directly per spec; we publish a generated `agents-resolved/` tree or instruct overlay-aware codex via AGENTS.md prose — Phase 2 decision).

**Edit loop:**
- Generic edit → push to upstream repo → `git submodule update --remote` in Smatchet → re-run `setup-harness.sh`.
- Smatchet-overlay edit → `agents-local/<name>.smatchet.md` → re-run `setup-harness.sh`.
- Re-running setup-harness is cheap (file concat). Optionally add a `make agents` shortcut and a file-watch mode.

Trade-off: loses today's junction "edit-and-it's-live" property for the upstream side. Mitigated by (a) overlay edits stay live-ish, (b) re-synth is one command, (c) most edits land in overlays (Smatchet-specific tuning).

## Phase 1 — Extract `agents/_shared/` only (detailed)

**Why first**: lowest coupling. `_shared/` is referenced from `setup-harness.sh` (a single linker block), `.claude/settings.json` (hook paths), and `scripts/dev/test-agent-contract.sh` (one Python-test invocation). No agent-prompt restructuring needed. Validates the submodule + overlay mechanism end-to-end before the heavier agent split.

### Slice 1.1 — Bootstrap upstream repo

1. Create empty GitHub repo `<agent-lib-name>` under user's namespace (or org TBD).
2. Initialize with `README.md` + `LICENSE` (license TBD — confirm before push).
3. In a fresh local clone of the upstream, create the skeleton `_shared/` tree by copying from Smatchet's current `agents/_shared/`:
   - `_shared/skills/grill-with-docs/` (all files except `SMATCHET-NOTES.md`)
   - `_shared/skills/scratchpad-recall/`
   - `_shared/skills/perf-instrument/` (rewrite Smatchet-specific bits with `<PERF_SCOPE_MACRO>` / `<BUILD_PRESET>` placeholders documented in `README.md`)
   - `_shared/skills/perf-measure/` (same)
   - `_shared/skills/perf-gatekeeper/` (same)
   - `_shared/templates/AgentDebug.h.tmpl` (rename from `SmatchetAgentDebug.h.tmpl`; placeholder-ise the include guard + namespace)
   - `_shared/token-tracking/` (verbatim — already generic)
4. Tag `v0.1.0`.

### Slice 1.2 — Mount as submodule in Smatchet

1. In Smatchet worktree: `git submodule add -- <upstream-url> agents/_shared` — wait: collision with existing tree. Steps:
   - `git rm -r --cached agents/_shared`
   - Delete `agents/_shared/` working-tree contents (after confirming submodule clone will repopulate).
   - `git submodule add -- <upstream-url> agents/_shared`
   - `git commit -m "chore(agents): extract _shared to <agent-lib-name> submodule"`
2. Create `agents-local/_shared/skills/{grill-with-docs,perf-instrument,perf-measure,perf-gatekeeper}/SMATCHET-NOTES.md` from the equivalents that used to live in `agents/_shared/skills/*/`. Track these.
3. **Refresh hard links** — `agents/_shared/token-tracking/*.py` files are hard-linked (not junctioned) into `.claude/hooks/` + `.claude/skills/` by today's `setup-harness.sh` (explore confirmed at `agents/scripts/core/setup-harness.sh:140-143`). Submodule init creates **new inodes**; the existing hard links in `.claude/` point to deleted inodes and are silently broken until re-linked. Run `bash agents/scripts/core/setup-harness.sh claude-code` immediately after `git submodule add` to recreate the hard links. Verify with `python .claude/hooks/agent-token-log.py --help`.

### Slice 1.3 — Wire the overlay merge into setup-harness

Files to modify:

- [`agents/scripts/core/setup-harness.sh`](../../agents/scripts/core/setup-harness.sh) — add a `synth_overlay()` helper that reads upstream `agents/_shared/skills/<skill>/SKILL.md` and concatenates `agents-local/_shared/skills/<skill>/SMATCHET-NOTES.md` onto `.claude/skills/<skill>/SKILL.md`. Continue using junctions for purely-generic skills (`scratchpad-recall`, `token-tracking`).
- [`agents/scripts/core/test-setup-harness.sh`](../../agents/scripts/core/test-setup-harness.sh) — add cases covering the synth step.
- [`docs/harness/SETUP.md`](../harness/SETUP.md) — document submodule init step (`git submodule update --init --recursive`).

Reuse existing helpers in `setup-harness.sh` (`link_dir`, `link_file`, `copy_template`) — see explore findings.

### Slice 1.4 — Update doc cross-refs that point into `agents/_shared/`

`scripts/dev/test-agent-contract.sh:147` — update Python-test invocation path (or leave as-is if submodule preserves path). Verify.

Audit `docs/AGENT_TOKEN_TRACKING.md` references to `agents/_shared/token-tracking/`. Path stays valid via submodule; no rewrite needed.

### Slice 1.5 — CI update

- `.github/workflows/doc-validation.yml` — confirm `paths: ['agents/_shared/token-tracking/**']` triggers still fire (submodule pointer changes in Smatchet repo show as `agents/_shared` diff, not nested).
- `.github/workflows/build-and-test.yml` — same.

If submodule path triggers don't fire on upstream-content changes (they won't — Smatchet CI sees the submodule SHA pointer, not the file contents), accept that contract: changes inside the upstream repo are validated by the upstream repo's own CI, not Smatchet's. Add an upstream CI workflow in the new repo mirroring the relevant doc-validation + Python-test gates.

### Phase 1 verification

1. `git submodule update --init --recursive` populates `agents/_shared/` cleanly.
2. `bash agents/scripts/core/setup-harness.sh claude-code` regenerates `.claude/agents/` + `.claude/skills/` with merged overlay content visible at the bottom of each `SKILL.md`.
3. `bash agents/scripts/core/test-setup-harness.sh` passes.
4. `bash scripts/dev/test-agent-contract.sh` passes (no path regressions).
5. `python agents/_shared/token-tracking/tests/test_infer_outcome.py` runs from submodule path.
6. Smoke test the skill-merge mechanism (Phase 1 scope is skills only — agent `.md` files at top level are unchanged this phase). Invoke the `perf-measure` **skill** via `Skill` tool and confirm `SKILL.md` content surfaces both the upstream generic body and the Smatchet `SMATCHET-NOTES.md` addendum (build preset, perf-scope prefix, scenario list). Repeat for `grill-with-docs`.
7. Commit submodule init + overlay-tree + setup-harness changes as one PR titled `chore(agents): extract _shared/ to <agent-lib-name> submodule (Phase 1)`.

## Phase 2 — Extract generic + thin-wrapper agents (sketch)

**Scope**: move the 5 generic agents (`code-review`, `security-review`, `mechanic`, `p4-blame`, `perf-detective`) and 8 thin-wrapper agents (`architect`, `command-system`, `handoff-implementer`, `lua-binder`, `mcp-toolsmith`, `pr-iterator`, `perf-measure`, `tracker-backend`) from `agents/*.md` into the upstream repo.

**Mechanism**:
1. Split each thin-wrapper agent into generic base (upstream) + Smatchet overlay (`agents-local/<name>.smatchet.md`).
2. Extend `setup-harness.sh` synth step to concat upstream agent + overlay into `.claude/agents/<name>.md` (regular file replacing today's junction).
3. Replace the Smatchet-internal `agents/` junction model with the submodule + overlay merge.
4. Update `AGENTS.md` § Agent file locations + § Harness adapter to describe the new flow.

**Open questions for Phase 2 plan-doc**:
- Codex adapter: codex reads `agents/*.md` per the agents.md spec; does it tolerate the submodule + overlay split, or do we need to publish a resolved `agents-resolved/` tree?
- Live-edit ergonomics: do we add `make agents` + a file-watch daemon, or rely on agents pulling the synth step before spawn?

## Phase 3 — Extract Smatchet-deep agents (sketch)

**Scope**: the 8 deeply coupled agents (`debug-detective`, `build-doctor`, `unreal-bridge`, `test-author`, `coderabbit-triage`, `perf-gatekeeper`, `git-janitor`, `test-rig`).

**Mechanism**: ruthlessly factor each agent into a generic workflow base (decision trees, output contracts, escalation patterns) shipped upstream, plus a substantial Smatchet overlay (build presets, dual-target rules, pillar invariants, exact paths, sanitizer recipes). Expect the overlay to be larger than the upstream base for these agents — that's fine.

**Pre-condition**: Phase 2 has validated the merge mechanism at scale; the cost model and tooling are settled.

## Critical files

Phase 1 touches:

- New: [`<agent-lib-name>` repo] — bootstrap.
- Smatchet repo:
  - `.gitmodules` (new entry)
  - `agents/_shared/` → submodule
  - `agents-local/_shared/skills/{grill-with-docs,perf-instrument,perf-measure,perf-gatekeeper}/SMATCHET-NOTES.md` (new)
  - [`agents/scripts/core/setup-harness.sh`](../../agents/scripts/core/setup-harness.sh) — add `synth_overlay()` helper
  - [`agents/scripts/core/test-setup-harness.sh`](../../agents/scripts/core/test-setup-harness.sh)
  - [`docs/harness/SETUP.md`](../harness/SETUP.md)
  - [`.github/workflows/doc-validation.yml`](../../.github/workflows/doc-validation.yml)
  - [`.github/workflows/build-and-test.yml`](../../.github/workflows/build-and-test.yml)

## Existing utilities to reuse

- `link_dir()` / `link_file()` / `copy_template()` in [`agents/scripts/core/setup-harness.sh`](../../agents/scripts/core/setup-harness.sh).
- Junction-vs-symlink platform branching already in place.
- `.gitignore` already covers `.claude/`, `.codex/`, `.cursor/` — no change needed for adapter dirs.
- Existing `SMATCHET-NOTES.md` pattern in `agents/_shared/skills/grill-with-docs/` becomes the template for all overlay notes.

## Risks

1. **Submodule UX friction**: contributors who skip `git submodule update --init` see broken `agents/_shared`. Mitigate with `agents/scripts/core/setup-harness.sh` running submodule init unconditionally + a SessionStart hook warning when submodule SHA drifts.
2. **CI doesn't trigger on upstream-content changes**: by design — submodule SHA pointers don't propagate. Solved by giving upstream its own CI; Smatchet CI only validates the integration.
3. **Overlay drift**: Smatchet overlays may go stale as upstream agents evolve. Mitigate with `scripts/dev/test-agent-contract.sh` extended to verify each upstream agent has either a current overlay or is explicitly marked `no-overlay`.
4. **Token-tracking hooks break transiently**: `.claude/settings.json` references `$CLAUDE_PROJECT_DIR/.claude/hooks/agent-token-log.py` + `skill-load-log.py`. These are **hard links** (not junctions) into `agents/_shared/token-tracking/*.py` per `agents/scripts/core/setup-harness.sh:140-143`. Submodule add creates new inodes for the populated files; the old hard links point to deleted inodes and silently break. Mitigation: Slice 1.2 step 3 mandates re-running `setup-harness.sh` immediately after `git submodule add`. Without that step, token-tracking goes silently dark until the next setup-harness run.
5. **Loss of live-edit on junction-backed agents**: addressed by overlay-only edits being live-ish; upstream edits require submodule push/pull. Acceptable trade-off; document in `docs/harness/SETUP.md`.
6. **Plan stress-test not run**: this plan has not been through the `grill-with-docs` skill against `docs/CONTEXT.md` + ADRs. Required before approval per `AGENTS.md` § Project rules § Plan stress-test. Add a pass before implementation begins.

## Verification (Phase 1)

End-to-end checklist:

1. `git submodule update --init --recursive` from a fresh Smatchet clone populates `agents/_shared/` from the upstream repo, no errors.
2. `bash agents/scripts/core/setup-harness.sh claude-code` regenerates `.claude/agents/`, `.claude/skills/`, `.claude/hooks/`. Synth step merges upstream skill prose + Smatchet `SMATCHET-NOTES.md` overlays.
3. `bash agents/scripts/core/test-setup-harness.sh` — green, including new synth-step cases.
4. `bash scripts/dev/test-agent-contract.sh` — green; no path regressions for non-touched agents.
5. `python agents/_shared/token-tracking/tests/test_infer_outcome.py` — green from submodule path.
6. Smoke test the skill-merge mechanism (Phase 1 scope is skills only — agent `.md` files at top level are unchanged this phase). Invoke the `perf-measure` **skill** via `Skill` tool and confirm `SKILL.md` content surfaces both the upstream generic body and the Smatchet `SMATCHET-NOTES.md` addendum (build preset, perf-scope prefix, scenario list). Repeat for `grill-with-docs`.
7. CI: `.github/workflows/doc-validation.yml` + `.github/workflows/build-and-test.yml` pass on the PR; upstream GitHub repo's own CI passes on `v0.1.0` tag.
8. `bash scripts/dev/is-pure-docs-diff.sh develop` correctly classifies the PR's diff — confirms the docs/CI tooling still understands the new tree.
9. Manual: orchestrator runs a delegated `mechanic` task end-to-end — agent is fully functional from submodule + overlay.

## Out of scope (this plan)

- Phase 2 + Phase 3 detail. Sketched here; each gets its own `docs/design/separate-agents-repo-phase-{2,3}.md` when Phase 1 ships.
- Migration of `AGENTS.md` itself into the upstream repo. Deferred — `AGENTS.md` carries enough Smatchet-specific content (UX pillars, merge gates, project rules) that it stays in this repo and references upstream agent docs by URL once they exist.
- Renaming `agents/` to something else. Decided against — preserves 2,849 path references unchanged.

## Deferred-symbol audit (plan-slimming hygiene)

Per `AGENTS.md` § Process rules § Scope-reduction edits: this plan details Phase 1 and **defers Phase 2 + Phase 3** (plus the `AGENTS.md`-migration and `agents/`-rename items in § Out of scope) to later plan-docs. Grepped every symbol named in the deferred work across `docs/CONTEXT.md`, `docs/adr/`, `agents/*.md`, and `docs/backlog/agent-self-improvement/` to confirm no live doc already describes the deferred work as shipped:

| Deferred symbol | Source (deferred phase) | Hits in audited trees | Action |
|---|---|---|---|
| `agents-local/` (overlay tree) | Phase 2/3 mechanism | 0 | none — not yet referenced |
| `<agent-lib-name>` / `agent-lib` (upstream repo) | bootstrap, named in 2/3 | 0 | none |
| `agents-resolved/` (codex resolved tree) | Phase 2 open question | 0 | none |
| `separate-agents-repo-phase-{2,3}.md` (future plan docs) | Phase 2/3 split | 0 | none — created when Phase 1 ships |
| `synth_overlay()` / overlay-merge synth step | Phase 1.3 + 2 | 0 | none — not yet in `setup-harness.sh` |
| `no-overlay` marker | Risk 3 mitigation (Phase 2) | 0 | none |

Result: **clean** — the feature is wholly unimplemented, so no `CONTEXT*.md` / ADR / agent / backlog entry yet describes the upstream repo, overlay tree, or submodule split as current truth. Nothing to revise or delete. Re-run this audit when Phase 1 lands (the submodule + `agents-local/` tree become real, and the Phase 2/3 sketches must not leak into live docs as decided).

Note: `agents/_shared/skills/grill-with-docs/SMATCHET-NOTES.md` already exists today and is correctly described (§ Architecture) as moving into the overlay tree — it is not a deferred symbol.
