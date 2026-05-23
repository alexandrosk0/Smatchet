# Smatchet agentic infrastructure — evaluation

**Date**: 2026-05-23
**Scope**: non-C++ surface only — `AGENTS.md`, `.claude/CLAUDE.md`, every doc linked
from those (`docs/agent-rules/*.md`, `docs/perforce/*.md`, `docs/harness/**.md`,
`docs/backlog/AGENT_SELF_IMPROVEMENT.md` + per-category live files), every agent
definition under `agents/`, and the bash / Python / PowerShell glue under
`scripts/dev/` + `scripts/setup-harness.{sh,ps1}` + the bats tests at
`tests/bats/`.
**Branch**: `develop` @ `96ab99f` (clean; in sync with `origin/develop`).
**Reviewer**: orchestrator + two parallel research subagents.

This is a research deliverable. As initially landed (CL 137 / PR #420 commit
`b0d2047`), this doc shipped alongside two non-eval files —
`scripts/dev/merge-gates.sh` and `tests/bats/merge_gates.bats` — that
implement the C1 + C2 fixes the doc names; bundling the eval with the first
PR it motivates keeps the PR self-documenting. Other findings in this doc
land in follow-up PRs (see § 1a Implementation status). The "this audit
itself touched no other files" guarantee that the original phrasing
implied applied only to the *audit pass* — the punch-list fixes are
explicitly out of audit scope and ship as separate slices.

---

## 1a. Implementation status

Updated 2026-05-23 (live; this section is the running ledger of which
findings are addressed in which PR).

