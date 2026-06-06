// pre-merge-review.js — the first saved Smatchet Workflow (canonical, tracked).
//
// Parallel-barrier `code-review` + `security-review` (both read-only → zero
// write-set collision, no worktree, no lock) → a judge stage (plain `agent()`,
// default model, schema-forced) collapsing the two severity-tagged punch-lists
// into ONE ranked, deduped verdict. Concurrency 2 (under the Opus cap). Touches
// no files; never reaches the gated ship-loop tail (the orchestrator acts on the
// verdict). See docs/agent-rules/workflow-orchestration.md.
//
// Discovery: linked into the gitignored .claude/workflows/ by setup-harness.sh,
// then run as  Workflow({ name: 'pre-merge-review', args: <see below> }).
//
// Input (args):
//   { pr: 884 }                 review GitHub PR #884
//   { base: 'origin/develop' }  review the local working branch vs that base
//   undefined / {}              default — local branch diff vs origin/develop
//                               (works with no GitHub remote; mirrors /code-review)

export const meta = {
  name: 'pre-merge-review',
  description: 'Parallel code-review + security-review of a PR or local diff, synthesized into one ranked deduped pre-merge verdict',
  phases: [
    { title: 'Review', detail: 'code-review + security-review in parallel (read-only, zero collision)' },
    { title: 'Judge', detail: 'collapse both punch-lists into one ranked, deduped verdict' },
  ],
}

// --- resolve the review target from args (the only branching in the script) ---
const target = (args && args.pr)
  ? { kind: 'pr',   ref: String(args.pr), desc: 'GitHub PR #' + args.pr }
  : { kind: 'diff', ref: (args && args.base) || 'origin/develop',
      desc: 'the local working branch diff vs ' + ((args && args.base) || 'origin/develop') }

// A severity-tagged punch list — both reviewers return this identical shape.
const FINDINGS = {
  type: 'object',
  additionalProperties: false,
  required: ['findings'],
  properties: {
    findings: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['severity', 'file', 'title'],
        properties: {
          severity: { type: 'string', enum: ['CRITICAL', 'HIGH', 'MEDIUM', 'LOW'] },
          file: { type: 'string', description: 'path:line when known' },
          category: { type: 'string' },
          title: { type: 'string' },
          detail: { type: 'string' },
          suggested_fix: { type: 'string' },
        },
      },
    },
  },
}

// The judge's collapsed output — one ranked, deduped verdict + a merge call.
const VERDICT = {
  type: 'object',
  additionalProperties: false,
  required: ['summary', 'ranked_findings', 'merge_recommendation'],
  properties: {
    summary: { type: 'string', description: 'one-paragraph overall assessment' },
    ranked_findings: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['rank', 'severity', 'file', 'title', 'source'],
        properties: {
          rank: { type: 'integer' },
          severity: { type: 'string', enum: ['CRITICAL', 'HIGH', 'MEDIUM', 'LOW'] },
          file: { type: 'string' },
          title: { type: 'string' },
          detail: { type: 'string' },
          source: { type: 'string', enum: ['code-review', 'security-review', 'both'] },
        },
      },
    },
    merge_recommendation: { type: 'string', enum: ['block', 'fix-then-merge', 'merge'] },
  },
}

const reviewPrompt = (lens) =>
  'Review ' + target.desc + ' for ' + lens + '.\n' +
  (target.kind === 'pr'
    ? 'Target: GitHub PR #' + target.ref + ' (use gh to fetch the diff). '
    : 'Target: diff the current working branch against ' + target.ref + '. ') +
  'Read-only — do NOT edit any files; discover via semantic search (run_pipeline / ' +
  'get_skeleton), not raw Grep/Glob (vexp-guard blocks them). Return a severity-tagged ' +
  'punch list: each finding = severity (CRITICAL/HIGH/MEDIUM/LOW), file (path:line when ' +
  'known), category, a one-line title, detail, and a suggested_fix. Empty findings is a ' +
  'valid clean result — do not invent issues.'

phase('Review')
const reviews = await parallel([
  () => agent(reviewPrompt('correctness, Smatchet invariants (C++14, RAII, LOG_*, UI-thread non-blocking), and code quality'),
              { agentType: 'code-review',     label: 'code-review',     phase: 'Review', schema: FINDINGS }),
  () => agent(reviewPrompt('security: input validation, injection, secret leakage, deserialization, sandbox/MCP/CLI/Lua/p4/HTTP/SQLite trust-boundary surfaces'),
              { agentType: 'security-review', label: 'security-review', phase: 'Review', schema: FINDINGS }),
])

phase('Judge')
const punchLists = reviews.filter(Boolean)
if (punchLists.length === 0) {
  return { summary: 'Both reviewers failed or were skipped — no verdict.', ranked_findings: [], merge_recommendation: 'block' }
}

const judgePrompt =
  'You are the judge for a pre-merge review of ' + target.desc + '. Two read-only reviewers ' +
  'returned severity-tagged punch lists. Collapse them into ONE verdict: (1) DEDUPE findings ' +
  'that both raised (mark source "both"); (2) RANK by severity then blast-radius (rank 1 = act ' +
  'first); (3) set merge_recommendation = "block" if any CRITICAL/HIGH is real, "fix-then-merge" ' +
  'for MEDIUM/LOW only, "merge" if genuinely clean. Be skeptical — drop a finding that is a ' +
  'false positive and say so in the summary.\n\n' +
  'code-review punch list:\n' + JSON.stringify(punchLists[0]) + '\n\n' +
  'security-review punch list:\n' + JSON.stringify(punchLists[1] || { findings: [] })

const verdict = await agent(judgePrompt, { label: 'judge', phase: 'Judge', schema: VERDICT })
return verdict
