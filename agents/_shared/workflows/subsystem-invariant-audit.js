// subsystem-invariant-audit.js — read-only multi-modal sweep auditing each
// Source/Core/src/<ctx>/ zone against its leaf AGENTS.md invariants, then
// aggregating the per-zone drift into one report.
//
// Each zone is audited by a `code-review` subagent (read-only → declared
// `read-only: true`, so the 5-wide fan-out is collision-free: no worktree, no
// lock; respects workflow-orchestration.md Boundary 2). Each auditor is blind to
// the others (multi-modal sweep — one disjoint zone each). A plain-`agent()`
// aggregator collapses the findings into a single ranked drift report. Touches
// no files; never reaches the gated ship-loop tail.
//
// Discovery: linked into .claude/workflows/ by setup-harness.sh →
//   Workflow({ name: 'subsystem-invariant-audit' })
// Input (args): { zones: ['Tracker','Sync'] } to scope; default = all leaf zones.
// See docs/agent-rules/workflow-orchestration.md.

export const meta = {
  name: 'subsystem-invariant-audit',
  description: 'Read-only fan-out auditing each Source/Core/src subsystem zone against its leaf AGENTS.md invariants, aggregated into one drift report',
  phases: [
    { title: 'Audit', detail: 'one read-only code-review per leaf zone (disjoint, collision-free)' },
    { title: 'Aggregate', detail: 'collapse per-zone drift into one ranked report' },
  ],
}

// The leaf-AGENTS.md zones (CONTEXT-MAP.md registry). Edit here when a subsystem
// earns a leaf AGENTS.md — no other change needed.
const ALL_ZONES = ['Tracker', 'Commands', 'Persistence', 'Sync', 'Ui']
const zones = (args && Array.isArray(args.zones) && args.zones.length) ? args.zones : ALL_ZONES

const DRIFT = {
  type: 'object',
  additionalProperties: false,
  required: ['zone', 'in_sync', 'drifts'],
  properties: {
    zone: { type: 'string' },
    in_sync: { type: 'boolean', description: 'true when the zone code upholds every leaf-AGENTS.md invariant' },
    drifts: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['severity', 'invariant', 'evidence'],
        properties: {
          severity: { type: 'string', enum: ['CRITICAL', 'HIGH', 'MEDIUM', 'LOW'] },
          invariant: { type: 'string', description: 'the leaf-AGENTS.md rule that is violated' },
          evidence: { type: 'string', description: 'file:line + what violates it' },
        },
      },
    },
  },
}

const auditPrompt = (ctx) =>
  'Audit the subsystem at Source/Core/src/' + ctx + '/ against the invariants in its leaf ' +
  'Source/Core/src/' + ctx + '/AGENTS.md. Read the leaf AGENTS.md, then check the zone source ' +
  'for any code that violates a stated invariant (discover via semantic search — run_pipeline / ' +
  'get_skeleton — not raw Grep/Glob; vexp-guard blocks them). Read-only — do NOT edit any file. ' +
  'Report each drift: severity, the invariant violated, and file:line evidence. in_sync=true with ' +
  'an empty drifts list is the expected clean result — do not invent violations.'

phase('Audit')
const audits = await parallel(
  zones.map((ctx) => () =>
    agent(auditPrompt(ctx), { agentType: 'code-review', label: 'audit:' + ctx, phase: 'Audit', schema: DRIFT })
  )
)

phase('Aggregate')
const perZone = audits.filter(Boolean)
const REPORT = {
  type: 'object',
  additionalProperties: false,
  required: ['summary', 'zones_audited', 'zones_with_drift', 'ranked_drifts'],
  properties: {
    summary: { type: 'string' },
    zones_audited: { type: 'integer' },
    zones_with_drift: { type: 'array', items: { type: 'string' } },
    ranked_drifts: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['rank', 'zone', 'severity', 'invariant', 'evidence'],
        properties: {
          rank: { type: 'integer' },
          zone: { type: 'string' },
          severity: { type: 'string', enum: ['CRITICAL', 'HIGH', 'MEDIUM', 'LOW'] },
          invariant: { type: 'string' },
          evidence: { type: 'string' },
        },
      },
    },
  },
}

const aggregatePrompt =
  'You are aggregating per-zone subsystem-invariant audits into one report. For each zone you got ' +
  'an { zone, in_sync, drifts[] } object. Produce: a one-paragraph summary; zones_audited (count); ' +
  'zones_with_drift (the zones whose in_sync is false); and ranked_drifts — every drift across all ' +
  'zones, ranked by severity then blast-radius (rank 1 = fix first). Drop nothing; do not invent.\n\n' +
  'Per-zone audits:\n' + JSON.stringify(perZone)

return await agent(aggregatePrompt, { label: 'aggregate', phase: 'Aggregate', schema: REPORT })
