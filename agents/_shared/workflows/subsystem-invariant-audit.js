// subsystem-invariant-audit.js — saved Workflow (portable, tracked).
//
// Read-only drift audit: each per-subsystem zone that carries a leaf AGENTS.md
// (scoped, imperative invariants) is reviewed by its own read-only `code-review`
// agent against ONLY that leaf's rules, then a judge stage aggregates the
// per-zone findings into one ranked drift report. All reviewers are read-only →
// zero write-set collision, no worktree, no lock (fan-out-safety Boundary, see
// docs/agent-rules/workflow-orchestration.md).
//
// PORTABLE (lives in agents/_shared/workflows/, purity-gated): the script body
// embeds NO project literal — no subsystem path, no repo-specific command. Zone
// discovery happens at RUNTIME and is delegated to a discovery agent (the
// Workflow JS runtime has no shell/fs/glob of its own), which reads the repo's
// own per-subsystem agent-doc registry + globs the leaf docs and returns the
// zone list. The earlier shape was DEFERRED from PR #887 because a hardcoded
// core-source path is a project literal test-portable-purity (correctly) blocks;
// this runtime-discovery shape carries none. Backlog:
// subsystem-invariant-audit-workflow-deferred.
//
// Discovery: linked into the gitignored .claude/workflows/ by setup-harness.sh,
// then run as  Workflow({ name: 'subsystem-invariant-audit' }).
//
// Input (args), all optional:
//   {}                  audit every discovered leaf-AGENTS.md zone (default)
//   { zones: [...] }    audit only these zones (repo-relative dir paths) —
//                       skips the discovery phase

export const meta = {
  name: 'subsystem-invariant-audit',
  description: 'Read-only fan-out auditing each subsystem zone against its leaf AGENTS.md invariants, aggregated into one drift report',
  phases: [
    { title: 'Discover', detail: 'enumerate the per-subsystem leaf-AGENTS.md zones at runtime (zero project literal in the script)' },
    { title: 'Audit', detail: 'one read-only code-review agent per zone vs ONLY that leaf rules (parallel, zero collision)' },
    { title: 'Aggregate', detail: 'collapse per-zone findings into one ranked drift report' },
  ],
}

// --- The discovery agent's output: the list of auditable zones. -------------
const ZONES = {
  type: 'object',
  additionalProperties: false,
  required: ['zones'],
  properties: {
    zones: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['name', 'dir', 'leaf'],
        properties: {
          name: { type: 'string', description: 'short subsystem name' },
          dir: { type: 'string', description: 'repo-relative source dir the rules govern' },
          leaf: { type: 'string', description: 'repo-relative path to that dir leaf AGENTS.md' },
        },
      },
    },
  },
}

// --- A per-zone drift punch list (each auditor returns this shape). ---------
const ZONE_FINDINGS = {
  type: 'object',
  additionalProperties: false,
  required: ['zone', 'findings'],
  properties: {
    zone: { type: 'string' },
    findings: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['severity', 'file', 'invariant', 'title'],
        properties: {
          severity: { type: 'string', enum: ['CRITICAL', 'HIGH', 'MEDIUM', 'LOW'] },
          file: { type: 'string', description: 'path:line when known' },
          invariant: { type: 'string', description: 'the leaf-AGENTS.md rule violated' },
          title: { type: 'string' },
          detail: { type: 'string' },
          suggested_fix: { type: 'string' },
        },
      },
    },
  },
}

// --- The aggregator's collapsed drift report. ------------------------------
const REPORT = {
  type: 'object',
  additionalProperties: false,
  required: ['summary', 'ranked_findings', 'clean_zones'],
  properties: {
    summary: { type: 'string', description: 'one-paragraph drift assessment across all zones' },
    clean_zones: { type: 'array', items: { type: 'string' }, description: 'zones with zero findings' },
    ranked_findings: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['rank', 'severity', 'zone', 'file', 'invariant', 'title'],
        properties: {
          rank: { type: 'integer' },
          severity: { type: 'string', enum: ['CRITICAL', 'HIGH', 'MEDIUM', 'LOW'] },
          zone: { type: 'string' },
          file: { type: 'string' },
          invariant: { type: 'string' },
          title: { type: 'string' },
          detail: { type: 'string' },
        },
      },
    },
  },
}

