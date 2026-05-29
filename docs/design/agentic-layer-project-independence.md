# Plan — Documentation + agentic-layer reorganization for **project-independence**

> **Slug**: `agentic-layer-project-independence`
> **Status**: approved (2026-05-29); implementation phased (A–F). This doc is the canonical plan; the session plan-file copy is ephemeral.

## Context

Two intertwined goals:

1. **Reuse the agentic layer in another similar project.** Today the agentic content (agent defs, rules, harness, self-improvement framework, scripts) is ~60% portable, but the project-specific 40% is **hardcoded values scattered inline** — build presets, paths, perf thresholds (`6.94ms`/`16.67ms`/`100ms`), lint zone-globs, the `SMATCHET_*` env prefix, and 8 subsystem-bound agents. There is **no project-config seam** today. The framework (delegation, self-improvement, harness adapters, ship-loop, merge-gates) is already project-agnostic — the coupling is in *values, not design*.
2. **Fix the docs drift** that surfaced this session: `docs/backlog/` conflates plan-tracking with the self-improvement loop; the shipped-plan index (`BACKLOG_PLANS.md §1`) indexes only ~20/53 plans, frozen 2026-05-17; four single-file orphan dirs; nothing enforces placement.

**Target outcome:** a repo where the **portable agentic layer is physically separated** from the **project-specific layer**, with all project values in **one `project.config.json`**, so reuse = "copy the portable tree + write a new config." Plus a clean, CI-enforced `docs/` taxonomy that can't re-drift. `AGENTS.md` and `agents/` stay at repo root.

**Locked decisions:** boundary + config seam (no `${VAR}` templating); physical `agents/core/` + `agents/project/` split; machine-readable `project.config.json`; full `docs/design`→`docs/plans` rename; consolidate orphan dirs; keep `docs/high-integrity/`.

## Target architecture — the portable / project boundary

```text
/AGENTS.md              root; PORTABLE structure + prose pointers to project.config.json
/project.config.json    NEW; the ONE home for project-specific values (see schema below)
/agents/                root (symlink-backed into .claude/)
  _shared/              PORTABLE  (skills, token-tracking, templates)
  core/                 PORTABLE  generic roles — read specifics from config
                        code-review, security-review, mechanic, test-author, git-janitor,
                        coderabbit-triage, architect, debug-detective, build-doctor,
                        spike-hunter, perf-detective, perf-instrument, perf-measure,
                        perf-gatekeeper, test-rig
  project/              PROJECT   subsystem-bound agents
                        tracker-backend, grid-engine, lua-binder, mcp-toolsmith,
                        offline-sync, unreal-bridge, p4-blame, p4-janitor
docs/
  STRUCTURE.md          NEW  normative taxonomy + portable/project tag per dir + guard map
  PORTABILITY.md        NEW  extraction guide: what to copy, what to re-author, the config keys
  CONTEXT.md            PROJECT (glossary)            adr/  PROJECT
  agent-rules/          PORTABLE (rules; project values come from config)
  harness/              PORTABLE (IDE adapters)
  self-improvement/     framework PORTABLE / entries PROJECT
    AGENT_SELF_IMPROVEMENT.md ; categories/ (was docs/backlog/agent-self-improvement/)
  plans/                PROJECT     INDEX.md (auto-indexed) ; active/ (was design/) ; shipped/ (was design/archive/)
  guides/               mixed       offline-builds, perf-workflow, caveman, agent-token-tracking
  reference/            PROJECT     evaluation snapshot, archived scenario
  perf/  perforce/  high-integrity/   PROJECT (unchanged locations)
```

Rule of thumb for the agent split: **`project/` = an agent whose identity is a Smatchet subsystem/feature; `core/` = a generic engineering role** (it may mention Smatchet specifics, but those become config-driven).

### `project.config.json` schema (sketch)

Single source for the values currently inlined. Scripts read it where cheap; AGENTS.md/rules prose points to it for parameterized facts (no `${VAR}` substitution — prose stays readable, the config is the canonical value table).

