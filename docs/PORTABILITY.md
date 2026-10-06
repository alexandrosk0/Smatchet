<!-- index-summary: Portable/project classification + extraction guide for reusing the agentic layer -->
# Portability — reusing the agentic layer in another project

The agentic infrastructure (delegation, self-improvement loop, harness adapters,
ship-loop, merge-gates, the doc/plan guards) is project-agnostic and lives in its
own repository, [`the-unwilling-agentic-bunch`](https://github.com/alexandrosk0/the-unwilling-agentic-bunch),
which this repo mounts as the git submodule `agent-layer/`. The coupling to *this*
project is **values, not design**, and those values live in one file:
[`project.config.json`](../project.config.json) (validated by
`project.config.schema.json`).

**Reuse = add the submodule + write `project.config.json`.** The layer runs
`test-portable-purity.sh` on itself, so its files never gain a new hardcoded
project literal and the boundary can't silently rot.

## Classification

### PORTABLE — in the layer repo, mounted at `agent-layer/`

| Path | Notes |
|---|---|
| `agents/core/` | generic engineering roles; read project specifics from `project.config.json` |
| `agents/_shared/` | skills, token-tracking, templates — zero project coupling |
| `docs/agent-rules/` | delegation / merge-gates / ship-loops / process rules; values come from config |
| `docs/harness/` | IDE adapter setup (claude-code / codex / cursor) |
| `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` *(framework)* | the index spec + category structure + counting (`test-backlog-counts.sh`). The **entries** (`categories/`) stay in the host |
| gate scripts | `agents/scripts/{core,project}/`: `merge-gates.sh` + `.graphql`, `test-shell-lint.sh`, `test-doc-anchors.sh`, `test-agent-contract.sh`, `test-plan-index.sh`, `test-markdown-links.sh`, `setup-harness.sh`, `test-lint-rules.sh`, and the rest. The host also keeps byte-identical, drift-gated mirrors of `scripts/dev/{project-config,test-all,test-docs}.sh` ([`mirrored-paths.txt`](mirrored-paths.txt)) |
| `AGENTS.md` | the rulebook; project values are prose pointers to `project.config.json`. The host's root `AGENTS.md` is a stub that imports it |

### PROJECT-SPECIFIC — re-author per project

| Path | Notes |
|---|---|
| `project.config.json` (+ schema) | **the one file a reuser rewrites** |
| `agents/project/` | subsystem-bound agents (tracker, grid, lua, mcp, offline-sync, unreal, p4-blame, command-system) — replace with the new project's subsystems |
| `docs/plans/` | this project's plans (active + shipped) + the auto-index |
| `docs/self-improvement/categories/*` *(entries)* | this project's friction items |
| `docs/CONTEXT.md`, `docs/adr/` | this project's glossary + decisions |
| `docs/perf/`, `docs/perforce/`, `docs/high-integrity/`, `docs/reference/` | project artifacts |
| project-only scripts | `scripts/dev/`: `perf-baseline.sh`, `p4-*.sh`, `test-subsystem-docs.sh`, the git hooks |

**Agent split rule:** `project/` = an agent whose identity is a project subsystem/feature; `core/` = a generic role (it may *mention* project specifics, but those are config-driven). Note: `p4-janitor` lives in `core/` (generic VCS-maintenance, portable to any project whose `vcs.optional_layer` is `p4`), while `p4-blame` is `project/` (bound to this project's blame UI classes).

## Adoption checklist

1. Add the layer: `git submodule add https://github.com/alexandrosk0/the-unwilling-agentic-bunch.git agent-layer`, then `git submodule update --init --recursive`. Every clone, worktree and CI checkout needs the submodule checked out.
2. Write a fresh `project.config.json` at the superproject root (validate against `project.config.schema.json`).
3. Copy the mirrored scripts (`project-config.sh`, `test-all.sh`, `test-docs.sh`) from the layer into `scripts/dev/` and list them in `docs/mirrored-paths.txt`.
4. Add a root `AGENTS.md` stub that imports `agent-layer/AGENTS.md`.
5. Run `bash agent-layer/agents/scripts/core/setup-harness.sh <harness>`, then `check-harness-provisioned.sh`, which fails on an empty or incomplete mount.
6. Author the new project's `agents/project/` subsystem agents.
7. Verify agent discovery on each supported harness (Claude Code + Codex).

Layer changes are made in the layer repo and reach a host as a gitlink bump (`chore(agent-layer): bump to <sha>`).

*The section below is historical: it listed the path contracts the in-repo reorg had to rewrite before the layer moved out.*

## External path contracts (rewrite targets for the reorg phases)

Hardcoded paths that later phases must update (so moves don't break machine consumers):

| Consumer | Path it hardcodes | Phase |
|---|---|---|
| `agents/scripts/core/test-agent-contract.sh` | `agents/*.md` loop **and** `agents/$a.md` named-agent paths | B |
| `agents/scripts/core/setup-harness.sh` | `link_agents()`: per-file hardlinks into `.claude/agents/` (`link_dir` now serves only `.claude/skills/<name>`); codex/cursor `agents/*.md` counters | B |
| `AGENTS.md` | delegation tables naming `agents/<name>.md` | B |
| `agents/scripts/core/test-backlog-counts.sh` | `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`, `docs/self-improvement/categories` (`INDEX` / `DIR` and the `declare -A FILES` map) | C |
| `.gitattributes` | `docs/self-improvement/categories/applied.md merge=union` | C |
| `agents/scripts/core/sort-applied-md.sh` | `docs/self-improvement/categories/applied.md` | C |
| `.understand-anything/*.json` | knowledge-graph node paths | C/D |
| `agents/scripts/core/test-plan-index.sh` | `PLAN_INDEX_ARCHIVE_DIR`, `PLAN_INDEX_FILE` (the config vars) | D |
| ~252 `Source/Core/**` comments + ADRs + bats | `docs/plans/shipped/<slug>.md` prose refs | D (via `rewrite-plan-paths.sh`) |
| `.github/workflows/*` | `paths:` filters; `doc-validation.yml` guard list | B–F |

## Gate #1 — agent discovery with subdirs (verified)

Claude Code links `.claude/agents` → the whole `agents/` dir. The `agents/core/` +
`agents/project/` split must not hide agents. The mechanism — `setup-harness.sh`
emits **flat hardlinks** in `.claude/agents/` (one per agent, into the
subdirs) — is proven by `agents/scripts/core/test-agent-discovery-fixture.sh` before any
real move (Phase B). See that test for the fixture.
