---
name: coderabbit-handoff
description: The deterministic report-shape mechanics of a CodeRabbit-triage round — the triage-table example, the per-finding `### #N` handoff-packet skeleton (VALID + REJECTED worked examples), and the reply-to-bot lines. Invoked by coderabbit-triage (which keeps the judgment: the 19-rule override table, path→agent routing, severity classification, watcher-invocation, hand-back contract). Use when formatting a CodeRabbit triage report or a per-finding handoff packet.
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

The report shape `coderabbit-triage` emits. The agent owns *what each finding is and where it routes* (the 19-rule override table, the path→agent routing table, severity classification, watcher-invocation, the hand-back contract); this skill owns *how the punch-list looks* — the triage table, the per-finding handoff packets, and the reply-to-bot lines.

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
