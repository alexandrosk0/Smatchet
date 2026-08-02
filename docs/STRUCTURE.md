<!-- index-summary: normative docs/agentic taxonomy + portable/project boundary + enforcing guards -->
# Repository documentation & agentic structure (normative)

This is the **binding map** of where things live and the **portable / project
boundary** that lets the agentic layer be reused in another project. Each row
names the guard that enforces it — doc and lint share one source of truth.
Reuse guide: [`PORTABILITY.md`](PORTABILITY.md). Plan: [`plans/shipped/agentic-layer-project-independence.md`](plans/shipped/agentic-layer-project-independence.md).

## The boundary

**PORTABLE** (copy into another project as-is; project values come from `project.config.json`):
`agents/core/`, `agents/_shared/`, `docs/agent-rules/`, `docs/harness/`, the self-improvement **framework**, the generic `scripts/dev/test-*.sh` + `merge-gates.*` + `project-config.sh`, and `AGENTS.md`'s structure.

**PROJECT-SPECIFIC** (re-authored per project): `project.config.json`, `agents/project/`, `docs/plans/`, `docs/self-improvement/categories/*` (entries), `docs/CONTEXT.md`, `CONTEXT-MAP.md`, the per-subsystem leaf docs `Source/Core/src/<ctx>/{AGENTS,CONTEXT,README}.md`, `docs/adr/`, `docs/perf/`, `docs/perforce/`, `docs/high-integrity/`, `docs/reference/`, project-only scripts (`test-lint-rules.sh`, `test-subsystem-docs.sh`, `perf-*`, `p4-*`).

The single seam for project values is **`project.config.json`** (schema-validated). `scripts/dev/project-config.sh` exports it as `PC_*` shell vars.

## Taxonomy

| Path | Tier | Holds | Enforced by |
|---|---|---|---|
| `/AGENTS.md` | portable | rulebook (prose pointers to `project.config.json`) | `test-doc-anchors` |
| `/CONTEXT-MAP.md` | project | registry of per-subsystem leaf docs + harness-discovery index | `test-subsystem-docs` |
| `Source/Core/src/<ctx>/{AGENTS,CONTEXT,README}.md` | project | per-subsystem leaf rules / glossary / orientation | `test-subsystem-docs` |
| `/project.config.json` (+ `.schema.json`) | project | the one value table | schema validation (`doc-validation.yml`) |
| `agents/core/` | portable | generic engineering-role agents | `test-portable-purity`, `test-agent-contract` |
| `agents/project/` | project | subsystem-bound agents | `test-agent-contract` |
| `agents/_shared/` | portable | skills, token-tracking, templates | `test-portable-purity` |
| `docs/agent-rules/` | portable | delegation / merge-gates / ship-loops / process rules | `test-portable-purity`, `test-doc-anchors` |
| `docs/harness/` | portable | IDE adapters | `test-portable-purity` |
| `docs/self-improvement/` | framework portable / entries project | `AGENT_SELF_IMPROVEMENT.md` + `categories/` | `test-backlog-counts` |
| `docs/plans/active/` | project | working plans (+ `_plan-template.md`, `_plan-locks*`) | `test-plan-naming` |
| `docs/plans/shipped/` | project | shipped plans (**never renamed** — see below) | `test-plan-index`, `test-plan-ref-integrity` |
| `docs/plans/deferred/` | project | parked-indefinitely plans (captured design, build only on a named recurrence; `STATUS: DEFERRED` banner) | `test-plan-naming`, `test-plan-ref-integrity` |
| `docs/plans/INDEX.md` | project | auto-generated shipped-plan index | `test-plan-index` |
| `docs/guides/` | mixed (per-file `tier:`) | how-to docs (offline-builds, perf-workflow, caveman, agent-token-tracking) | `test-portable-purity` |
| `docs/reference/` | project | dated snapshots / archived refs | — |
| `docs/{CONTEXT.md,adr,perf,perforce,high-integrity}` | project | glossary, decisions, perf, p4, lint baseline | their own gates |
| `/{SECURITY,CPP_CODE,AGENTIC_INFRA}_AUDIT.md`, `/UX_DESIGN_CRITIQUE.md`, `/TEST_COVERAGE_GAP_MAP.md`, `/MUTATION_PILOT.md` | project | **dated whole-tree audit reports** — one campaign each, `**Date:**` + `**Branch:**` + `**Scope:**` header, findings numbered and cited normatively from production comments (81 `CPP_CODE_AUDIT.md #N` / `SECURITY_AUDIT.md #N` citations in `Source/`) | `test-markdown-links` (link hygiene) · **no content/lifecycle guard** — deliberate, see below |
| `/CLI_GUIDE.md`, `/LUA_GUIDE.md`, `/MCP_GUIDE.md`, `/AI_POLICY.md`, `/CONTEXT-MAP.md` | project | top-level user-facing guides | `test-markdown-links` (gate-blind-spot-sweep Slice 3) |
| `backlog/` | project | pre-plan review queues (`BACKLOG_CODE_REVIEW.md`, `MANUAL_TEST_QUEUE.md`, `POST_P0_REVIEW.md`, `DEEP_REVIEW_*.md`) — findings not yet promoted to a plan or an ADR | `is-pure-docs-diff` / `merge-gates` treat it as docs · `issue-sweep` reads `elevate-to-issue:` lines · **no freshness guard** |

