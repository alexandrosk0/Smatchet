# Plan: Merge Gates — CI green + CodeRabbit + user comments before merge

## Context

Current ship-loop: `diagnose → fix → build → commit → push → open PR → squash-merge → cleanup`. No CI wait, no comment-resolution gate, no CodeRabbit check. Post-ship protocol offers 4 options: Manual verify, Review PR, Squash-merge, Done.

No C++ changes. Smatchet app not running. Gate evaluation via `gh api` shell commands in the agent prompts. Only AGENTS.md + git-janitor.md change.

---

## 1. AGENTS.md — ship-loop sequence

**Replace** ship-loop (line 77):
```
BEFORE: diagnose → fix → build → commit → push → open PR → squash-merge → cleanup
AFTER:  diagnose → fix → build → commit → push → open PR → [gate-check] → squash-merge → cleanup
```

`[gate-check]` = orchestrator evaluates gates via `gh api` before merging:
- If gates pass: proceed squash-merge
- If gates block: surface live status to user, offer skip
- If user said "merge when green": stay in gate-check loop, auto-merge when clear

---

## 2. AGENTS.md — merge gates section (new, after ship-loop)

```
### Merge gates

After PR opened, before squash-merge, orchestrator checks three gates:

1. **CI gate**: `gh api repos/{o}/{r}/commits/{sha}/check-runs` — all required
   checks must have `conclusion == "success"`. Any failure or pending blocks.
2. **CodeRabbit gate**: `gh api repos/{o}/{r}/issues/{n}/comments` — no
   unresolved comments from `coderabbitai[bot]` (exclude comments starting
   with `<!-- smatchet-handoff -->`).
3. **User comment gate**: same endpoint — no unresolved non-bot comments
   (exclude bot-login users + marker-prefixed comments).

All three gates enabled by default. Disable individually by setting the
corresponding env var to false: `SMATCHET_MERGE_GATE_CI=false`,
`SMATCHET_MERGE_GATE_CODERABBIT=false`, `SMATCHET_MERGE_GATE_USER=false`.

Override: pass `SKIP_MERGE_GATES=true` to bypass all gates and merge
immediately.
```

---

## 3. AGENTS.md — post-ship protocol

**Replace** current 4 AskUserQuestion options (lines 93-99) with 5:

1. **Manual verify** — user drives before merge (unchanged)
2. **Review PR** — user reads diff on GitHub (unchanged)
3. **Wait for gates** — orchestrator stays in gate-check loop; auto-merges when CI green + all comments resolved. Shows inline status ("CI: 2/5 passing, CodeRabbit: 3 unresolved"). New default when gates enabled.
4. **Merge now (skip gates)** — bypass all gates, squash-merge immediately. Hidden when `SKIP_MERGE_GATES` is locked to false.
5. **Done** — no further action, PR stays draft (unchanged)

Skip-condition: user already said "merge when green" → auto-poll until gates pass, then merge without asking.

---

## 4. git-janitor.md — gate check before merge

**Current flow**: verify mergeable → squash-merge → delete branch → plan revision

**New flow**: verify mergeable → **check gates** → squash-merge → delete branch → plan revision

**Gate check block** (pseudocode for the agent prompt):

```bash
check_merge_gates() {
    local owner repo prNumber sha skip="$1"

    # CI gate
    local checks=$(gh api "repos/$owner/$repo/commits/$sha/check-runs" --jq \
        '[.check_runs[] | {name, status, conclusion}]')
    local failures=$(echo "$checks" | jq '[.[] | select(.status=="completed" and .conclusion!="success")] | length')
    local pending=$(echo "$checks" | jq '[.[] | select(.status=="in_progress" or .status=="queued")] | length')
    if [ "$failures" -gt 0 ]; then echo "BLOCKED: $failures checks failing"; return 1; fi
    if [ "$pending" -gt 0 ]; then echo "PENDING: $pending checks still running"; return 1; fi

    # Comment gates
    local comments=$(gh api "repos/$owner/$repo/issues/$prNumber/comments")
    local coderabbit_unresolved=$(echo "$comments" | jq \
        '[.[] | select(.user.login=="coderabbitai[bot]") |
          select(.body | startswith("<!-- smatchet-handoff -->") | not)] | length')
    local user_unresolved=$(echo "$comments" | jq \
        '[.[] | select(.user.login != "coderabbitai[bot]") |
          select(.body | startswith("<!-- smatchet-handoff -->") | not)] | length')

    if [ "$coderabbit_unresolved" -gt 0 ]; then echo "BLOCKED: $coderabbit_unresolved CodeRabbit comments"; return 1; fi
    if [ "$user_unresolved" -gt 0 ]; then echo "BLOCKED: $user_unresolved user comments"; return 1; fi

    echo "PASSED"; return 0
}
```

If blocked and `--skip-gates` not passed: HALT with status message "Merge gate blocked: CI failure / N unresolved comments. Pass --skip-gates to override." Wait for user instruction.

Gate check is best-effort (gh CLI must be installed and authenticated). If `gh` unavailable, log warning and proceed (no silent failure).

---

## 5. Override paths

Three ways to skip gates:
1. **Post-ship option 4**: "Merge now (skip gates)" in AskUserQuestion
2. **git-janitor `--skip-gates`** flag bypasses all gate checks
3. **Env var `SKIP_MERGE_GATES=true`** at orchestrator level disables gates entirely for the session

---

## Files changed (2 files only)

| File | Change |
|------|--------|
| `AGENTS.md` | Ship-loop sequence, new merge-gates section, post-ship protocol 4→5 options |
| `agents/git-janitor.md` | Gate-check step before merge, `--skip-gates` flag, gate-check shell logic |

---

## Verification

1. **Doc review**: ensure AGENTS.md + git-janitor.md produce correct agent behavior
2. **Manual walk**: simulate a PR with red CI → agent reads check-runs → gates block → user "merge now" → gates skip
3. **Manual walk**: git-janitor runs `check_merge_gates`, finds blocked condition, halts with status message
