# Agent docs improvements — 5-action P2/P3 backlog clearance

**Context**: Eight structural / governance issues raised in agent feedback (AGENTS.md length, backlog overflow, missing autonomy defaults, stale security surface, thin glossary, lock terminology, vexp coupling, missing post-ship protocol). After validation against the tree + backlog cross-check + design-doc clash check, scope locked to 5 high-value P2/P3 actions that clear the most pressing fires. Remaining 4 items parked. Total implementation: ~95 min.

---

## Design-doc clash check

Validated against `docs/plans/active/agentic-coding-handoff.md`, `agentic-flow-implementation.md`, `agentic-triage-flow.md`:

- **Action 3 clash found** — backlog entry's `scripts/dev/worktree-spawn.sh` does not exist (Glob: 0 hits). Real worktree creation lives in `ClaudeCodeLocalRunner.cpp` per handoff doc L143 (already bases on `origin/develop` + auto-fetch via `handoff.auto_fetch_before_worktree`). The Phase D/E pain was the `claude/<id>` worktrees spawned by the Claude Code harness itself — separate path, mechanism unknown. Action 3 re-scoped to **investigate harness-side spawn**.
- **Action 4 insufficiency found** — agentic-handoff adds new attack surfaces (sentinel files, env allow-list, spawned-claude subprocess, gh PAT) absent from the AI-only bullet. Action 4 expanded to **two bullets** (AI + Handoff/harness), same time budget.
- Actions 1, 2, 5 — no clash.

## Validation Pass (against current tree)

| # | Original feedback claim | Tree truth | Status |
|---|---|---|---|
| 1 | AGENTS.md 515 lines | **514** lines | valid, **PARKED** |
| 2 | tooling.md 25 open items | **27** entries (`^- YYYY-MM-DD` count) | valid, in scope |
| 3 | Autonomous ship-loop missing from AGENTS.md | Referenced at L286 / L299 / L301 as memory-pointer only; policy text private | valid, in scope (backlog `process.md` 2026-05-17 P2) |
| 4 | security-review.md missing AI surfaces | grep `OpenAi\|Anthropic\|Ollama\|AiClient` in `agents/security-review.md`: **0 hits**; 21 `Source_Core/**/Ai*.{cpp,h}` files exist | valid, in scope (backlog `process.md` 2026-05-17 P2) |
| 5 | Post-ship turn-end protocol missing | Not in AGENTS.md; user memory `feedback_post_ship_prompt.md` only | valid, in scope (backlog `process.md` 2026-05-17 P3) |
| 6 | CONTEXT.md glossary thin (8 entries) | **9** entries in 17 lines | valid, **PARKED** |
| 7 | Lock terminology inconsistent | `plan-lock` 187 hits, `lock-claim` 69, `lock-slug` 31, `holds-lock` 4 — role-distinct, not synonyms | partial, **PARKED** (glossary-only) |
| 8 | vexp section harness-specific | AGENTS.md L482–514 auto-regenerated block | valid, **PARKED** |
| — | (added from backlog) Worktree bootstrap base on stale HEAD | `process.md` L46–50 P2, hit Phase D + Phase E | valid, in scope (re-scoped to investigation) |

---

## In-Scope Actions (5)

### Action 1 · Ship autonomous ship-loop section in AGENTS.md (P2, ~20 min)

**Source**: `docs/backlog/agent-self-improvement/process.md` L16–20 (2026-05-17 orchestrator P2).

**Insertion point**: AGENTS.md between L121 (end of UX Pillars) and L122 (start of Project rules). New top-level `## Autonomous ship-loop default`.

**Required content** (verbatim from backlog entry):
- Full sequence: diagnose → fix → build → commit → push → open PR → squash-merge → `git-janitor` cleanup → backlog entry, all in one turn
- Clarifications batched once at the start via `AskUserQuestion`
- Exception list:
  - Debug-mode pause-loop (already at L284)
  - Destructive ops outside loop (L99 destructive git ops)
  - Cross-repo / external-service mutations
  - Anything not previously authorised in a durable rule (CLAUDE.md / AGENTS.md)
- Cross-link from § Delegation (L271) + debug-mode pause-loop (L284)

**Post-action**: archive backlog entry to `docs/backlog/agent-self-improvement/applied.md`.

---

### Action 2 · Sweep tooling.md open entries (P3 cadence, ~1 h)

**Source**: trigger when category > ~20 (per `docs/backlog/AGENT_SELF_IMPROVEMENT.md` triage spec). Current count: 27.

