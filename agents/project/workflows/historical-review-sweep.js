// historical-review-sweep.js — persisted, project-scoped (tracked).
//
// Historical code-review fan-out: one code-review agent per merged PR, each
// reviewing ONLY the lines that PR introduced that are STILL ALIVE + untouched
// at origin/develop (survivor digest from historical-review-survivors.sh).
// Pairs with the `historical-code-review` skill + the findings ledger at
// docs/self-improvement/historical-review-findings.md.
//
// PROJECT-SCOPED, not portable: the prompt embeds Smatchet literals (Source/Core,
// AGENTS.md, the survivor extractor path) so this lives under
// agents/project/workflows/ — NOT agents/_shared/workflows/, which
// test-portable-purity forbids project literals in. setup-harness.sh links BOTH
// dirs into the gitignored .claude/workflows/, so it still resolves by name:
//     Workflow({ name: 'historical-review-sweep', args: [<PR numbers>] })
//
// INPUT (args) — the PR work-list, REQUIRED. Deliberately NO fixed list in the
// body: a hard-coded array goes stale every batch (the "reconstruct from
// scratch" friction this persistence fixes). Pass a JSON ARRAY of PR numbers:
//     Workflow({ name: 'historical-review-sweep', args: [1180, 1181, 1182] })
// NOTE: this harness ALWAYS delivers args to the script as a STRING — even a
// real JSON array arrives as the literal text "[1180, 1181, 1182]" (probe:
// {argType:'string', isArray:false, parsedIsArray:true}). So the JSON.parse
// below is MANDATORY, not a fallback. Also accepted: { prs: [...] }. Empty /
// unparseable -> LOUD throw, never the silent 14ms "success" that hid a
// misrouted list (the original 0-agent no-op).
//
// Discover the next batch first (orchestrator step — the script has no shell
// access, so the list must arrive via args):
//     gh pr list --state merged --base develop --limit 900 --json number \
//       --jq '[.[] | select(.number < 542) | .number] | sort | reverse | .[0:100]'

export const meta = {
  name: 'historical-review-sweep',
  description: 'Historical code-review sweep of merged PRs — review only survivor lines still alive at origin/develop',
  phases: [
    { title: 'Review', detail: 'one opus code-review agent per PR over its survivor digest', model: 'opus' },
  ],
}

// --- resolve the PR work-list from args ------------------------------------
// Accepted shapes: a bare array of PR numbers, { prs: [...] }, a JSON string of
// either, or the historical-review-worklist.sh --json OBJECT
// ({ coverage, units: [{pr, ...}] }) — in that last form the gate's COMPUTED
// coverage triple is carried through to this workflow's return value, so a
// batch header quotes the computed number instead of a hand-transcribed one.
// Integer-filter so a stray non-number can never become a #NaN agent label.
let parsed = args
if (typeof parsed === 'string') { try { parsed = JSON.parse(parsed) } catch (e) { parsed = null } }
let coverage = null
if (parsed && !Array.isArray(parsed)) {
  if (parsed.coverage && typeof parsed.coverage === 'object') coverage = parsed.coverage
  if (Array.isArray(parsed.units)) parsed = [...new Set(parsed.units.map(u => u && u.pr))]
  else if (Array.isArray(parsed.prs)) parsed = parsed.prs
}
const prs = Array.isArray(parsed)
          ? parsed.map(Number).filter(n => Number.isInteger(n) && n > 0)
          : []
if (!prs.length) {
  throw new Error(
    'historical-review-sweep: empty PR work-list. Pass args as a JSON ARRAY of PR ' +
    'numbers, e.g. Workflow({ name: "historical-review-sweep", args: [1180, 1181] }). ' +
    'See docs/self-improvement/historical-review-findings.md § Sweep status (resume).')
}

phase('Review')
log(`Historical-review sweep: ${prs.length} PRs  (#${prs[prs.length-1]} .. #${prs[0]}). Each agent runs the survivor extractor + reviews only lines still alive at origin/develop.`)

const SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['pr', 'status', 'summary', 'findings'],
  properties: {
    pr: { type: 'number', description: 'the PR number reviewed' },
    sha: { type: 'string', description: 'short squash-commit sha from the digest header' },
    status: { type: 'string', enum: ['findings', 'clean', 'superseded', 'error'] },
    summary: { type: 'string', description: 'one-line survivor summary, e.g. "14 files, 417/418 introduced lines alive"' },
    findings: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['severity', 'file', 'line', 'problem', 'fix', 'userVisible'],
        properties: {
          severity: { type: 'string', enum: ['CRITICAL', 'HIGH', 'MEDIUM', 'LOW'] },
          file: { type: 'string', description: 'repo-relative path' },
          line: { type: 'number', description: 'HEAD line number from the digest' },
          problem: { type: 'string' },
          fix: { type: 'string' },
          userVisible: { type: 'boolean', description: 'true = user-visible product defect/correctness/safety -> GitHub Issue candidate; false = internal debt' },
        },
      },
    },
  },
}

