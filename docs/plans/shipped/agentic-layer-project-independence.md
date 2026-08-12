# Plan — Documentation + agentic-layer reorganization for **project-independence**

<!-- plan-date: 2026-05-29 -->
<!-- index-summary: Docs + agentic-layer reorganization for project-independence (Phases A-F): agents/core+project split, project.config seam, docs/plans+self-improvement taxonomy, STRUCTURE.md, 8 CI guards -->

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

Single source for the values currently inlined. Scripts read it where cheap; AGENTS.md/rules prose points to it for parameterized facts (no `${VAR}` substitution — prose stays readable, the config is the canonical value table). A `project.config.schema.json` (JSON Schema) ships alongside and is validated in CI. The schema must cover the **full coupling surface** found in the audit (not just build/perf/lint):

```json
{
  "project": { "name": "Smatchet", "env_prefix": "SMATCHET",
               "literals": ["Source_Core","Target_Standalone","SmatchetStandalone",
                            "SmatchetCore_DX12","UnrealPlugins","DX12","ITrackerClient"] },
  "build": { "presets": ["ninja-iter-msvc","ninja-test-msvc","ninja-msvc-asan"],
             "targets": ["SmatchetStandalone","SmatchetCore_DX12"], "exe_path": "build/{preset}/Smatchet.exe" },
  "perf": { "frame_budget_ms": 6.94, "fps_floor_ms": 16.67, "freeze_ms": 100,
            "baselines_dir": "docs/perf/baselines", "policy": "docs/perf/regression-policy.json" },
  "lint": { "zones": { "strict": ["Source_Core/src/Tracker","..."], "light": ["Source_Core/src/Ui","..."],
                       "exempt": ["ThirdParty","build"] },
            "rules": ["no-printf-stderr","no-raw-new","define-imgui","deviation-overdue"],
            "deviation_keyword": "SMATCHET_DEVIATION", "baseline": "docs/high-integrity/baseline.md" },
  "vcs": { "primary": "git", "optional_layer": "p4", "p4_streams": ["..."] },
  "ci": { "required_checks": ["Test-delta gate","Windows + MSVC","Windows + MSVC (Smatchet light — AI/Whisper/MCP off)"],
          "path_filters": { "code_globs": ["Source_Core/**","Plugins/**","Target_Standalone/**"],
                            "docs_ignore": ["**/*.md","docs/**","agents/**"] } },
  "merge_gates": { "cr_bot": "coderabbitai", "override_labels": ["tests-out-of-band","perf-out-of-band","cr-out-of-band"] },
  "visual_validation": { "trigger_globs": ["**/SmatchetTheme.cpp","**/Smatchet*Ui*.cpp","**/Locales/*.json"] },
  "golden": { "artifacts_dir": "tests/golden" },
  "harness": { "discovery_mode": "flat-symlink", "supported": ["claude-code","codex","cursor"] },
  "guards": { "doc_validation": ["test-plan-index","test-plan-naming","test-plan-ref-integrity",
                                 "test-plan-archived","test-portable-purity","test-markdown-links"] },
  "docs": { "taxonomy_exceptions": [] },
  "subsystems": [ { "name":"tracker", "agents":["tracker-backend"] }, "..." ],
  "agents": { "enabled_project": ["tracker-backend","grid-engine","..."] }
}
```

A `scripts/dev/project-config.sh` loader exports these as shell vars (e.g. `PC_PERF_BUDGET_MS`) so scripts source one file instead of hardcoding. `project.literals` (plus `project.name`/`env_prefix`/`build.*`/`vcs.p4_streams`) is the **denylist source** for the portable-purity guard (see Enforcement).

## Feasibility gate (built + verified in Phase A, BEFORE the Phase-B move)

**Gate #1 — agent discovery + harness compatibility with subdirs.** Confirmed coupling to flat `agents/<name>.md` that the subdir split would break:
- `scripts/setup-harness.sh` does `link_dir ".claude/agents" "agents"` (links the whole dir; Claude Code discovery must then recurse into `core/`+`project/`).
- `scripts/dev/test-agent-contract.sh` both loops `for f in agents/*.md` (flat) **and** hardcodes `f="agents/$a.md"` for named agents.
- `AGENTS.md` delegation tables name agents as canonical `agents/<name>.md`.
- the codex/cursor adapters in `setup-harness.sh` read `agents/*.md`.

