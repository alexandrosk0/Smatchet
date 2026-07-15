---
name: coderabbit-handoff
description: The deterministic mechanics of a CodeRabbit-triage round — the gh api fetch commands, bot body-shape parse markers, the triage-table example, the per-finding `### #N` handoff-packet skeleton (VALID + REJECTED worked examples), the reply-to-bot lines, and the watcher-invocation dispatch steps. Invoked by coderabbit-triage (which keeps the judgment: the 19-rule override table, path→agent routing, severity classification, hand-back contract). Use when fetching bot artefacts, formatting a CodeRabbit triage report or handoff packet, or running the merge-watcher dispatch shape.
triggers:
  - coderabbit-handoff
  - triage table
  - handoff packet
  - triage report
  - reply to bot
version: 1
---

<!-- Skill-only helper (no agent twin; registered in SKILL_ONLY_HELPERS). This is the
     EXTRACTED report-shape body of agents/core/coderabbit-triage.md — the deterministic
     triage-table + per-finding handoff-packet templates. The agent keeps the reasoning
     (override table, routing, severity, watcher mode, hand-back) and points here for the
     report shape. reduce-agent-prompt-bloat. Cross-harness: Codex/Cursor read the agent's
     summary + this path; Claude Code loads this skill on demand.
     The agent's REAL `## Outcome:` / `## Session context append` / `## Self-improvement`
     headings live in the agent, NOT here — keep them un-fenced there so the agent contract
     check stays green. -->

# coderabbit-handoff (skill)

The mechanics `coderabbit-triage` runs. The agent owns *what each finding is and where it routes* (the 19-rule override table, the path→agent routing table, severity classification, the hand-back contract); this skill owns the deterministic *how* — the fetch commands, the body-shape parse markers, the triage table, the per-finding handoff packets, the reply-to-bot lines, and the watcher dispatch steps.

## Fetch commands

Fetch every bot artefact in parallel (capture stdout, never pipe to a render thread). `--slurp` makes `--paginate` return one valid JSON array instead of a concatenated stream of arrays:

```bash
PR=<num>; OWNER_REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner)
gh api "repos/$OWNER_REPO/pulls/$PR/reviews"   --paginate --slurp > .triage-reviews.json
gh api "repos/$OWNER_REPO/pulls/$PR/comments"  --paginate --slurp > .triage-review-comments.json
gh api "repos/$OWNER_REPO/issues/$PR/comments" --paginate --slurp > .triage-issue-comments.json
```

If the installed `gh` predates `--slurp` (added in gh 2.40), fall back to `--paginate --jq '.[]' | jq -s '.'`.

Bot body-shape parse markers (the severity *mapping* judgment stays in the agent): CodeRabbit tags severity with emoji (🛠️ actionable · ⚠️ caution · 💡 nit · 🧹 chore — read literally) and posts `_Actionable comments posted: N_` / `_Nitpick comments (N)_` headers. Cursor Bugbot opens each inline finding with a `### <title>` heading, then a `**<Sev> Severity**` line (e.g. `**Medium Severity**`), then a `<!-- DESCRIPTION START -->` marker; its summary review carries `<!-- BUGBOT_REVIEW -->` + "found N potential issues" + `<!-- BUGBOT_FIX_ALL -->`.

## Watcher-invocation dispatch

The 6-step shape when `smatchet-merge-watcher` (per `docs/plans/shipped/smatchet-merge-watcher.md` Phase 3) invokes the triage agent:

