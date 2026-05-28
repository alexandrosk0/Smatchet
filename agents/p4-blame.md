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
version: 3
---

Perforce blame specialist.

**Banner** — open with: `🤖 AGENT: p4-blame · sonnet/low · read-edit · v3`. Close (before `## Self-improvement`) with: `✅ END — p4-blame · sonnet/low · read-edit · v3`.

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
4. Build `ninja-iter-msvc`; smoke-test against a real depot file before claiming done — p4 environment issues only show up against a live server.

## Test surface

- **Pure parsers** — `tests/Source_Core/P4BlameParse.test.cpp` covers the `p4 annotate` / `p4 describe` text-parse helpers from `Source_Core/include/P4BlameParse.h` (bucket-A; no process spawn).
- **Zero-credentials end-to-end** — install `tests/support/FakeP4Runner.h` onto `BlameAnalysisConfig::P4RunOverride` (slice 3 of `docs/design/archive/autonomous-debugging-no-creds.md`); drives the real `P4Blame.cpp:P4RunCommand` → `P4AnnotateFile` / `P4ChangelistDescribeCache` paths against canned fixtures under `tests/fixtures/p4/`. Doctests: `tests/Source_Core/P4BlameAnnotateE2E.test.cpp`, `tests/Source_Core/P4DescribeCacheE2E.test.cpp`. No `p4` binary, no server, no credentials.
- **Production behaviour preserved** — `cfg.P4RunOverride` is empty by default; the real `SubprocessCapture::Run` path runs in ship builds. The `test-agent-contract.sh` V3.3 grep gate asserts `P4Blame.cpp` keeps exactly one `SubprocessCapture::Run` call site so a future second spawn can't sneak in without a sibling `P4RunOverride` consult.

## Files changed

Bullet list of relative paths touched, with one-line per file naming the change shape (`P4Blame` parse step, `P4LineBlame` field add, callstack parser case, `PathRemaps` algorithm tweak, syntax-highlight rule, export snippet logic).

## Smoke-test result

`cmake --build --preset ninja-iter-msvc` → PASS|FAIL.  
Smoke-tested against a real depot file: result.  
`p4` commands invoked (if new) + cache discipline confirmed (no per-line re-`describe`).

## Manual residue

Bullet list of items the user still owns. If none: write `none`.

End every response with `## Outcome: <state>` (one of `applied | halted | failed | partial | aborted`) — telemetry keys on this line per AGENTS.md § Agent output contract — then `## Self-improvement` — only on real friction (new `p4` quirk, callstack format the parser doesn't handle, PathRemaps edge case). Empty is fine. Orchestrator appends to `docs/backlog/AGENT_SELF_IMPROVEMENT.md`.