**Procedure**:
- Add `## Triage log` at top of `tooling.md` with sweep date 2026-05-18 + outcome counts: `before` / `moved-to-parked` / `dedup-merged` / `escalated-to-external-blockers` / `after`. `dedup-merged` count covers entries that restate or near-duplicate an earlier item (e.g. the 2026-05-17 + 2026-05-16 Bucket-E tooltip-helper pair). Each dedup merge writes the survivor's entry with a `Supersedes: <date>` line referencing the dropped sibling so the audit trail is preserved without keeping both rows.
- Reorder: P0 / P1 / P2-with-owner at top; stale P3 to a new `## Parked` section at bottom
- 5 P2 items needing assignment (dated 2026-05-17):
  - test-author — `test-all.sh` worktree baseline drift (8 false fails)
  - test-author — Bucket-E tooltip-content helper
  - code-review — `coverage-delta-gate.sh` dismissable via empty `tests/support/foo.h`
  - code-review — `coverage.sh shift 2` unbound under `set -u`
  - code-review — `coverage.yml` cache key missing `CMakePresets.json`
- Move external-blocked items (mesa-on-CI, gitleaks-install) to `external-blockers.md` if not already
- **No deletions** — every entry retained, reorganised only

**Post-action**: `tooling.md` count stays 27 but P3 parked block is structurally separate; signals cleanup.

---

### Action 3 · Investigate harness-side worktree-spawn mechanism (P2, ~30 min)

**Source**: `docs/backlog/agent-self-improvement/process.md` L46–50 (2026-05-17 orchestrator P2).

**Clash resolution** (per double-check pass): the backlog entry's `scripts/dev/worktree-spawn.sh` reference was conjecture — that script does not exist (Glob 0 hits). Two worktree-creation paths exist:

1. **Agentic-handoff runner-spawned** (`agent/<proposalId>` worktrees) — `Source_Core/src/ClaudeCodeLocalRunner.cpp` per handoff doc L143. Already correct: bases on `origin/develop` + auto-fetch via `handoff.auto_fetch_before_worktree` config flag (default `true`). H3+ shipped — no action.
2. **Claude Code harness-spawned** (`claude/<id>` worktrees) — the Phase D/E pain. Mechanism unknown — likely Claude Code SDK config or an internal harness flag.

**Re-scoped action**: 30 min investigation into path 2.

**Procedure**:
- `git config --get-all worktree.*` + inspect Claude Code SDK/harness config in `.claude/` for spawn-base override
- Read `.claude/settings.json` + `.claude/settings.local.json` for any base-branch config
- Check current worktree's `git config --local --list` for hints
- If config knob exists (e.g. `claude.worktree.baseBranch`) → patch it to `origin/develop` + document in `docs/harness/SETUP.md`
- If no config knob → file as **external-blocker** (`docs/backlog/agent-self-improvement/external-blockers.md`); document the manual workaround (`git checkout -b <branch> origin/develop` after spawn) in `docs/harness/SETUP.md`

**Post-action**: archive backlog entry to `applied.md` (either as fixed or as escalated-to-external-blocker).

---

### Action 4 · Update security-review attack-surface map — AI + Handoff (P2, ~30 min)

**Source**:
- AI bullet: `docs/backlog/agent-self-improvement/process.md` L34–38 (2026-05-17 security-review P2). 5 AI components pre-enumerated.
- Handoff bullet: was AGENTS.md handoff-envelope section (sentinel files, env allow-list, branch naming, PR draft requirement, spawned-claude subprocess) — section deleted by v2 doc-cleanup; the deleted-runtime banner in `docs/backlog/agent-self-improvement/applied.md` records the historical scope.

**Rationale for two bullets** (per double-check pass): the agentic-coding-handoff design adds attack surfaces not covered by the AI bullet alone — sentinel files act as a child-process trust boundary, the spawned `claude` subprocess receives an env block (allow-list discipline), and `gh pr create --draft` uses a GH PAT. Both bullets ship together; both pre-enumerated; total cost unchanged.

**File**: `agents/security-review.md`. Insertion point: L65 (after "Image fetches" bullet, before "Known crash classes").

**Bullet A — AI feature surface** (verbatim from backlog):