**Why the audit reports have no content guard (explicit `—`, not an oversight).** They are *dated snapshots of a finished campaign*, not living documents: a report is correct as of its `**Date:**` header and is never edited to track the tree. So there is nothing for a freshness gate to assert — a stale report is the expected steady state, and the numbered findings must stay stable precisely because production comments cite them by number. What they are NOT is scratch files: deleting or renumbering one silently breaks 81 first-party code comments. Treat them as append-only-by-campaign, like `docs/adr/`.

`CONTEXT.md` stays at `docs/` root (the one living-index hub). Generated/template files are `_`-prefixed (`_plan-locks.generated.md`) or carry a "do not edit" header.

## Naming

Lowercase-kebab for dirs and leaf-doc slugs; `_`-prefix reserved for templates/generated; SCREAMING_CASE reserved for append-target index/registry files (`CONTEXT.md`, `INDEX.md`, `AGENT_SELF_IMPROVEMENT.md`, `MARKER_INVENTORY.md`) **and for the repo-root dated audit reports** (below). Enforced for plans by `test-plan-naming`.

**Where a NEW dated audit report goes: repo root, SCREAMING_CASE, one file per campaign.** Recorded here so the next audit follows a rule instead of precedent. Rationale — the existing six are cited by number from production comments and from each other's `**Companions:**` headers, so discoverability at the root is load-bearing; `docs/reference/` is for *superseded* snapshots, and moving a live report there would break those citations. Requirements for a new one:

1. Root, `SCREAMING_CASE.md`, named for the *subject*, not the date (`SECURITY_AUDIT.md`, not `AUDIT_2026_06.md`) — a re-audit of the same subject **replaces** the file's body and bumps `**Date:**`; it does not add a second file.
2. A header block with `**Date:**`, `**Branch:**`, `**Scope:**` (what was and was not read), and `**Companions:**` linking sibling reports, matching the six above.
3. Findings **numbered** and stable, because production comments cite them as `<FILE>.md #N`. Renumbering is a breaking change.
4. Add a taxonomy row above in the same PR — that is what this rule exists to make automatic.

A dated snapshot that is *not* a numbered-findings audit (a one-off measurement, a superseded reference) still goes to `docs/reference/`; a how-to still goes to `docs/guides/`.

## Plan lifecycle

1. New plan → `docs/plans/active/<slug>.md`, committed immediately (`wip(plan): <slug>`).
2. Populate `## Implementation log` / `## Deviations` / `## Verification (actual)` post-ship.
3. `git mv` to `docs/plans/shipped/<slug>.md`. Its row appears in `INDEX.md` automatically (`test-plan-index --fix`); curated one-liner via an `<!-- index-summary: … -->` comment in the plan.
4. **`docs/plans/shipped/` is never renamed.** ~252 source/ADR/script references cite shipped plans by path; a rename would touch `Source/Core/**` comments for zero functional gain. Decision recorded here so no future reorg re-litigates it.
5. **Reference plans tier-less: `docs/plans/<slug>.md`** (not `…/active/<slug>.md` or `…/shipped/<slug>.md`). `test-markdown-links` resolves the tier-less form against `active/`, `shipped/`, or `deferred/`, so a reference survives the archival `git mv active → shipped` with no ref-sweep. A **tiered** link passes the local existence check while the target is still in `active/` but 404s on CI after the move — the `markdown-links-local-passes-ci-fails-after-plan-archive` class; `test-markdown-links.sh --merge-tree-warn` flags tiered plan links in a diff before they rot.

## Decision tree — where does X go?

- A plan → `docs/plans/active/` (shipped → `shipped/`).
- Agent friction/idea → `docs/self-improvement/categories/<category>.md`.
- A binding decision → `docs/adr/NNNN-<slug>.md`.
- A reusable engineering-role agent → `agents/core/`; a subsystem-bound one → `agents/project/`.
- A project value (path, preset, threshold, label) → `project.config.json` (never hardcode in a portable file).
- A how-to → `docs/guides/` (+ a `tier:` marker); a dated snapshot → `docs/reference/`.
- A whole-tree audit campaign with numbered findings → a repo-root `SCREAMING_CASE.md` (+ a taxonomy row) — see § Naming.
- A review finding not yet worth a plan → `backlog/` (promote to `docs/plans/active/` or `docs/adr/` when it is).

## Enforcement guards (`doc-validation.yml`, cheap Ubuntu lane)

`test-plan-index` (index fresh), `test-plan-ref-integrity` (no dangling plan refs), `test-plan-naming` (kebab), `test-portable-purity` (no NEW project literals in portable dirs — baselined), `test-agent-contract` (agent output contract), `test-doc-anchors` (`§` anchors resolve), `test-backlog-counts` (self-improvement counts), plus `project.config.schema.json` validation.

## Known follow-up

The portable **structure** is in place, but the portable files still embed project literals in prose (baselined in `portable-purity-baseline.txt` — see that file for the current count). **Full de-Smatchet-ification** of `agents/core/` + `docs/agent-rules/` prose (replace literals with `project.config` references) is tracked in `docs/self-improvement/categories/` — until done, reuse means copy + adapt the prompts, not copy verbatim. `test-portable-purity` prevents the baseline from growing.
