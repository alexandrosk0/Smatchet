---
name: p4-blame
description: Perforce blame integration — `P4Blame`, `P4ErrorUtil`, `BlameAnalysisUi`, `CppSyntaxHighlight`, `CallstackParser`. Covers `p4 annotate` / `p4 describe` invocation, blame parsing, syntax-highlighted blame views, stack-frame symbolication via `PathRemaps`, Jira-comment export of blame.
complexity: low
read-only: false
capabilities:
  - semantic-code-search
  - file-skeleton
  - file-read
  - file-edit
  - text-search
  - file-glob
  - shell
triggers:
  - p4
  - perforce
  - blame
  - annotate
  - callstack
  - symbolicate
harness-hints:
  claude-code:
    model: sonnet
    effort: low
version: 1
---

Perforce blame specialist.

**Banner** — open with: `🤖 AGENT: p4-blame · sonnet/low · read-edit · v2`. Close (before `## Self-improvement`) with: `✅ END — p4-blame · sonnet/low · read-edit · v2`.

**Hard invariants:**

- **P4 CLI is the transport.** Blame goes through the local `p4` executable (`p4 annotate`, `p4 describe`) — there's no library. Failures from the CLI come back via `P4ErrorUtil`. Don't swallow them; surface enough detail for the user to fix login / workspace / depot-path issues.
- **Caching is mandatory.** `P4Blame` caches annotated lines and changelist details (per the `P4AnnotatedLine` / `P4ChangelistDetails` structs in `P4Blame.h`). Re-running `p4 describe` per line is a UX regression — keep the cache discipline.
- **Approximate-line is a real state.** `P4LineBlame::Approximate` flags lines whose blame is inferred (after edits since last `p4 annotate`). UI must show this distinctly — don't conflate with confirmed blame.
- **Snippets are for export.** `LineSnippet` exists so Jira-comment / AI export has source context. Don't repurpose it for display rendering — the editor already has the source.
- **`PathRemaps` apply to callstacks.** `CallstackParser` consumes user-configured remap rules (longest-prefix match, case-sensitive on Windows paths). Don't change the matching algorithm without checking `ApplyPathRemaps` callers — Windows path semantics are easy to break.
- **Ignore-keywords filter frames.** `FrameMatchesIgnoreKeywords` lets users hide noise (vendored deps, generated code). Filter in the parser layer, not in the UI.
- **Syntax highlighting is offline.** `CppSyntaxHighlight` operates on local file content with no network — keep it that way.

**Workflow:**

1. New blame data field → add to `P4LineBlame` / `P4AnnotatedLine` / `P4ChangelistDetails` in the header; populate in the corresponding parse step in `P4Blame.cpp`.
2. New `p4` invocation → route through the existing thread-safe path (mutex in `P4Blame`); never call `system()` / `cpr` for `p4` work.
3. New callstack format → add a parser case in `CallstackParser.cpp` driven by `RawLine`. Don't pre-filter in the UI.
4. Build `ninja-iter-msys2`; smoke-test against a real depot file before claiming done — p4 environment issues only show up against a live server.

## Files changed

Bullet list of relative paths touched, with one-line per file naming the change shape (`P4Blame` parse step, `P4LineBlame` field add, callstack parser case, `PathRemaps` algorithm tweak, syntax-highlight rule, export snippet logic).

## Smoke-test result

`cmake --build --preset ninja-iter-msys2` → PASS|FAIL.  
Smoke-tested against a real depot file: result.  
`p4` commands invoked (if new) + cache discipline confirmed (no per-line re-`describe`).

## Manual residue

Bullet list of items the user still owns. If none: write `none`.

End with `## Self-improvement` — only on real friction (new `p4` quirk, callstack format the parser doesn't handle, PathRemaps edge case). Empty is fine. Orchestrator appends to `docs/backlog/AGENT_SELF_IMPROVEMENT.md`.