function promptFor(pr) {
  return [
    `You are doing a HISTORICAL CODE REVIEW of merged PR #${pr} in the Smatchet repo (C:\\\\Dev\\\\Smatchet, working dir already set).`,
    ``,
    `STEP 1 — extract survivors (run exactly this, read-only):`,
    `    bash agents/scripts/core/historical-review-survivors.sh --pr ${pr} --context 3`,
    `This resolves the PR's squash commit and emits a DIGEST of ONLY the lines this PR introduced that are STILL ALIVE and UNTOUCHED at origin/develop (lines a newer PR rewrote are excluded by construction).`,
    ``,
    `Digest line marks:`,
    `  • leading SPACE  = a SURVIVING line still blamed to this PR -> THIS is your review surface.`,
    `  • leading "~"    = a context line added/changed by a DIFFERENT commit -> background only, NEVER flag it.`,
    ``,
    `STEP 2 — handle these cases first:`,
    `  • If the digest prints "FULLY SUPERSEDED" -> return status:"superseded", findings:[].`,
    `  • If the extractor errors (exit 2, e.g. no merge commit) -> return status:"error", summary:<the stderr line>, findings:[].`,
    `  • If the ONLY survivors are pure data/ledger rows with no reviewable logic (e.g. docs/self-improvement/merge-snapshots.jsonl appends, generated baselines) -> return status:"clean", findings:[].`,
    ``,
    `STEP 3 — review ONLY the space-marked surviving lines. The digest already carries 3 context lines per hunk — PREFER it. Open a file at HEAD with Read ONLY when you genuinely need wider context, and then Read a WINDOW (offset/limit) around the cited HEAD line range — NEVER read a whole file (whole-file reads bloat the transcript and trigger compaction mid-sweep). You may open files for context, but you must NEVER flag a line outside the surviving set — it is either someone else's code or already gone. Because already-fixed lines are excluded by construction, assume NOTHING here is "already known/fixed": a surviving line is live debt.`,
    ``,
    `Apply the Smatchet code-review checklist (see AGENTS.md + docs/agent-rules/cpp-rules.md):`,
    `  • C++14 hard (no string_view/optional/variant/structured-bindings/if-constexpr); must compile MSVC + Clang.`,
    `  • Logging: LOG_{DEBUG,INFO,WARN,ERROR,TRACE} only — never printf/std::cerr (except the named pre-logger-init / CLI-stdout exceptions).`,
    `  • RAII: no raw new/delete; unique_ptr+make_unique; const& non-trivial params; move on last use; unique_ptr<T> member in a header needs full T.`,
    `  • Exceptions: empty catch(...){} = CRITICAL.`,
    `  • UI-thread (Pillar-1/2): NO sync disk/network/p4/subprocess I/O on the ImGui render path (functions under Ui/ or named Draw*/Render*); >100ms block w/o a cue = CRITICAL. Per-frame heap alloc / re-parse on render path = perf finding.`,
    `  • Never crash: bounds-checked, no silent UB, graceful degradation.`,
    `  • No GLFW/OpenGL includes in Source/Core/include headers (DX12 dual-target); no IMGUI_USE_WCHAR32 redefine; no brace-list reassignment for nlohmann.`,
    `  • Correctness/data-loss bugs (wrong external mutation, dropped/clobbered user edits, migration key mismatches).`,
    `  • Gate/test scripts that fail-OPEN or false-PASS (e.g. passed=0&&failed=0 exits 0; unanchored exclude tokens; comment-classifier exempting real code).`,
    `  • Docs: broken cross-refs / moved-script paths / stale line-pins / doc-vs-code drift on a gated literal (allow-lists, ADR/plan status).`,
    ``,
    `Severity: CRITICAL = crash/data-loss/UI-freeze/security or a blocking gate that lets bad code through. HIGH = serious correctness or a real fail-open. MEDIUM = latent/conditional defect or meaningful drift. LOW = cosmetic/doc/dormant. Skip pure formatting nits.`,
    `userVisible=true ONLY for user-facing product defect / correctness / safety (-> GitHub Issue per ADR-0014); everything internal (tooling, gates, docs, tech-debt) = false.`,
    ``,
    `STEP 4 — return the structured object: pr=${pr}; sha from the digest header; status="findings" if any, else "clean"; summary = the digest's one-line survivor count; findings cited at their HEAD line numbers. Be terse and concrete in problem/fix. Do not invent findings — a clean PR returning [] is the common, correct case.`,
  ].join('\n')
}

const results = await parallel(prs.map((pr) => () =>
  agent(promptFor(pr), { label: `review:#${pr}`, phase: 'Review', model: 'opus', schema: SCHEMA })
))

const ok = results.filter(Boolean)
const withFindings = ok.filter(r => r.status === 'findings')
const clean = ok.filter(r => r.status === 'clean')
const superseded = ok.filter(r => r.status === 'superseded')
const errored = ok.filter(r => r.status === 'error')
const died = results.length - ok.length

const allFindings = withFindings.flatMap(r => (r.findings || []).map(f => ({ ...f, pr: r.pr, sha: r.sha })))
const bySev = sev => allFindings.filter(f => f.severity === sev)

log(`Sweep done: ${ok.length}/${results.length} returned — ${withFindings.length} with findings, ${clean.length} clean, ${superseded.length} superseded, ${errored.length} error, ${died} died.`)
log(`Findings: ${bySev('CRITICAL').length} CRITICAL, ${bySev('HIGH').length} HIGH, ${bySev('MEDIUM').length} MEDIUM, ${bySev('LOW').length} LOW.`)

return {
  totals: {
    reviewed: results.length,
    returned: ok.length,
    withFindings: withFindings.length,
    clean: clean.length,
    superseded: superseded.length,
    errored: errored.length,
    died,
    critical: bySev('CRITICAL').length,
    high: bySev('HIGH').length,
    medium: bySev('MEDIUM').length,
    low: bySev('LOW').length,
  },
  // the work-list gate's COMPUTED coverage triple, passed through verbatim when
  // args came from `historical-review-worklist.sh --json` (null otherwise) — a
  // batch header should quote THIS, never a hand-transcribed stderr line.
  coverage,
  findings: allFindings,
  errors: errored.map(r => ({ pr: r.pr, summary: r.summary })),
  diedPrs: prs.filter((_, i) => !results[i]),
  perPr: ok.map(r => ({ pr: r.pr, sha: r.sha, status: r.status, summary: r.summary, count: (r.findings || []).length })),
}