| PR | Findings | Branch | State |
|---|---|---|---|
| [#419](https://github.com/alexandrosk0/Smatchet/pull/419) | **C3** — `manual-locks-render-sync.sh` `--force` → `--force-with-lease` + sync-branch fetch | `fix/manual-locks-render-force-with-lease` | **MERGED** (squash `1cb370d`) |
| [#420](https://github.com/alexandrosk0/Smatchet/pull/420) | **C1** + **C2** — `SKIP_MERGE_GATES` guard + `set -uo pipefail` + fail-closed jq defaults + 2 new bats tests + this eval doc | `fix/merge-gates-skip-and-fail-closed` | **MERGED** (squash, ~19:14 UTC) |
| [#421](https://github.com/alexandrosk0/Smatchet/pull/421) | **H7** + **H8** + **H10** — 8 agent version bumps + perf-instrument/measure Helper-class sections + p4-janitor Maintenance-class sections + delegation.md table row | `chore/agent-contract-hygiene` | **MERGED** (squash `4a8772c`) |
| [#422](https://github.com/alexandrosk0/Smatchet/pull/422) | **H6** + **H13** + **H15** — `lock-claim-p4.sh` CAS stream capture + `lock-release.sh` case-glob split + path-boundary anchor on `[Ss]matchet*` repo check | `fix/lock-primitives-hardening` | Open — rebased onto develop to clear cross-PR perf-measure.md CR thread |
| [#423](https://github.com/alexandrosk0/Smatchet/pull/423) | **H4** + **H5** — `p4-task-stream-to-pr.sh` defensive awk env-pass + `p4-task-stream-gc.sh` subshell counter loss + Root-with-spaces parsing | `fix/p4-task-stream-hardening` | **MERGED** (squash `b6320e9`) |
| [#424](https://github.com/alexandrosk0/Smatchet/pull/424) | **H1** + **H12** — APPROVED CR ignores `cr_open` per spec + `cr_installed` probe distinguishes 404 from auth/transient errors (fail safe) + 5 new bats tests | `fix/merge-gates-h1-h12` | **MERGED** (squash `37ab31b`) |
| [#426](https://github.com/alexandrosk0/Smatchet/pull/426) | **H16** — STALE_RESOLVED — CR thread resolution counts as accept; addresses the real gate-vs-CR mismatch that wedged #421/#422/#423/#425 today | `fix/merge-gates-cr-thread-resolution` | In flight |

Still on the punch list (highest-leverage first):

- **C4** (P0) — draft-PR CR-bypass. Watcher hasn't shipped the
  `ensure_pr_ready_for_review` fix yet, so every PR in this session needed
  a manual `gh pr ready` before CR would review. 3-pronged fix designed in
  `docs/backlog/agent-self-improvement/process.md` (~3 h).
- **CR-triage backlog** — PRs #421 / #422 / #423 have CR-actionable
  findings (4 / 1 / 2 respectively). Watcher gave up after the configured
  triage attempts. Needs manual triage / fix / push round before they
  unblock. Captured here so it's not silently forgotten.
- **H2** — `gh_pr_ready_idempotent` matches on English error strings only.
- **H9** — `architect.md` Investigator-vs-Implementer output drift (needs
  design decision on adding a new "Design" class).
- **H11** — trigger keyword collisions across agents (needs
  disambiguation rules).
- **Eval-punch-list item 4** — agent-contract test extension to enforce
  P2 systemic findings.
- **Eval-punch-list item 5** — bash-lint sweep via `shellcheck` across
  the rest of `scripts/dev/*.sh`.
- **Eval-punch-list item 6** — bats coverage for lock primitives.
- **Eval-punch-list item 7** — skill ↔ agent parity gate.

---

## 1b. Next-session resume checklist

**Session 1 (2026-05-23) shipped 8 PRs** (#419–#424 + CR-fix follow-ups on
#421 and #423). Two merged (#419 C3, #420 C1+C2+eval-doc). Six waiting
for the watcher to clear gates + auto-merge.

### Step 1 — Drain the queue (do first, ~5 min)

Run **before any new work**:

```bash
# Refresh local develop to pick up any PRs the watcher merged overnight.
git checkout develop && git pull --ff-only origin develop

# Confirm watcher state on all 6 open PRs from session 1.
python scripts/dev/merge-watcher-cli.py status
```

Expected on resume: PRs #421 / #422 / #423 / #424 each show one of —
`PR_CLOSED_OR_MERGED` (watcher merged them — good),
`BLOCKED` (still polling — fine),
`TRIAGE_BUDGET_EXHAUSTED` (watcher gave up again — see Step 2).

For each PR that **didn't auto-merge** (`OPEN` + non-merged state):

```bash
gh pr view <N> --json state,mergeable,reviewDecision,statusCheckRollup
gh api repos/alexandrosk0/Smatchet/pulls/<N>/reviews \
  | python -c 'import json,sys; [print(r["state"], r["user"]["login"], r["body"][:140]) for r in json.load(sys.stdin) if "coderabbit" in r["user"]["login"].lower()]'
```

If CR found new actionable findings after the session-1 fix commits — triage
+ push another round (same pattern as session 1).

If everything is green and CR is clean but watcher is stuck — re-register
to reset triage budget (per session 1's pattern):

```bash
python scripts/dev/merge-watcher-cli.py unregister <N>
python scripts/dev/merge-watcher-cli.py register <N>
```

### Step 2 — Update § 1a Implementation status

After the queue drains, edit this doc's § 1a table to mark
#421 / #422 / #423 / #424 as MERGED. Keep #422's row note about the
sweep CR comment (CR'd `agents/perf-measure.md` on a PR that didn't touch
it; resolved by #421's fix). Ship the eval-doc update via a small
docs-only PR or direct-push to develop per the open backlog P2 about
plan-revision direct-pushes.

### Step 3 — Pick the next batch from the punch list

Order by leverage (highest first):

1. **C4** (P0, ~3 h) — draft-PR CR-bypass fix. The watcher's
   `ensure_pr_ready_for_review` is only called in the merge path, not at
   registration / first poll. Every PR in session 1 needed a manual
   `gh pr ready` before CR would review. Three-pronged fix already designed
   in [`docs/backlog/agent-self-improvement/process.md`](../backlog/agent-self-improvement/process.md)
   2026-05-21 P0: (a) flip ready in `git-janitor` before merge-gates poll,
   (b) require a non-empty CR review (not just SUCCESS StatusContext),
   (c) route through `coderabbit-triage` automatically. Touches
   `scripts/dev/merge-watcher.py` line ~782 + `scripts/dev/merge-gates.sh`
   + `merge-gates.graphql` + `agents/git-janitor.md`. **Will conflict
   with anything else touching merge-gates.sh** — sequence carefully.
2. **H2** (~30 min) — `gh_pr_ready_idempotent` at
   `scripts/dev/merge-gates.sh:411` matches English error strings only
   (`*"not in draft state"*|*"already marked ready"*`). Real gh exit
   strings vary by version + locale. Fix: probe `gh pr view --json isDraft`
   as a positive check before falling back to the case-match. Add bats
   coverage. **Touches merge-gates.sh** — coordinate with C4.
3. **Eval-punch-list item 4** (~2 h) — extend
   `scripts/dev/test-agent-contract.sh` to enforce pattern P2 (agent
   definition entropy). Specifically: assert every agent's frontmatter
   `version:` matches its banner; assert no trigger keyword is owned by
   two agents; assert skill ↔ agent SKILL.md siblings have matching
   `version:` + `triggers:`.
4. **Eval-punch-list item 5** (~3 h) — bash-lint sweep. Add
   `scripts/dev/lint-bash.sh` that runs `shellcheck` over
   `scripts/dev/*.sh` and fails on `SC2086` / `SC2046` / `SC2155`. Fix
   the residue findings in one batch (MEDIUM M2 trap cleanup, M3
   multibyte truncation, M5 PS-incompatible cleanup hints, M6 jq
   injection in `lock-staleness-sweep.sh`).
5. **Eval-punch-list item 6** (~4 h) — bats coverage for lock primitives.
   `tests/bats/lock_claim.bats`, `lock_release.bats`,
   `lock_claim_update.bats`. Mirror the merge_gates.bats stub pattern.
6. **H9** — `architect.md` Investigator-vs-Implementer drift. Needs a
   **design decision before code**: add a new "Design" class to the
   contract table (with `## Goal` / `## Affected components` / `## Interface
   contracts` / `## Implementation handoff`), OR reshape architect's
   output to match Investigator (`## Hypotheses` → `## Evidence` → ...).
   Recommend grilling via `grill-with-docs` skill first.
7. **H11** — trigger keyword collisions. Audit every agent's `triggers:`
   frontmatter against `docs/agent-rules/delegation.md` § Trigger
   auto-activation table. Disambiguate (drop dup keys, or add explicit
   heuristic notes in the prompts).
8. **Eval-punch-list item 7** (~1 h) — skill ↔ agent parity gate. Belongs
   in `test-agent-contract.sh` extension above (item 3).

### Step 4 — Update § 1a as each PR ships

The eval doc's § 1a table is the canonical status ledger. Update it at
every PR-merge boundary so future readers see the cumulative state.

### Avoid in-flight rebases

PRs #421 / #422 / #423 / #424 all branched from develop pre-session-1.
If session 1's PRs are still open at session 2 start, rebasing them onto
the new develop tip (which now has #419 + #420) costs nothing — they
have disjoint write sets from the merged PRs. Don't try to bundle a
session-2 fix into a session-1 PR's branch; ship as a new PR.

### Helpful one-liners

```bash
# Survey all open PRs + their CR state in one shot.
gh pr list --state open --json number,title,statusCheckRollup,reviewDecision \
  --jq '.[] | "\(.number) \(.title) | review=\(.reviewDecision // "none")"'

# Watcher state.
python scripts/dev/merge-watcher-cli.py status

# Drain merged PRs from develop into local checkout.
git checkout develop && git pull --ff-only origin develop

# Standard p4-gated small-change loop — quick reference:
#   p4 reconcile -e <files>  # open files for edit
#   p4 change -i             # create CL via stdin spec
#   p4 shelve -c <N>         # shelve for shelf-review gate
#   AskUserQuestion          # user reviews in P4V
#   p4 shelve -d -c <N>      # delete shelf
#   p4 submit -c <N>         # land on //smatchet/main
#   git checkout -b <branch> && git add <files> && git commit && git push
#   gh pr create --draft && python scripts/dev/merge-watcher-cli.py register <N>
#   gh pr ready <N>          # C4 workaround until C4 ships
```

---

## 1. Executive summary

The agentic infrastructure is **structurally sound and unusually thorough** —
nearly every rule has a concrete script, a bats test, an ADR, or a backlog
entry as evidence. The recent AGENTS.md reduction (PR #417, `96ab99f`) split
the doc into a load-bearing stub + topic files in `docs/agent-rules/` and
removed the worst navigability problem the repo had a month ago.

That said, the audit surfaced **four CRITICAL gaps**, all of which are
contract-vs-implementation disagreements that can let bad PRs merge to
`develop` without the orchestrator noticing:

| # | Category | One-line risk |
|---|---|---|
| C1 | merge-gates | `SKIP_MERGE_GATES=true` is a documentation-only convention — `scripts/dev/merge-gates.sh` never reads it. Any miswired caller bypasses gates silently. |
| C2 | merge-gates | `set -e` is not enabled in `merge-gates.sh`; jq failures silently empty `cr_actionable`, and `[ "$cr_actionable" -gt 0 ]` evaluates empty as 0 → COMMENTED-with-findings can pass. |
| C3 | git discipline | `scripts/dev/manual-locks-render-sync.sh:104` does a **bare `git push --force`** — direct violation of AGENTS.md's globally-banned `--force` rule. |
| C4 | CodeRabbit gate | Open P0 in `process.md` (2026-05-21): draft PRs bypass CR review because `auto_review.drafts: false`, watcher polls a placeholder SUCCESS, never sees an actual review. **Confirmed live on PR #412.** Fix is filed but not yet shipped. |

A further **~15 HIGH** findings cluster around two systemic themes:

- **Bash defensive-programming drift** — `set -euo pipefail` is inconsistent
  across `scripts/dev/`. The same anti-pattern (empty-variable-treated-as-0)
  recurs in 5+ scripts. Hardening is local but the discipline is repo-wide.
- **Agent-definition vs project-rules drift** — eight agents have
  `version: 1` in frontmatter but `· v2` in their banner; the helper-agent
  output contract (`## Spec executed` / `## Result`) is not actually emitted
  by `perf-instrument` or `perf-measure`; `architect.md` declares itself
  Investigator class but writes Implementer-shape reports.

The system is **good enough to ship product code through every day**, but
the bypass surface around the merge gate is wider than the docs admit, and
the agent-output-contract drift is starting to make telemetry unreliable.

---

## 2. What's working well

To put the findings in context — these are the parts that hold up under audit:

- **AGENTS.md reduction (PR #417)** — five topic files in `docs/agent-rules/`
  give every agent a stable URL to link to, and the AGENTS.md stubs are
  short enough to keep loaded in every harness's context.
- **Self-improvement loop** — `docs/backlog/agent-self-improvement/` has 75
  applied entries and 75 live ones across six categories. Agents actually
  flag friction; the orchestrator actually triages. This is the highest-
  leverage process this repo has.
- **Merge-gate test coverage** — `tests/bats/merge_gates.bats` covers all
  seven documented CodeRabbit outcome states, plus pagination, OOB labels,
  `gh` failure, timeout, and the `ask_user_question` shim. Coverage here is
  genuinely good.
- **Stranded-CL recovery contract** — `p4-task-stream-to-pr.sh
  --promote-reviewed-cl` exits 5 on (a) non-existent CL, (b) non-pending,
  (c) wrong client, (d) missing task-stream-id tag, with operator-facing
  cleanup recipe. `test-p4-dual-vcs.sh` scenarios 4+5 cover all three
  refusal paths. **Contract is honoured.**
- **Harness adapter table** is real — agents declare capability tags
  (`semantic-code-search`, `file-skeleton`, etc.) and the harness layer in
  AGENTS.md maps them to concrete tools. Codex / Cursor degrade gracefully
  rather than breaking.
- **Force-push carve-outs are tightly scoped** — only `claude/<id>/*` and
  `agent/<task-stream-id>/*` branches with `--force-with-lease`, only during
  API-500 recovery, only with zero non-self commits ahead. The carve-outs
  are documented in ADR 0005 + ADR 0008 and the global ban is enforced in
  `process-rules.md` § Force-push carve-out.

---

## 3. CRITICAL findings

### C1 · `SKIP_MERGE_GATES=true` is documentation-only (no enforcement)

`AGENTS.md § Merge gates` (and `docs/agent-rules/merge-gates.md:36`)
state: *"`SKIP_MERGE_GATES=true` at session init bypasses all gates. No
per-merge skip. Subagent propagation: orchestrator must explicitly add
`SKIP_MERGE_GATES` to any delegated `git-janitor` invocation's env."*

`scripts/dev/merge-gates.sh` **never reads `SKIP_MERGE_GATES`**. The
override is a pure documentation contract — every caller is trusted to
gate the call themselves. A miswired `git-janitor` invocation, or a
hand-rolled merge call from a new script, would poll regardless and could
auto-merge against the orchestrator's expectations.

**Fix**: add a one-line guard at the top of `poll_merge_gates()`:
```bash
if [ "${SKIP_MERGE_GATES:-}" = "true" ]; then
  echo "GATES_SKIPPED (SKIP_MERGE_GATES=true)"
  return 0
fi
```

### C2 · `merge-gates.sh` lacks `set -euo pipefail`

`scripts/dev/merge-gates.sh:53` enables `set -o pipefail` only. Several
`local x=$(... | jq ...)` assignments compute the CR / CI / user-comment
counts. If `jq` errors (malformed GraphQL response, network blip), the
variable silently sets to empty. The subsequent integer comparison
(`[ "$cr_actionable" -gt 0 ]`) treats empty as **0** → CodeRabbit
`COMMENTED + N>0` (the exact shape that dropped 5 findings on PR #357 per
`process.md` 2026-05-21) can pass.

Same anti-pattern recurs in `merge-gates.sh:281-285` (`cr_open` counting),
`check-required-tools.sh:32`, `agent-progress.sh:18`, `tail-agent.sh:90`,
and `p4-task-stream-gc.sh:99-171`.

**Fix**: add `set -euo pipefail` to all scripts in `scripts/dev/` that
mutate state or feed decisions. Default-init integer vars to `-1` so a
later `-gt 0` test fails closed, not open.

### C3 · `manual-locks-render-sync.sh:104` bare `git push --force`

`AGENTS.md § Project rules § Force-push carve-out`: force-push is
**globally banned** except a narrow `--force-with-lease` carve-out on
`claude/<id>/*` and `agent/<task-stream-id>/*`. This script
unconditionally bare-force-pushes a `lock-render-sync/*` branch. Direct
contract violation.

**Fix**: replace with `git push --force-with-lease origin
"$lock_render_branch"`. The branch shape (`lock-render-sync/*`) is not in
the carve-out exclusion list either — either widen the carve-out
explicitly or rename the branch to fit `claude/<id>/lock-render-*`.

### C4 · Draft-PR CR-bypass (P0 in process.md, confirmed live on PR #412)

`process.md` 2026-05-21 P0: every PR that stays draft through merge
bypasses CodeRabbit review because `.coderabbit.yaml` ships with
`auto_review.drafts: false`. CR's placeholder `StatusContext` fires
SUCCESS regardless of draft state, so `merge-gates.sh` treats CR as
PASSED via the "CR installed, head commit has CR StatusContext SUCCESS"
branch. Six+ session PRs landed without a single CR comment classified
or actioned.

2026-05-22 comment confirms it reproduced on PR #412:
> Manually ran `gh pr ready 412` to unblock CR. Root cause in
> `scripts/dev/merge-watcher.py` line ~782 — fix is to call
> `ensure_pr_ready_for_review` once on first poll.

**Status**: P0 known, three-pronged fix scoped (~3 h), not yet shipped.
Net effect today: every watcher-registered draft PR ships without CR
review unless the user manually flips draft.

---

## 4. HIGH findings

### H1 · APPROVED CR review still blocks on unrelated open threads
`merge-gates.sh:281-285` + the pass check at L384 require **both**
`cr_pass=true` AND `cr_open==0`. AGENTS.md says "APPROVED → pass
unconditionally". On a PR where CR approves but an unrelated thread is
still open, the gate sits indefinitely. Fix: in the pass condition, when
`cr_state==APPROVED`, ignore `cr_open`.

### H2 · `gh_pr_ready_idempotent` matches on English error strings only
`merge-gates.sh:411` checks `case "$out" in *"not in draft state"*…)`.
Real `gh` error strings vary across versions and locales. Any
non-matching variant returns exit 6 → caller halts auto-merge. Fix:
probe `gh pr view --json isDraft -q .isDraft` as a positive check.

### H3 · `p4-task-stream-to-pr.sh` uses `git add -A`
L306-307. Top-of-prompt "Git Safety Protocol" explicitly warns against
`git add -A` for the sensitive-file dragnet reason (`.env`, credentials,
large binaries). On an auto-PR ship path this is exactly the rule's
target case. Fix: stage only paths from `p4 describe -S` / `p4 diff -sa`
for the submitted CL.

### H4 · `p4-task-stream-to-pr.sh` awk injection via PR title
L397-411 interpolates `$pr_title` into `-v t="$pr_title"`. A title with a
double-quote breaks the awk string. Fix: pass via env
(`PR_TITLE=… awk -v t=ENVIRON["PR_TITLE"]`).

### H5 · `p4-task-stream-gc.sh` subshell counter loss + Root-with-spaces
L99-171 the script knows about the subshell-loses-counters bug (warns at
L173) but its workaround `wc -l || echo 0` returns `wc-output\n0` on a
p4-down state. Final summary unreliable. Separately, L154-155
`awk '{print $2}'` on a `Root:` line with `C:\Program Files\…` captures
only the first token; the subsequent `rm -rf "$client_root"` either
no-ops or removes a sibling.

### H6 · `lock-claim-p4.sh` CAS-error stream capture
L108 captures stderr only. Newer p4d versions emit the counter-collision
on stdout. On those, `cas_err` is empty and the retry loop fires for a
real lock conflict, eventually exiting 3 (transient) instead of 1
(held). Caller misclassifies the failure.

### H7 · Agent-definition contract drift (8 agents)
`command-system`, `lua-binder`, `grid-engine`, `mechanic`, `p4-blame`,
`mcp-toolsmith`, `offline-sync`, `unreal-bridge` all carry
`version: 1` in frontmatter but `· v2` in banner. Telemetry
(`agent_version` field, per `delegation.md:230`) pivots on frontmatter →
will undercount v2 work for every PR these agents touch. Skill SKILL.md
files lack `version:` entirely.

### H8 · Helper-agent required sections not emitted
`agents/perf-instrument.md:68` + `agents/perf-measure.md:103` — Helper
class mandates `## Spec executed` → `## Result` per `delegation.md` §
Agent output contract. Neither agent's prompt body actually instructs
the agent to emit either heading. Their `Report:` prose substitutes;
downstream telemetry doesn't key on it.

### H9 · `architect.md` Investigator class drift
Declares `read-only:true` but writes plan-doc bodies (Implementer-shape
sections — `Goal / Affected components / Interface contracts / Risks /
Implementation handoff`). Contract says Investigator must emit
`## Hypotheses` → `## Evidence` → `## Cause` → `## Handoff`. Mismatch.

### H10 · `p4-janitor.md` Maintenance contract not honoured
L92 hand-waves "Standard agent-output contract" instead of actually
emitting `## Pre-flight` → `## Mutations applied` → `## Regression gate`
→ `## Residue requiring user action`. Maintenance-class telemetry blind
to p4-janitor.

### H11 · Trigger-keyword collisions across agents
- `architect` + `mechanic` both legitimately match `refactor`
  (heuristic-disambiguated by file count; not encoded in `triggers:`
  frontmatter).
- `coderabbit-triage` claims `PR review comments` overlapping
  `code-review`'s `PR review`.
- `perf-gatekeeper` claims `regression check` overlapping
  `debug-detective`'s `regression`.
- `perf-instrument` and `perf-measure` carry `triggers:` frontmatter but
  are not in `delegation.md:147-160` — they're helper-dispatched, not
  user-keyword-routed. Document this or drop the field.

### H12 · `cr_installed` auto-probe fails open on non-404 errors
`merge-gates.sh:100-105` probes `repos/$owner/$repo/contents/.coderabbit.yaml`
via `gh api`. On a brand-new repo, auth failure, or transient
`gh` error, the probe returns non-zero → `cr_installed=false` → CR gate
auto-passes. Should distinguish 404 (truly absent) from other errors
(treat as installed, fail safe).

### H13 · `lock-release.sh:74` brittle case-glob pattern
`*"unable to delete"*"does not exist"*)` is a multi-substring glob; a
real network error like `unable to delete '…': remote rejected` matches
the first substring and gets treated as a no-op (lock leak). Fix: AND
the two substrings explicitly.

### H14 · Stale cross-link to deleted `ClaudeCodeLocalRunner` infrastructure
`docs/agent-rules/delegation.md:213` ("the `agent/<id>` shape is GONE")
+ ADR 0005 (Withdrawn as historical) + the carve-out exclusion list
reintroduces `agent/<task-stream-id>/*` for p4 promotion. The doc is
internally consistent but a fast reader could misread; a recently-
applied entry in `applied.md` references the cleanup. Worth a final
grep-sweep for `ClaudeCodeLocalRunner` / `handoff-implementer` /
`pr-iterator` references in agent files.

### H15 · `lock-claim.sh:67-72` repo-match regex too loose
`case` pattern `*[Ss]matchet*` matches anywhere in the URL — a fork
named `Smatchet-fork` matches; a non-Smatchet repo whose path contains
"smatchet" also matches. Lock could be claimed against the wrong repo
on an unusual clone.

---

## 5. MEDIUM findings

| # | File / area | Issue |
|---|---|---|
| M1 | `merge-gates.sh:139` | No jitter — three concurrent merge-watchers polling the same PR lock-step every 60 s, slamming the gh API |
| M2 | `vexp-strip-agents-md.sh:68-70` | No `trap` to clean `.tmp` on Ctrl-C; leaves debris |
| M3 | `p4-task-stream-to-pr.sh:155-158` | `cut -c1-64` on multibyte UTF-8 title can produce invalid UTF-8 branch name (Git+NTFS rejects) |
| M4 | `is-pure-docs-diff.sh:47` | `^[A-Z][A-Z_]*\.md$` allow-list would classify `T.md` test files as pure-docs |
| M5 | Cleanup hints printed by `lock-claim-p4.sh`, `p4-task-stream-to-pr.sh` | Use `&&` chain — broken in PowerShell 5.1, which Windows users land in |
| M6 | `lock-staleness-sweep.sh:127-133` | `jq` filter has shell-interpolated `${title}`; jq-injection if slug validator ever relaxes |
| M7 | Skill ↔ agent duplication | `perf-instrument`, `perf-measure`, `perf-gatekeeper` exist as both `agents/*.md` AND `agents/_shared/skills/<name>/SKILL.md`. No parity test ensures they don't drift |
| M8 | `coderabbit-triage.md:103` | References `CLAUDE.md` for overrides; canonical is `AGENTS.md` per `Agent file locations` |
| M9 | `_shared/skills/*` SKILL.md files | No `version:` field — telemetry pivot misses skill versions |
| M10 | `agents/test-author.md:23` | `delegates-to: command-system`, but `command-system` is terminal per `delegation.md:289` |
| M11 | Bats coverage gap | Zero bats for `lock-claim*`, `lock-release*`, `p4-task-stream-to-pr.sh`, `is-pure-docs-diff.sh`, `vexp-strip-agents-md.sh`. Lock primitives have only `test-lock-primitives.sh` integration |
| M12 | `merge-gates.sh:411` `gh_pr_ready_idempotent` | Cross-platform: gh exit codes + error strings differ by OS + version |
| M13 | Backlog open-issue cross-reference | `infra.md` 64-75 (tracker TU split) and `tooling.md` 2026-05-22 P1 (merge-watcher.py) name agents that aren't aware of the open work |

---

## 6. LOW findings (selected)

| # | File / area | Issue |
|---|---|---|
| L1 | `tail-agent.sh:90` | `ahead=$(git rev-list --count $head_sha...origin/develop)` unused; shadowed by L92 |
| L2 | `lock-claim-update.sh:1-4` | Header comment says `started_at`; JSON schema field is `started` |
| L3 | `p4-task-stream.sh:127` | `hostname` on MSYS2 may return UNC-prefixed name — cosmetic |
| L4 | `locks-show.sh:74-78` | O(N) Python heredoc per ref — slow above ~100 locks |
| L5 | `test-p4-dual-vcs.sh:142-144` | No `trap` to clean up `$TRACE_DIR` on early failure |
| L6 | `architect.md:32` | Hard-references `mcp__vexp__get_skeleton` (Claude-Code-specific) instead of abstract skeleton-capability tag |
| L7 | `debug-detective.md:165` | References `feedback_autonomous_ship_loop` user-private memory token — opaque to readers without that memory |
| L8 | `p4-janitor.md:101` | `## Self-improvement` is empty placeholder instead of canonical text |
| L9 | Banners use emojis | Conflicts with the no-emoji rule, but banners are agent-emitted not assistant-narration. Cosmetic |

---

## 7. Systemic patterns

Three patterns explain the majority of individual findings:

### P1 · Docs are stricter than scripts (contract-as-aspiration)

`SKIP_MERGE_GATES` (C1), force-push ban (C3), APPROVED-passes-
unconditionally (H1), `--force-with-lease` (M5 cleanup hints), `set -e`
discipline (C2 + 5 follow-ons): the documented contract is tighter than
what the scripts enforce. The orchestrator + every agent assume the
docs are the truth. **A miswired caller can quietly defeat any of these
guardrails today.**

Action: add a `scripts/dev/test-agent-contract.sh` clause that grep-
verifies the script enforces every load-bearing rule its docstring
claims, and wire it into `test-all.sh`. Cheap, catches future drift.

### P2 · Agent-definition entropy

The agent-output contract, frontmatter `version:`, `triggers:`, and
banner versioning have drifted across 8+ agents. The
`Implementer/Investigator/Helper/Maintenance` class system is
under-enforced. Three agents (`perf-instrument`, `perf-measure`,
`perf-gatekeeper`) live as both agents and skills with no parity gate.

Action: add a `scripts/dev/test-agent-contract.sh` clause to verify
every agent file:
- has a `version:` integer that matches its banner;
- emits the required sections for its class (string-grep);
- doesn't claim a trigger keyword owned by another agent;
- if a sibling skill exists, both files have matching `version:` +
  frontmatter `delegates-to:` / `triggers:` agreement.

### P3 · Bash defensive-programming inconsistency

`set -e` is rarely enabled. Empty-variable-treated-as-0 fires in
`merge-gates.sh` (C2), `agent-progress.sh`, `tail-agent.sh`,
`p4-task-stream-gc.sh`, `check-required-tools.sh`. `trap`-cleanup is
missing in `vexp-strip-agents-md.sh`, `test-p4-dual-vcs.sh`. Quoting
hardness varies (H4 awk injection, M6 jq injection).

Action: add a `scripts/dev/lint-bash.sh` that runs `shellcheck` over
`scripts/dev/*.sh` and fails on `SC2086` / `SC2046` / `SC2155`. Existing
`shellcheck`-compatible code reviews are not automated.

---

## 8. Recommendations — prioritised punch list

Highest leverage first. Each item is bounded; none requires architectural
change.

1. **Close C1+C2 in one PR** (≈2 h). Add `SKIP_MERGE_GATES` guard at the
   top of `poll_merge_gates`; add `set -euo pipefail` + integer var
   defaults in `merge-gates.sh`. Bats coverage in
   `tests/bats/merge_gates.bats` for both paths.
2. **Fix C3** (≈10 min). One-line change:
   `manual-locks-render-sync.sh:104` → `--force-with-lease`.
3. **Ship C4** (≈3 h). The fix is fully designed in `process.md`
   2026-05-21 P0 — three prongs (flip-ready in `git-janitor`, require
   non-empty CR review, route through `coderabbit-triage`).
4. **Agent-contract test** (≈2 h). `scripts/dev/test-agent-contract.sh`
   already exists per `tests/bats/`. Extend it to enforce P2 above.
5. **Bash-lint sweep** (≈3 h). Add `shellcheck` to
   `scripts/dev/lint-bash.sh`; fix the H5 + H6 + H13 + M2 + M3 +
   M6 + M11 findings in one batch.
6. **Bats coverage for lock primitives** (≈4 h). One bats file per
   primitive (`lock_claim.bats`, `lock_release.bats`,
   `lock_claim_update.bats`, `p4_task_stream_to_pr.bats`,
   `is_pure_docs_diff.bats`).
7. **Skill ↔ agent parity gate** (≈1 h). Add a parity test asserting
   `agents/<name>.md` and `agents/_shared/skills/<name>/SKILL.md` carry
   matching `version:` + `triggers:`.

After these seven the residue is LOW-severity cosmetic + ~15 process-rule
backlog items already triaged for the next sweep.

---

## 9. Out of scope (called out for honesty)

- **No C++ code reviewed.** User explicitly scoped this evaluation to
  the non-C++ surface. The CRITICAL bugs in `bug.md` (code-color slices
  not invoking `Colorize`, SSE flush boundary synthesis, AiClientFactory
  raw `new`) are real but not this document's subject.
- **No end-to-end behavioural test of the merge-gates poller** — audit
  was static + spec-based. A live test against a real CR-installed PR
  would catch some script defects faster than reading them.
- **CodeRabbit configuration audit** — `.coderabbit.yaml` was not
  reviewed in detail. The `auto_review.drafts: false` setting drives C4
  but the rest of the config (path filters, rule severity) was not
  inspected.
- **Harness-specific glue** — `.claude/hooks/*` scripts and Cursor /
  Codex setup were not deeply inspected beyond `setup-harness.sh`'s view
  of them.

---

## 10. Appendix — files inspected

- `AGENTS.md` (repo root)
- `.claude/CLAUDE.md` (loaded into session)
- `docs/agent-rules/{delegation,ship-loops,merge-gates,process-rules,ux-pillars,golden-image-approval}.md`
- `docs/perforce/{AGENT_FLOWS,SETUP,RUNBOOK}.md`
- `docs/harness/SETUP.md`
- `docs/backlog/AGENT_SELF_IMPROVEMENT.md`
- `docs/backlog/agent-self-improvement/{bug,security,process,tooling,infra,test,external-blockers,applied}.md`
- `agents/*.md` (22 files) and `agents/_shared/skills/**/SKILL.md` (5 files) and `agents/_shared/token-tracking/{SKILL.md,README.md}`
- `scripts/dev/{merge-gates.sh, merge-gates.graphql, merge-gates-prompt.sh, p4-task-stream.sh, p4-task-stream-to-pr.sh, p4-task-stream-gc.sh, p4-reconcile-check.sh, lock-claim.sh, lock-claim-p4.sh, lock-claim-update.sh, lock-release.sh, lock-release-p4.sh, locks-show.sh, lock-staleness-sweep.sh, locks-render-markdown.sh, manual-locks-render-sync.sh, agent-progress.sh, tail-agent.sh, is-pure-docs-diff.sh, vexp-strip-agents-md.sh, test-agent-contract.sh, test-p4-dual-vcs.sh, plan-doc-table-probe.sh, check-required-tools.sh}`
- `scripts/setup-harness.sh`
- `tests/bats/{merge_gates.bats, merge_watcher.bats, merge_watcher_integration.bats}`

Two parallel research subagents produced the per-file deep-dives that fed
findings H7–H11 (agent definitions) and C1–C3, H1–H6, M1–M13 (scripts).
Their raw reports are not checked in — synthesised into this document only.