```json
{
  "project": { "name": "Smatchet", "env_prefix": "SMATCHET" },
  "build": { "presets": ["ninja-iter-msvc","ninja-test-msvc","ninja-msvc-asan"],
             "targets": ["SmatchetStandalone","SmatchetCore_DX12"],
             "exe_path": "build/{preset}/Smatchet.exe" },
  "perf": { "frame_budget_ms": 6.94, "fps_floor_ms": 16.67, "freeze_ms": 100,
            "baselines_dir": "docs/perf/baselines", "policy": "docs/perf/regression-policy.json" },
  "lint": { "zones": { "strict": ["Source_Core/src/Tracker", "..."], "light": ["Source_Core/src/Ui","..."],
                       "exempt": ["ThirdParty","build"] },
            "rules": ["no-printf-stderr","no-raw-new","define-imgui","deviation-overdue"],
            "deviation_keyword": "SMATCHET_DEVIATION", "baseline": "docs/high-integrity/baseline.md" },
  "vcs": { "primary": "git", "optional_layer": "p4" },
  "subsystems": [ { "name":"tracker", "agents":["tracker-backend"] }, "..." ],
  "agents": { "enabled_project": ["tracker-backend","grid-engine","..."] }
}
```

A small `scripts/dev/project-config.sh` loader exports these as shell vars (e.g. `PC_PERF_BUDGET_MS`) so scripts source one file instead of hardcoding.

## Feasibility gate (verify FIRST, before any agent move)

**Gate #1 — agent discovery with subdirs.** Claude Code discovers subagents from `.claude/agents/*.md` (likely flat). Moving canonical defs into `agents/core/` + `agents/project/` must not hide them. Resolution: canonical source stays split; **`scripts/setup-harness.sh` generates flat discovery symlinks** in `.claude/agents/` pointing into the subdirs (one link per agent). Verify Claude Code resolves agents through these flat links before committing the split; if recursive discovery already works, skip the flattening. Fallback if neither works: revert to the `tier:` frontmatter approach (no moves) — but try the split first per the locked decision.

## Phased plan (each phase = its own PR(s); docs/scripts PRs pass via the merged Pattern C gate)