**Resolution (de-circularized):** the flattening mechanism + a discovery fixture are built and verified in **Phase A on a copy/fixture** (not on the live tree), so the mechanism is proven before any `git mv`. Mechanism: canonical source becomes split (`core/`/`project/`); `setup-harness.sh` emits **flat discovery symlinks** in `.claude/agents/` (one per agent) so every harness still resolves `agents/<name>`. If Claude Code recursion already works, the flat links are belt-and-suspenders. Fallback if neither resolves: `tier:` frontmatter, no moves. The physical `git mv` only happens in Phase B once the Phase-A fixture proves discovery on **both** Claude Code and Codex.

## Phased plan (each phase = its own PR(s); docs/scripts PRs pass via the merged Pattern C gate)

- **Phase A — compatibility layer + config seam + classification (no moves).** Pure additive, lowest risk, de-risks every later phase:
  - The canonical plan doc is already tracked at `docs/plans/shipped/agentic-layer-project-independence.md` (PR #532) — satisfies the "plan first" rule; lifecycle-rule edits are deferred to Phase F (after `docs/plans/` exists).
  - Add `project.config.json` + `project.config.schema.json` + `scripts/dev/project-config.sh` loader (capture today's values).
  - Add `docs/PORTABILITY.md`: per-file portable/project classification + coupling inventory + extraction checklist. **Inventory the external path contracts** in `.github/`, `.gitattributes`, scripts, hooks, harness docs (the rewrite targets for later phases).
  - Add the plan-index generator `scripts/dev/test-plan-index.sh` pointed at the *current* paths; `--fix` the ~33-plan drift in place.
  - **Build + verify the agent-discovery flattening on a fixture** (Gate #1, de-circularized): prove flat discovery symlinks resolve on Claude Code **and** Codex before any move. No `git mv` yet.
  - Generate the **project-literal denylist** (from `project.config.json`) consumed by the purity guard.
- **Phase B — agent split.** `git mv` agents into `agents/core/` + `agents/project/`. Update the full compatibility surface from Gate #1: `setup-harness.sh` (emit flat discovery symlinks; fix the codex/cursor `agents/*.md` counters to recurse), `test-agent-contract.sh` (the `for f in agents/*.md` loop **and** the hardcoded `agents/$a.md` named-agent paths), `test-doc-anchors.sh` globs, `AGENTS.md` delegation-table paths, CI `paths:` filters. Verify discovery on both harnesses + `test-agent-contract.sh` 19/19 green.
- **Phase C — backlog split.** `git mv` `AGENT_SELF_IMPROVEMENT.md` + `self-improvement/categories/` → `docs/self-improvement/{,categories/}`. Fix the ~135 refs (12 agent prompts, `test-backlog-counts.sh:32-33`, `.gitattributes:55`, `sort-applied-md.sh`, `AGENTS.md`, knowledge-graph JSON). Verify `git grep docs/self-improvement/categories` → 0.
- **Phase D — plans home (heavy; safe rewrite, not raw sed).** `git mv` `design/`→`plans/active/`, `design/archive/`→`plans/shipped/`, `BACKLOG_PLANS.md`→`plans/INDEX.md`. Rewrite the ~252 refs with a **purpose-built `scripts/dev/rewrite-plan-paths.sh`** (NOT a blind global sed) that handles each context separately — Markdown links, raw prose paths (C++ comments), generated JSON (`.understand-anything/*`), and **leaves intentional historical references in shipped plan logs untouched** — and **emits a review report** of every changed vs skipped reference. Still observe the ordering invariant (`docs/design/archive`→`docs/plans/shipped` before `docs/design`→`docs/plans/active`). Repoint the index generator. Dismiss the high-integrity/test-delta delta gates only via the documented `tests-out-of-band` label (comment-only change). Add **temporary tombstone stubs** (`docs/plans/active/README.md`, `docs/plans/shipped/README.md`, `docs/plans/INDEX.md`) pointing to the new homes (removable after a grace period). Verify `test-plan-ref-integrity.sh` → 0 dangling + full build green.
- **Phase E — orphan consolidation + hub demotion.** Create `guides/` + `reference/`; `git mv` `dev/offline-builds.md`, `PERF_WORKFLOW.md`, `CAVEMAN.md`, `AGENT_TOKEN_TRACKING.md`, evaluation snapshot, archived scenario; remove emptied dirs; fix ~117 hub refs. **Mixed dirs (`guides/`, `self-improvement/`) get a per-file frontmatter `tier: portable|project` tag enforced by `test-portable-purity.sh` + documented in `STRUCTURE.md`** (lighter than forcing `portable/`+`project/` subdirs; split a dir only where the boundary is clean). Keep `docs/high-integrity/` + `CONTEXT.md` (root).
- **Phase F — config-wire (where cheap) + rules + lock.** Source `project-config.sh` in scripts that already use env-vars (merge-gates, perf-run, test-backlog-counts) and replace the cheapest inline values. **Done-bar (hard): no file under `agents/core/`, `agents/_shared/`, `docs/agent-rules/`, or `docs/harness/` may contain a project literal except as a `project.config` key reference** (`test-portable-purity.sh` enforces). Heavier rewiring of **project-only** scripts (e.g. `test-lint-rules.sh` zone globs) may stay as documented follow-ups — those files are project-specific, not portable. Add `docs/STRUCTURE.md` (taxonomy + per-file portable/project tags + per-row enforcing guard + "shipped/ never renamed again" record). Stub into `process-rules.md` + 1-line `AGENTS.md` pointer (keeps `§` anchors resolving). Now update the plan-lifecycle rule to `docs/plans/active/<slug>.md`. Flip the log-only guards to fail.

## Enforcement guards (the "written rules", all in cheap `doc-validation.yml`)

| Guard | Script | Checks |
|---|---|---|
| Plan index fresh | `test-plan-index.sh` | every `plans/shipped/*.md` indexed; regenerate+diff; `--fix` |
| Shipped⇒archived | `test-plan-archived.sh` | an `active/` plan with populated Implementation log + passed Verification must be under `shipped/` (`<!-- in-flight -->` opt-out; log-only→fail) |
| Naming | `test-plan-naming.sh` | `docs/plans/**` kebab (excl. `^_`) |
| Link integrity | extend `test-markdown-links.sh` | un-exclude `plans/shipped`; diff-scope→`--all` |
| No dangling ref | `test-plan-ref-integrity.sh` | grep all tracked files for `docs/plans/(shipped\|active)/<slug>.md` → exists |
| Portable purity | `test-portable-purity.sh` (NEW) | files under `agents/core/`, `agents/_shared/`, `docs/agent-rules/`, `docs/harness/` contain **none of the generated denylist** — `project.name`, `env_prefix`, every `project.literals` entry (`Source_Core`, `Target_Standalone`, `SmatchetStandalone`, `SmatchetCore_DX12`, `DX12`, `ITrackerClient`, …), `build.presets` (`ninja-iter-msvc`…), `vcs.p4_streams` — except as a `project.config` **key reference**. Denylist is generated from `project.config.json`, not a hardcoded `Smatchet`/`SMATCHET_` pair. Also enforces the `tier:` frontmatter tag in mixed dirs. This is the guard that actually keeps the portable layer reusable. |

## Key files / models

- Generator/guard models: `scripts/dev/test-backlog-counts.sh` (`--fix`), `scripts/dev/test-lint-rules.sh` (`--catalog --refresh` regenerate+diff), `scripts/dev/test-markdown-links.sh`, `scripts/dev/test-doc-anchors.sh`, `scripts/dev/test-agent-contract.sh`.
- Harness adapter: `scripts/setup-harness.sh` (flat discovery symlinks).
- Hand-verify (not just sed): `test-backlog-counts.sh:32-33,40-49`, `.gitattributes:55`, `sort-applied-md.sh`, `.understand-anything/*.json`, CI `paths:` filters in `doc-validation.yml`.

## Verification

- **Gate #1 first:** confirm Claude Code resolves agents after the split (via setup-harness flat symlinks); `test-agent-contract.sh` 19/19 green.
- **Per phase:** `scripts/dev/test-all.sh` green; `git grep <old-path>` → 0; `test-plan-index.sh` no drift; `test-plan-ref-integrity.sh` 0 dangling; `test-portable-purity.sh` clean.
- **Phase D:** full dual-target build green (comment-only edits, no compile impact); gates dismissed only via out-of-band label, never admin bypass.
- **Reuse smoke test (end-to-end proof of the goal):** in a scratch repo, copy **only the intended portable tree** (`agents/{core,_shared}/`, `docs/agent-rules/`, `docs/harness/`, the self-improvement *framework* files, the generic scripts) + a **fresh `project.config.json`**; run `test-portable-purity.sh` against the **generated denylist** (proves zero project-literal leakage, not just the `Smatchet` string); and verify **both Claude Code and Codex** discover/load the core agents through the harness. This is the literal validation of the plan's goal.

### "Portable extraction done" bar (hard)

1. Portable dirs (`agents/core`, `agents/_shared`, `docs/agent-rules`, `docs/harness`) contain **no** project literal except `project.config` key references (`test-portable-purity.sh` green on the generated denylist).
2. Core agents load through **every** supported harness (Claude Code + Codex), verified via the Phase-A fixture and the smoke test.
3. Every doc/plan guard fails on injected drift (proven once, e.g. add an unindexed `shipped/` file → `test-plan-index.sh` fails → `--fix` greens).
4. Old paths (`docs/plans/active/`, `docs/plans/shipped/`, `docs/plans/INDEX.md`) either redirect via tombstone, are explained, or are intentionally removed — no silent dead entry points.

- **Final state:** `docs/` = `STRUCTURE.md PORTABILITY.md CONTEXT.md adr/ agent-rules/ guides/ harness/ high-integrity/ perf/ perforce/ plans/ reference/ self-improvement/`; `agents/` = `core/ project/ _shared/`; `project.config.json` + `project.config.schema.json` at root.

## Plan-review disposition (2026-05-29)

External review applied. Verdict per point:

- **#1 plan-lifecycle sequencing — ACCEPTED (already satisfied):** the canonical plan is committed at `docs/plans/active/<slug>.md` (#532) before any phase; lifecycle-rule edit deferred to Phase F (after `docs/plans/` exists). Noted in Phase A/F.
- **#2 agent split breaks flat discovery — ACCEPTED:** verified `setup-harness.sh` whole-dir link + `test-agent-contract.sh` flat-glob *and* hardcoded `agents/$a.md`. Full compatibility surface now enumerated in Gate #1 + Phase B.
- **#3 Gate #1 circularity — ACCEPTED:** flattening mechanism + discovery fixture moved into Phase A; physical `git mv` only in Phase B after the fixture proves it.
- **#4 `docs/high-integrity/` inconsistency — REJECTED (stale):** develop already has `docs/high-integrity/baseline.md` (`docs/backlog/…` 404s). Plan was already correct; config sketch lists the right path.
- **#5 config under-scopes coupling — ACCEPTED:** schema expanded with ci, merge_gates, visual_validation, golden, harness, guards, docs.taxonomy_exceptions, vcs.p4_streams + `project.literals`.
- **#6 purity guard too narrow — ACCEPTED:** guard now uses a denylist generated from `project.config.json` (`project.literals` + presets + p4 streams), not the `Smatchet`/`SMATCHET_` pair.
- **#7 final state vs "all values in config" — ACCEPTED:** added the hard done-bar; heavy rewiring allowed to stay follow-up only for **project-only** scripts, never portable files.
- **#8 safer rewrite than global sed — ACCEPTED:** Phase D uses `rewrite-plan-paths.sh` (per-context handling + skip historical refs + review report).
- **#9 old-path compatibility — ACCEPTED (with nuance):** temporary tombstones at the top entry points; historical refs inside shipped logs are left intact (not rewritten), which #8's script enforces.
- **#10 mixed-dir labeling — ACCEPTED (lighter form):** per-file `tier:` frontmatter enforced by `test-portable-purity.sh` + `STRUCTURE.md`, rather than forcing `portable/`+`project/` subdirs everywhere.

## Implementation log

- **PR #542 · Phase A** — `project.config.json` + schema + `project-config.sh` loader; `docs/PORTABILITY.md`; `test-plan-index.sh` (closed index drift 16→53); Gate #1 discovery fixture; doc-validation wiring.
- **PR #543 · Phase B** — `git mv` 24 agents → `agents/core/` (16) + `agents/project/` (8); `setup-harness` flat links; location-agnostic `test-agent-contract`; ~319 refs rewritten.
- **PR #544 · Phase C** — `git mv` self-improvement → `docs/self-improvement/{,categories/}`; ~135 refs; hardcoded consumers fixed; count drift synced.
- **PR #545 · Phase D** — `git mv` design → `docs/plans/{active,shipped}` + `BACKLOG_PLANS.md`→`INDEX.md`; `rewrite-plan-paths.sh` (718 refs/269 files) + 166-ref active→shipped correction; `test-plan-ref-integrity.sh`; tombstones.
- **PR #546 / #547 · setup-harness** — fixed the Windows hang: flat per-agent **hardlinks** (not symbolic, not a recursion-dependent junction); collision warning. (#546 landed the junction interim; #547 re-shipped the flat-hardlink version.)
- **PR #548 · Phase E** — orphan dirs → `docs/guides/` + `docs/reference/`; root hubs demoted (kebab); completed Phase D's relative-form ref residue.
- **PR (this) · Phase F** — `docs/STRUCTURE.md` (normative taxonomy + guard map); `test-portable-purity.sh` (baselined) + `test-plan-naming.sh`; process-rules + AGENTS.md pointers; de-Smatchet follow-up filed.

## Deviations from plan

- **Phase B was far larger than the plan's "cheap, symlink-backed" estimate** — ~319 cross-references (AGENTS.md, ADRs, agent prompts, knowledge-graph) — handled by scripted rewrite (archive excluded).
- **Agent discovery: flat hardlinks, not a junction** — Windows symbolic `mklink` intermittently hangs; a junction-to-subdirs would depend on unverified harness recursion. Flat hardlinks are reliable + recursion-independent (the maintainer confirmed delegation works).
- **`agents/project/` has 9, not 8** — `command-system` added; `p4-janitor` kept in `core/` (generic VCS-maintenance, portable to any p4 project), per maintainer.
- **`test-portable-purity` is baseline-mode, not hard-zero** — the portable *structure* shipped, but the prompts still embed ~157 project literals; full de-Smatchet-ification is a tracked follow-up (`docs/self-improvement/categories/infra.md`). The guard blocks NEW leakage.
- **Pre-existing dangling refs allowlisted** — refs to never-existed plans (`agentic-{flow,triage,handoff}`, etc.) are allowlisted in `test-plan-ref-integrity`, not fixed here.
- **`docs/plans/shipped/` declared never-renamed** (252-ref blast radius) — recorded in STRUCTURE.md so no future reorg re-litigates it.

## Verification (actual)

- **CI-gated guards (doc-validation.yml) — PASS:** `test-plan-index` (53 indexed), `test-plan-ref-integrity` (72/72 resolve), `test-plan-naming`, `test-portable-purity` (baseline holds), `test-agent-contract` (25/0), `test-doc-anchors` (0 broken), `test-backlog-counts` (8/0), `test-agent-discovery-fixture`, `project.config.schema.json` validation.
- **Agent discovery — PASS (live):** maintainer confirmed delegation works through the flat `.claude/agents/*.md` links after `setup-harness.sh`.
- **Phase D rename — PASS:** dual-target build green on #545 (comment-only Source_Core edits; `tests-out-of-band` label); ref-integrity 0 dangling.
- **`test-markdown-links` (local-only, not CI-gated):** residual breaks are pre-existing (never-existed-plan refs + one pre-existing `delegation.md` relative bug).
- **Reuse smoke test — NOT-RUN (deferred):** copying the portable tree into a scratch repo + a fresh config is the end-to-end proof; deferred with the de-Smatchet follow-up (portable files still carry baselined literals, so a verbatim copy isn't literal-clean yet).