// === Phase 1 — discover the zones (delegated; no shell in the JS runtime) ===
phase('Discover')
let zones = (args && Array.isArray(args.zones))
  ? args.zones.map((d) => ({ name: String(d).split('/').filter(Boolean).pop() || String(d), dir: String(d), leaf: String(d).replace(/\/?$/, '/') + 'AGENTS.md' }))
  : null

if (!zones) {
  const discoverPrompt =
    'Enumerate this repo\'s per-subsystem leaf AGENTS.md zones — read-only.\n' +
    'A "leaf AGENTS.md" is a subsystem-scoped agent-rules file co-located inside a ' +
    'source subsystem directory (NOT the repo-root AGENTS.md, NOT any docs/ rule-doc). ' +
    'Find them via the repo\'s own per-subsystem agent-doc REGISTRY first (the registry ' +
    'doc that maps each subsystem to its leaf AGENTS.md — search the repo root for a ' +
    'context/subsystem map), then confirm against the working tree by globbing for ' +
    'leaf AGENTS.md files nested under the core source tree (e.g. `git ls-files "*/AGENTS.md"` ' +
    'then drop the repo-root one and any under docs/, agents/, tests/, ThirdParty/, build/). ' +
    'Return one entry per zone: name (short subsystem name), dir (the repo-relative ' +
    'source directory the rules govern), leaf (the repo-relative path to that AGENTS.md). ' +
    'Do NOT edit anything. An empty list is a valid answer if the repo has no leaf AGENTS.md.'
  const discovered = await agent(discoverPrompt, { label: 'discover-zones', phase: 'Discover', schema: ZONES })
  zones = (discovered && Array.isArray(discovered.zones)) ? discovered.zones : []
}

if (!zones.length) {
  log('subsystem-invariant-audit: no leaf-AGENTS.md zones discovered — nothing to audit.')
  return { summary: 'No subsystem leaf-AGENTS.md zones found; nothing audited.', ranked_findings: [], clean_zones: [] }
}
log(`subsystem-invariant-audit: ${zones.length} zone(s) — ${zones.map((z) => z.name).join(', ')}.`)

// === Phase 2 — one read-only auditor per zone (parallel barrier) ===========
phase('Audit')
const auditPrompt = (z) =>
  'Audit the source subsystem at ' + z.dir + ' for DRIFT from ITS OWN leaf agent-rules ' +
  'file at ' + z.leaf + '. Read-only — do NOT edit any files; discover via semantic ' +
  'search / code-graph queries first, falling back to Grep/Glob.\n' +
  'STEP 1: read ' + z.leaf + ' and extract every imperative, checkable invariant it states ' +
  '(naming rules, banned constructs, layering/ownership constraints, required patterns).\n' +
  'STEP 2: review the code under ' + z.dir + ' for any place that VIOLATES one of those leaf ' +
  'invariants. Judge ONLY against this leaf\'s rules — do not re-litigate repo-wide rules a ' +
  'central gate already enforces, and do not flag style preferences the leaf does not state.\n' +
  'Return zone="' + z.name + '" and a punch list: each finding = severity ' +
  '(CRITICAL/HIGH/MEDIUM/LOW), file (path:line when known), invariant (the exact leaf rule ' +
  'violated), a one-line title, detail, and a suggested_fix. Empty findings is a valid clean ' +
  'result — do not invent drift.'

const audits = await parallel(zones.map((z) => () =>
  agent(auditPrompt(z), { agentType: 'code-review', label: 'audit:' + z.name, phase: 'Audit', schema: ZONE_FINDINGS })
))

// === Phase 3 — aggregate into one ranked drift report ======================
phase('Aggregate')
const results = audits.filter(Boolean)
if (results.length === 0) {
  return { summary: 'Every zone auditor failed or was skipped — no drift report.', ranked_findings: [], clean_zones: [] }
}

const aggregatePrompt =
  'You are the aggregator for a subsystem-invariant drift audit. Each read-only auditor ' +
  'returned a per-zone punch list of violations of that zone\'s leaf AGENTS.md invariants. ' +
  'Collapse them into ONE drift report: (1) RANK all findings by severity then blast-radius ' +
  '(rank 1 = act first); (2) list clean_zones (zones that returned zero findings); (3) write a ' +
  'one-paragraph summary of where the codebase has drifted from its documented per-subsystem ' +
  'invariants. Be skeptical — drop a finding that is a false positive and say so in the summary.\n\n' +
  'Per-zone punch lists:\n' + JSON.stringify(results)

const report = await agent(aggregatePrompt, { label: 'aggregate', phase: 'Aggregate', schema: REPORT })
return report