- **Phase A — config seam + classification (no moves).** Add `project.config.json` + `scripts/dev/project-config.sh` loader (capturing today's values). Add `docs/PORTABILITY.md` classifying every agentic file portable/project + the coupling inventory + extraction checklist. Add the plan-index generator `scripts/dev/test-plan-index.sh` pointed at the *current* paths and `--fix` the ~33-plan drift in place. Verify Gate #1. Pure additive — lowest risk, proves the seam.
- **Phase B — agent split.** `git mv` agents into `agents/core/` + `agents/project/`; update `scripts/setup-harness.sh` to emit flat discovery symlinks; update `test-agent-contract.sh`/`test-doc-anchors.sh` globs + any `agents/<name>.md` path refs (delegation tables, CI `paths:`). Verify discovery + contract tests green.
- **Phase C — backlog split.** `git mv` `AGENT_SELF_IMPROVEMENT.md` + `agent-self-improvement/` → `docs/self-improvement/{,categories/}`. Fix the ~135 refs (12 agent prompts, `test-backlog-counts.sh:32-33`, `.gitattributes:55`, `sort-applied-md.sh`, `AGENTS.md`, knowledge-graph JSON). Verify `git grep docs/backlog/agent-self-improvement` → 0.
- **Phase D — plans home (heavy).** `git mv` `design/`→`plans/active/`, `design/archive/`→`plans/shipped/`, `BACKLOG_PLANS.md`→`plans/INDEX.md`. Global `sed` across tracked files incl. ~252 `Source_Core/**` comments — **ordered: rewrite `docs/design/archive`→`docs/plans/shipped` BEFORE `docs/design`→`docs/plans/active`** (else paths nest wrong). Repoint the index generator. Dismiss the high-integrity/test-delta delta gates only via the documented `tests-out-of-band` label (comment-only change). Verify `test-plan-ref-integrity.sh` → 0 dangling + full build green.
- **Phase E — orphan consolidation + hub demotion.** Create `guides/` + `reference/`; `git mv` `dev/offline-builds.md`, `PERF_WORKFLOW.md`, `CAVEMAN.md`, `AGENT_TOKEN_TRACKING.md`, evaluation snapshot, archived scenario; remove emptied dirs; fix ~117 hub refs. Keep `docs/high-integrity/` + `CONTEXT.md` (root).
- **Phase F — config-wire (where cheap) + rules + lock.** Source `project-config.sh` in the scripts that already use env-vars (merge-gates, perf-run, test-backlog-counts) and replace the cheapest inline values; leave heavy rewiring (test-lint-rules zone globs) as documented follow-ups. Add `docs/STRUCTURE.md` (taxonomy + portable/project tags + per-row enforcing guard + "shipped/ never renamed again" record). Stub into `process-rules.md` + 1-line `AGENTS.md` pointer (keeps `§` anchors resolving). Flip the log-only guards to fail.

## Enforcement guards (the "written rules", all in cheap `doc-validation.yml`)

| Guard | Script | Checks |
|---|---|---|
| Plan index fresh | `test-plan-index.sh` | every `plans/shipped/*.md` indexed; regenerate+diff; `--fix` |
| Shipped⇒archived | `test-plan-archived.sh` | an `active/` plan with populated Implementation log + passed Verification must be under `shipped/` (`<!-- in-flight -->` opt-out; log-only→fail) |
| Naming | `test-plan-naming.sh` | `docs/plans/**` kebab (excl. `^_`) |
| Link integrity | extend `test-markdown-links.sh` | un-exclude `plans/shipped`; diff-scope→`--all` |
| No dangling ref | `test-plan-ref-integrity.sh` | grep all tracked files for `docs/plans/(shipped\|active)/<slug>.md` → exists |
| Portable purity | `test-portable-purity.sh` (NEW) | files under `agents/core/`, `docs/agent-rules/`, `docs/harness/` contain no `project.name`/`env_prefix` literal (e.g. "Smatchet", "SMATCHET_") except via config pointer — the guard that keeps the portable layer reusable |

## Key files / models

- Generator/guard models: `scripts/dev/test-backlog-counts.sh` (`--fix`), `scripts/dev/test-lint-rules.sh` (`--catalog --refresh` regenerate+diff), `scripts/dev/test-markdown-links.sh`, `scripts/dev/test-doc-anchors.sh`, `scripts/dev/test-agent-contract.sh`.
- Harness adapter: `scripts/setup-harness.sh` (flat discovery symlinks).
- Hand-verify (not just sed): `test-backlog-counts.sh:32-33,40-49`, `.gitattributes:55`, `sort-applied-md.sh`, `.understand-anything/*.json`, CI `paths:` filters in `doc-validation.yml`.

## Verification

- **Gate #1 first:** confirm Claude Code resolves agents after the split (via setup-harness flat symlinks); `test-agent-contract.sh` 19/19 green.
- **Per phase:** `scripts/dev/test-all.sh` green; `git grep <old-path>` → 0; `test-plan-index.sh` no drift; `test-plan-ref-integrity.sh` 0 dangling; `test-portable-purity.sh` clean.
- **Phase D:** full dual-target build green (comment-only edits, no compile impact); gates dismissed only via out-of-band label, never admin bypass.
- **Reuse smoke test (end-to-end proof of the goal):** in a scratch dir, copy `agents/{core,_shared}/`, `docs/agent-rules/`, `docs/harness/`, the self-improvement framework + generic scripts, and a fresh `project.config.json`; confirm nothing references "Smatchet"/`SMATCHET_` (via `test-portable-purity.sh`) and the harness loads the core agents. This validates the whole point of the plan.
- **Final state:** `docs/` = `STRUCTURE.md PORTABILITY.md CONTEXT.md adr/ agent-rules/ guides/ harness/ high-integrity/ perf/ perforce/ plans/ reference/ self-improvement/`; `agents/` = `core/ project/ _shared/`; one `project.config.json` at root.

## Implementation log
*(populated post-ship — bullet per shipped phase/PR: `<sha/PR> · <one-line>`)*

## Deviations from plan
*(populated post-ship — what changed/deferred vs this plan, one-line rationale each)*

## Verification (actual)
*(populated post-ship — what was actually tested + result: passed / failed / not-run)*