```
- **AI feature surface** — provider HTTP clients (OpenAi/Anthropic/Ollama),
  streaming parsers (`AiSseParser` / `AiNdjsonParser`),
  `AgentsMdLoader` (filesystem read into prompt),
  `AiContextBuilder` (data exfil channel for ticket / view / audit data),
  `AiAssistantController` (worker thread + cancel atom + Lua glue surface).
  Per-client checks: URL allow-list / sanitisation, error-body redaction
  (no API keys in logs), buffer caps on streamed responses,
  `AgentsMdLoader` path validation (no `..` traversal),
  Lua `ai.*` rate limit / sandbox-respect.
```

**Bullet B — Handoff / harness surface**:

```
- **Coding-harness handoff surface** — `ClaudeCodeLocalRunner` spawns an
  external `claude` binary inside a per-proposal git worktree;
  sentinel files (`SEED.json` / `USER_RESPONSE.json` / `RUN_RESULT.json` /
  `CLARIFICATION_NEEDED.json` / `PR_URL.txt`) cross the trust boundary
  between Smatchet (parent) and the spawned harness (child).
  Per-component checks:
  (1) **Env allow-list** enforced at spawn time (`PATH, HOME, USER, USERPROFILE,
      TEMP, TMP, SYSTEMROOT, GH_TOKEN, GITHUB_TOKEN, ANTHROPIC_API_KEY` only;
      no `SMATCHET_*` passthrough — verified by env-allow-list doctest).
  (2) **Sentinel-file write contracts** — single-writer single-reader;
      `RUN_RESULT.json` strictly last; runner asserts each file's writer
      matches the documented owner in the deleted AGENTS.md handoff-envelope section (removed by v2 doc-cleanup).
  (3) **Branch-name discipline** — `agent/<proposalId>/<short-slug>`;
      runner refuses `develop` / `main`; harness refuses non-`agent/*` push.
  (4) **PR draft requirement** — every harness-opened PR is `--draft`;
      no `gh pr merge` from harness; no force-push to non-`agent/*`.
  (5) **GH PAT scope** — `GH_TOKEN` / `GITHUB_TOKEN` minimum scope only
      (repo + pull_request); rotate on harness compromise.
  (6) **Worktree GC** — kept on Complete/Failed for inspection;
      explicit `handoff.gc` command; never auto-deletes user-modified files.
```

**Also**: update frontmatter `description:` field (L3) to mention "AI assistant + coding-harness handoff" surfaces.

**Post-action**: archive backlog entry to `applied.md`; file a new applied entry for the handoff surface (the AGENTS.md handoff-envelope section was the cross-link source — section was deleted by v2 doc-cleanup; the entry's historical context is preserved per the deleted-runtime banner in `applied.md`).

---

### Action 5 · Add post-ship turn-end protocol (P3, ~5 min, batch with Action 1)

**Source**: `docs/backlog/agent-self-improvement/process.md` L28–32 (2026-05-17 orchestrator P3).

**Insertion point**: sub-bullet under `## Autonomous ship-loop default` (from Action 1). Same commit.

**Required content**:
- End the turn with `AskUserQuestion` offering 4 canonical options:
  - Manual verify
  - Review PR
  - Squash-merge
  - Done
- **Skip-condition**: user has already said "no more changes" or "ship it and stop" → defer to `git-janitor` instead

**Post-action**: archive backlog entry to `applied.md`.

---

## Parked Actions (4)

Deferred to a later session to avoid doc-restructure churn in the same change set:

| Issue | Reason parked |
|---|---|
| AGENTS.md three-way split | Bigger restructure; ship the missing sections first (Action 1+5), reassess split need after |
| CONTEXT.md glossary expansion (9→32) | Definitions need writing, not copying; ~1 h cost; lower urgency than P2 fires |
| Lock terminology glossary clarification | Feedback overstated — roles distinct, not synonyms; bundle with glossary expansion |
| vexp section → `.claude/CLAUDE.md` | vexp installer re-injection behaviour unknown; needs probe; lower urgency |

Re-file as a single P3 entry in `process.md` referencing this plan, so the parked items don't lose their evidence trail.

---

## File Write Set

| File | Change | Lines impact |
|---|---|---|
| `AGENTS.md` | insert § Autonomous ship-loop default (Actions 1+5) | +30 |
| `agents/security-review.md` | new AI + Handoff surface bullets + frontmatter tweak (Action 4) | +35 |
| `docs/harness/SETUP.md` *(or `.claude/settings.json` patch)* | Action 3 outcome — config knob fix OR manual-workaround documentation | +5–10 |
| `docs/backlog/agent-self-improvement/external-blockers.md` | (conditional, if Action 3 finds no config knob) — escalate worktree-base issue | +6 |
| `docs/backlog/agent-self-improvement/tooling.md` | triage sweep: `## Triage log` + reorder + `## Parked` (Action 2) | structural |
| `docs/backlog/agent-self-improvement/process.md` | remove 4 archived entries (Actions 1, 3, 4, 5) | -36 |
| `docs/backlog/agent-self-improvement/applied.md` | archive 4 entries + 1 handoff-surface entry (Action 4) | +50 |
| `docs/backlog/agent-self-improvement/process.md` | add 1 new entry tracking the 4 parked items | +6 |

---

## Verification

After implementation, each must pass:

- [ ] `grep "Autonomous ship-loop default" AGENTS.md` returns 1 hit (section header)
- [ ] `grep "Manual verify.*Review PR.*Squash-merge.*Done" AGENTS.md` returns 1 hit
- [ ] `grep "AiClient\|AiAssistant\|AiSseParser" agents/security-review.md` returns ≥1 hit
- [ ] `grep "Coding-harness handoff surface" agents/security-review.md` returns 1 hit
- [ ] `tooling.md` contains a `## Triage log` block dated 2026-05-18 + a `## Parked` section
- [ ] `process.md` no longer contains the 4 archived entries
- [ ] `applied.md` gained 5 entries dated 2026-05-18
- [ ] Action 3 outcome documented either in `docs/harness/SETUP.md` or `external-blockers.md`
- [ ] No C++ build verification needed — zero `Source_Core/` / `Plugins/` / `Target_Standalone/` touch across all 5 actions. Docs / agent prompts / backlog markdown only.
- [ ] `tooling.md` `## Triage log` block shows non-zero `dedup-merged` count if any duplicate-pair entries were merged (e.g. the two Bucket-E tooltip-helper entries dated 2026-05-17 + 2026-05-16).

---

## Estimated Cost

| Task | Time | Risk |
|---|---|---|
| Action 1 — ship-loop section | 20 min | low — verbatim from backlog |
| Action 2 — `tooling.md` triage sweep | 60 min | low — pure reorganisation |
| Action 3 — investigate harness-side worktree-spawn | 30 min | medium — outcome (config patch vs external-blocker) unknown until probe |
| Action 4 — security-review AI + Handoff surface | 30 min | low — both bullets pre-enumerated |
| Action 5 — post-ship protocol | 5 min | low — batched with Action 1 commit |
| Verification + `applied.md` archive | 15 min | low |
| **Total** | **~95 min** | |

---

## Branch + PR strategy — single PR, four commits

**Decision**: one PR (branch `chore/agent-docs-improvements`), four logical commits. Rationale:

- All five actions touch only docs / agent prompts / backlog markdown — zero C++. Reviewer cognitive load is dominated by reading prose, not auditing code paths; bundling avoids four separate review-cycle ramp-ups.
- Each commit is independently reverable (clean per-action scope) so cherry-pick / partial revert remains cheap if one action proves wrong.
- Single CI cycle vs four (no build cost — pure docs — but still saves CI minutes on coverage / lint runs).
- Single applied.md commit set in one PR keeps the backlog-archive audit trail contiguous.

**Branch**: `chore/agent-docs-improvements` off `origin/develop`.

**Commit sequence**:

1. **`chore(agents): autonomous ship-loop + post-ship protocol (Actions 1+5)`** — ~25 min, clears 2 backlog entries (process.md ship-loop P2 + post-ship P3). Single commit because Action 5 is a sub-bullet under Action 1's new section.
2. **`docs(agents): security-review AI + handoff attack surface (Action 4)`** — ~30 min, clears 1 backlog entry, adds 1 new applied entry for the handoff surface.
3. **`chore(harness): worktree-spawn base investigation outcome (Action 3)`** — ~30 min, clears 1 backlog entry. Commit either patches `docs/harness/SETUP.md` (config-knob path) or appends to `external-blockers.md` (escalation path) — final commit message reflects which.
4. **`chore(backlog): tooling.md triage sweep — 27 entries (Action 2)`** — ~60 min, structural-only reorder + `## Triage log` + `## Parked` block + dedup merges.

**PR title**: `chore(agents): clear 4 P2/P3 backlog entries + tooling.md triage sweep`

**PR body**: cross-link this design doc (`docs/plans/shipped/agent-docs-improvements.md`) + list the 4 archived backlog entry titles + dedup count from the triage sweep.

**No bucket-E / build verification** — pure docs PR. CI runs lint / coverage on touched files only; no `cmake --build` invocation needed.

**Lock claim**: this plan's write set (AGENTS.md, agents/security-review.md, docs/backlog/agent-self-improvement/*, docs/harness/SETUP.md, docs/plans/shipped/agent-docs-improvements.md) — claim via `bash scripts/dev/lock-claim.sh agent-docs-improvements <write-set-file>` before the first commit; release auto-runs on PR merge per the `lock-slug:` line in the PR body.

---

## Implementation log

- **b1c6629b** · `chore(agents): autonomous ship-loop + post-ship protocol (Actions 1+5)` — Inserted `## Autonomous ship-loop default` section between § UX Pillars and § Project rules in AGENTS.md (34 lines, L72 onwards). Sub-section `### Post-ship turn-end protocol` ships in the same commit. Archived 2 backlog entries from process.md → applied.md (ship-loop P2 + post-ship P3).
- **1324cc97** · `docs(agents): security-review AI + handoff attack surface (Action 4)` — Added two bullets to `agents/security-review.md` § Smatchet attack surface (after "Image fetches", before "Known crash classes"). Frontmatter `description:` field updated to mention "AI-assistant" + "coding-harness-handoff" surfaces. Archived 1 backlog entry from process.md → applied.md (AI feature surfaces P2).
- **5be9ed33** · `chore(harness): worktree-spawn base — investigation outcome (Action 3)` — `git config --local` probe confirmed root cause is Claude Code SDK session-spawn using parent repo local HEAD as worktree base. ClaudeCodeLocalRunner path already correct per agentic-coding-handoff.md L143. Outcome: workaround documented in `docs/harness/SETUP.md` § Worktree base — known stale-HEAD pitfall (two-track: pre-session parent-on-develop, OR rebase-on-origin/develop first-move mid-session). External-blockers.md gained new entry escalating SDK base-selection upstream. Archived 1 backlog entry from process.md → applied.md (worktree bootstrap P2).
- **031ea475** · `chore(backlog): tooling.md triage sweep — 27 → 26 entries (Action 2)` — Structural reorganisation only. New `## Triage log` block at top with sweep date 2026-05-18 + counts (before 27 / dedup-merged 1 / moved-to-parked 16 / escalated 0 / after 10 P2). New `## Parked` section at bottom holds 16 P3 entries. Dedup survivor: 2026-05-17 Bucket-E tooltip-content-identity helper carries `Supersedes: 2026-05-16` line. No applied.md archive — sweep is process hygiene, not feature ship.

### Follow-up PRs — parked-items closure (2026-05-19)

The 4 items deferred under § Parked Actions all shipped as standalone follow-up PRs after PR #260 (b1c6629b–5feab6f6) merged. Each closes one parked entry from `docs/backlog/agent-self-improvement/process.md` 2026-05-18 orchestrator P3 (`Agent-docs improvements: 4 parked items deferred from PR #260`).

- **0639f3f1** *(PR #262, merged at sha 16163c29)* · `chore(backlog): track 4 parked items from PR #260 as single P3 process entry` — Bridge PR. Adds the P3 tracking entry to `process.md` so the deferred items don't lose their evidence trail. Each of the 4 items lists its concrete next-action inline.
- **820a90f9** *(PR #269, merged)* · `docs(glossary): clarify lock terminology — 4 role-distinct terms` — Closes parked item #3 (lock terminology clarification). New `## Plan locks` section in `docs/CONTEXT.md` naming the 4 role-distinct terms (plan-lock / lock-slug / lock-claim / holds-lock); mirror `## Terminology` table in `docs/plans/shipped/git-ref-plan-locks.md`. Feedback that flagged the terms as inconsistent was conflating function with naming.
- **ec6ed3cd** *(PR #270, merged)* · `docs(glossary): expand CONTEXT.md — 13 → 41 entries across 7 sections` — Closes parked item #2 (CONTEXT.md glossary expansion). Five new sections: UX Pillars (5 entries), Performance monitoring (6), Plugin architecture (5), Test taxonomy (8), Harness concepts (5). Plus the pre-existing Agentic flow (8) + Plan locks (4) = 41 total entries.
- **d3618fc1** *(PR #273, merged)* · `chore(agents): extract § Delegation to docs/agent-rules/delegation.md` — Closes parked item #1 (AGENTS.md three-way split — **scoped down** to a single-file extraction). AGENTS.md 549 → 344 lines; § Delegation lifted to `docs/agent-rules/delegation.md` (18 subsections, 236 lines). Single-file extraction chosen because ~74 external `AGENTS.md § <section>` references would break under a full 3-way split; redirect stub at AGENTS.md § Delegation lists every moved subsection so cross-links still resolve. Audit script (`scripts/dev/test-agent-contract.sh`) sub-check [7/8] updated to look in delegation.md.

Parked item #4 (vexp section → `.claude/CLAUDE.md`) remains as the only unaddressed item — already filed as external-blocker (`docs/backlog/agent-self-improvement/external-blockers.md` 2026-05-13 entry: vexp installer auto-regenerates the block inside AGENTS.md, fighting any move would just trigger re-injection on next `vexp update`). Re-evaluate once vexp upstream ships an opt-out marker. Not blocking — the cost is ~250 input tokens per Claude Code session.

**Adjacent shipped, out-of-original-plan-scope**: [PR #266](https://github.com/alexandrosk0/Smatchet/pull/266) `chore(agents): align prompt contracts with AGENTS.md output-contract table` — 4 commits closing the 9-point drift validated in `docs/plans/shipped/agent-contract-alignment.md` (5-class output contract, 24/24 agents with `## Outcome:` mandate, telemetry `_infer_outcome` tightening, new bucket-A audit rig). Owns its own design doc + § Implementation log; cross-referenced here because it landed during the same session and shares the agent-prompt write set.

**Plan status: complete.** All 5 in-scope actions shipped via PR #260; all 4 parked items closed via PRs #269 / #270 / #273; out-of-scope #4 vexp move remains external-blocker. No follow-up planned for this design doc.

**Verification** (run 2026-05-18):

- ✅ V1 `grep "Autonomous ship-loop default" AGENTS.md` → 1 hit
- ✅ V2 `grep -E "\*\*(Manual verify|Review PR|Squash-merge|Done)\*\*" AGENTS.md` → 4 hits
- ✅ V3 `grep -E "AiClient|AiAssistant|AiSseParser" agents/security-review.md` → ≥1 hit
- ✅ V4 `grep "Coding-harness handoff surface" agents/security-review.md` → 1 hit
- ✅ V5 `## Triage log` in tooling.md
- ✅ V6 `## Parked` section in tooling.md (L84)
- ✅ V7 applied.md gained 4 new 2026-05-18 entries (ship-loop, post-ship, AI surface, worktree-bootstrap)
- ✅ V8 external-blockers.md gained 1 new entry (Claude Code SDK worktree base)
- ✅ V9 docs/harness/SETUP.md § Worktree base — known stale-HEAD pitfall present
- ✅ V10 process.md no longer contains the 4 archived ship-loop/post-ship/security-AI/worktree entries

**Mid-flight recovery**: orchestrator initially wrote Action 1+5 edits to main-repo paths (`C:\Dev\Smatchet\...`) instead of worktree paths (`C:\Dev\Smatchet\.claude\worktrees\xenodochial-montalcini-4b9116\...`) — the exact wrong-worktree footgun documented in process.md 2026-05-16 test-rig P2. Edits stashed safely on main (`stash@{0}: agent-docs-improvements Action 1+5 wrong-worktree recovery`) and replayed against worktree paths. No data lost; cost ~5 min recovery + one stash to drop after PR merges.

## Deviations from plan

- **Action 3 outcome ≠ config-knob patch**: plan envisioned either patching a Claude Code SDK config knob OR escalating to external-blocker. Investigation found NO patchable config knob — `extensions.worktreeconfig=true` is enabled but doesn't change base-selection behaviour (only enables per-worktree config files). Branch tracking (`branch.<claude-id>.merge=refs/heads/develop`) is set post-spawn. Outcome: escalation path taken. SETUP.md gained the workaround; external-blockers.md gained the upstream-owner entry.
- **Action 4 expanded ahead of plan**: plan called out the expansion to two bullets (AI + Handoff) during the double-check pass; landed as-planned. No drift.
- **Triage sweep count corrected**: plan estimated 27 → 26 after dedup. Final count exactly 26. No additional dedup candidates surfaced during the sweep.
- **No build / ctest run**: per the revised plan, zero C++ touch across all 5 actions; build verification dropped. Confirmed in CI by per-file lint hooks only.

## Verification

See **Verification** block under § Implementation log above. All 10 checks pass as of 2026-05-18.
