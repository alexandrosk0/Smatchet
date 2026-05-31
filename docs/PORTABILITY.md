<!-- index-summary: Portable/project classification + extraction guide for reusing the agentic layer -->
# Portability — reusing the agentic layer in another project

This repo's agentic infrastructure is built to be **lifted into another similar
project**. The framework (delegation, self-improvement loop, harness adapters,
ship-loop, merge-gates, the doc/plan guards) is project-agnostic; the coupling
to *this* project is **values, not design**, and those values live in one file:
[`project.config.json`](../project.config.json) (validated by
`project.config.schema.json`).

**Reuse = copy the PORTABLE tree below + rewrite `project.config.json`.** The
`test-portable-purity.sh` guard (added in a later phase) enforces that portable
files never hardcode a project literal, so the boundary can't silently rot.

## Classification

### PORTABLE — copy as-is (reusable in any similar project)

| Path | Notes |
|---|---|
| `agents/core/` | generic engineering roles; read project specifics from `project.config.json` |
| `agents/_shared/` | skills, token-tracking, templates — zero project coupling |
| `docs/agent-rules/` | delegation / merge-gates / ship-loops / process rules; values come from config |
| `docs/harness/` | IDE adapter setup (claude-code / codex / cursor) |
| `docs/self-improvement/` *(framework)* | the index spec + category structure + counting (`test-backlog-counts.sh`). The **entries** are project-specific; the **framework** is portable |
| generic scripts | `merge-gates.sh` + `.graphql`, `test-shell-lint.sh`, `test-doc-anchors.sh`, `test-agent-contract.sh`, `test-plan-index.sh`, `test-markdown-links.sh`, `project-config.sh`, `setup-harness.sh` |
| `AGENTS.md` *(structure)* | the rulebook shape; project values are prose pointers to `project.config.json` |

### PROJECT-SPECIFIC — re-author per project

| Path | Notes |
|---|---|
| `project.config.json` (+ schema) | **the one file a reuser rewrites** |
| `agents/project/` | subsystem-bound agents (tracker, grid, lua, mcp, offline-sync, unreal, p4-blame, command-system) — replace with the new project's subsystems |
| `docs/plans/` | this project's plans (active + shipped) + the auto-index |
| `docs/self-improvement/categories/*` *(entries)* | this project's friction items |
| `docs/CONTEXT.md`, `docs/adr/` | this project's glossary + decisions |
| `docs/perf/`, `docs/perforce/`, `docs/high-integrity/`, `docs/reference/` | project artifacts |
| project-only scripts | `test-lint-rules.sh` (zone globs), `perf-baseline.sh`, `p4-*.sh` |

**Agent split rule:** `project/` = an agent whose identity is a project subsystem/feature; `core/` = a generic role (it may *mention* project specifics, but those are config-driven). Note: `p4-janitor` lives in `core/` (generic VCS-maintenance, portable to any project whose `vcs.optional_layer` is `p4`), while `p4-blame` is `project/` (bound to this project's blame UI classes).

## Extraction checklist

1. Copy the PORTABLE tree into the new repo (preserve `agents/{core,_shared}/`, `docs/{agent-rules,harness}/`, the self-improvement framework files, the generic scripts, `AGENTS.md`).
2. Write a fresh `project.config.json` (validate against `project.config.schema.json`).
3. Run `scripts/setup-harness.sh <harness>` to regenerate flat agent-discovery links.
4. Author the new project's `agents/project/` subsystem agents.
5. Run `test-portable-purity.sh` — must be green (no project literals leaked into portable dirs).
6. Verify agent discovery on each supported harness (Claude Code + Codex).

## External path contracts (rewrite targets for the reorg phases)

Hardcoded paths that later phases must update (so moves don't break machine consumers):

| Consumer | Path it hardcodes | Phase |
|---|---|---|
| `agents/scripts/core/test-agent-contract.sh` | `agents/*.md` loop **and** `agents/$a.md` named-agent paths | B |
| `scripts/setup-harness.sh` | `link_dir ".claude/agents" "agents"`; codex/cursor `agents/*.md` counters | B |
| `AGENTS.md` | delegation tables naming `agents/<name>.md` | B |
| `agents/scripts/core/test-backlog-counts.sh` | `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`, `docs/self-improvement/categories` (lines 32-33, 40-49) | C |
| `.gitattributes` | `docs/self-improvement/categories/applied.md merge=union` (line 55) | C |
| `agents/scripts/core/sort-applied-md.sh` | `docs/self-improvement/categories/applied.md` | C |
| `.understand-anything/*.json` | knowledge-graph node paths | C/D |
| `agents/scripts/core/test-plan-index.sh` | `PLAN_INDEX_ARCHIVE_DIR`, `PLAN_INDEX_FILE` (config vars at top) | D |
| ~252 `Source/Core/**` comments + ADRs + bats | `docs/plans/shipped/<slug>.md` prose refs | D (via `rewrite-plan-paths.sh`) |
| `.github/workflows/*` | `paths:` filters; `doc-validation.yml` guard list | B–F |

## Gate #1 — agent discovery with subdirs (verified)

Claude Code links `.claude/agents` → the whole `agents/` dir. The `agents/core/` +
`agents/project/` split must not hide agents. The mechanism — `setup-harness.sh`
emits **flat hardlinks** in `.claude/agents/` (one per agent, into the
subdirs) — is proven by `agents/scripts/core/test-agent-discovery-fixture.sh` before any
real move (Phase B). See that test for the fixture.