1. **Watcher** spawns `claude -p AUTO_ACT_PROMPT` (literal text: `agents/scripts/core/merge-watcher.py:AUTO_ACT_PROMPT`). Spawn is gated by `MERGE_WATCH_AUTO_ACT=true` + per-PR / per-head-sha budget; defaults are off to prevent runaway-loop risk.
2. **Spawned session** invokes `coderabbit-triage` first. The watcher passes only PR metadata via the prompt string (`pr`, `owner`, `repo`, `head_sha`, `budget`, `attempt` — see `merge-watcher.py:AUTO_ACT_PROMPT.format(...)`); the agent fetches the CR review body + inline review-thread comments itself via `gh api` (§ Fetch commands above), runs the 19-rule override table + validation pass, and emits per-finding handoff packets — VALID (with target subsystem named) + REJECT-INVARIANT / REJECT-AMBIGUOUS with rationale.
3. **Spawned session** routes each VALID packet to its target subsystem agent (`tracker-backend`, `grid-engine`, `mechanic`, etc.) for the actual edits. REJECT findings are skipped outright; rationale surfaces in the commit body.
4. **Spawned session** commits + pushes once all subsystem dispatches complete.
5. **Watcher** re-polls on the next cycle; CR re-reviews the new head; the C4 (prongs 1 + 2) gate logic decides whether to merge.
6. **Watcher (default-on as of 2026-05-28; set `MERGE_WATCH_RESOLVE_CR_THREADS=false` to opt out)** — once the new head's poll shows CR is no longer block-shaped, `maybe_resolve_stuck_cr_threads` enumerates the CR-authored, non-outdated, unresolved review threads on the PR and calls GraphQL `mutation resolveReviewThread` per thread. Closes the gap where CR's per-line threads stay `isResolved:false` on prior commits even after CR's overall review is SUCCESS, which used to keep `cr_open > 0` and wedge the merge gate. See `docs/plans/shipped/merge-watcher-triage-recovery.md` § sub-bug (b).

C4 prong 3 (per `docs/reference/agentic-infrastructure-2026-05-23.md`) is what wires this multi-step dispatch into `AUTO_ACT_PROMPT`.

## Output format

```text
## Triage table
| # | file:line | severity | applies? | target | reason / rule |
|---|-----------|----------|----------|--------|---------------|
| 1 | Source/Core/src/Foo.cpp:123 | High | yes | tracker-backend | Catalog→parser bypass; route fix |
| 2 | Source/Core/include/Bar.h:42 | Medium | no (override #1) | — | Suggestion used `std::optional`; C++14 hard |
| 3 | Source/Plugins/Mcp/McpServer.cpp:88 | Low | superseded | — | Code rewritten in commit abc1234 |
...

## Findings

### #1 — High · `Source/Core/src/Foo.cpp:123` → tracker-backend
**CodeRabbit body (verbatim, trimmed):**
> <quoted summary, ≤ 4 lines>

**Validation:** confirmed live — `Foo::Save` still calls `cpr::Post` directly at line 127 instead of through `TrackerHttpClient`.

**Handoff packet** (paste into orchestrator → `tracker-backend` prompt):
- **Scope**: replace direct `cpr::Post` in `Foo::Save` with `TrackerHttpClient::Post` posted to the existing worker thread; wire result back via `MainThreadDispatcher`.
- **Allowed write set**: `Source/Core/src/Foo.cpp`, `Source/Core/include/Foo.h`.
- **Out of scope**: any other tracker file. Do NOT touch the shared `ITracker*.h` interface headers.
- **Invariant pre-decisions**: HTTP-through-TrackerHttpClient (override rule #7 — confirmed live, not rejected); UI-thread non-blocking (pillar 2).
- **Verification**: existing tests in `tests/Core/TrackerHttpClientPure.test.cpp` cover the call shape — no new test required. Manual: none.
- **Reply to bot** (orchestrator may post once fix lands): `Addressed in <sha>; routed through TrackerHttpClient as suggested.`

### #2 — Medium · `Source/Core/include/Bar.h:42` → REJECTED (override #1)
**CodeRabbit body:** suggests `std::optional<Bar>` for the return type.
**Reason:** C++14 hard (AGENTS.md § Project rules). The current `Bar*` + nullable contract is correct.
**Reply to bot** (orchestrator may post): `Not applicable — this repo is C++14-hard; `std::optional` is banned. The nullable-pointer return is intentional.`

...
```

The `## Outcome:`, `## Session context append`, and `## Self-improvement` sections are REAL agent headings (in `coderabbit-triage.md`, not here) — the agent emits them un-fenced after the findings.
