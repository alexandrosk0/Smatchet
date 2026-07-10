# Committing via the Bash tool needs a heredoc, not the PowerShell here-string template

- **Date**: 2026-07-10 · **Priority**: P3 · **Category**: process
- **Session**: issue-fixing thread (#1713, PR #1726)

## Friction

The environment's commit-message guidance is written for the PowerShell tool
(`git commit -m @'…'@` single-quoted here-string, with the mandatory
`Co-Authored-By:` / `Claude-Session:` footer). On this repo the ship-loop
commits through the **Bash** tool instead — `git -C <literal-abs-path> commit`
is the standard form for worktrees, because the integration tree rejects
`$VAR`/`$(pwd)` in the commit path. In git-bash, `@'…'@` is not a here-string:
`@'` parses as a literal `@` followed by a single-quoted block, so the message
became `@\n<real subject>\n…` and the commit subject was a bare `@`. Caught it
on the `git log -1 --format=%s` readback and had to `--amend -F <file>`, costing
an extra amend round-trip.

## Proposal

When committing from the **Bash** tool, never paste the PowerShell `@'…'@`
template verbatim. Use one of:
- `git commit -F <file>` after writing the message to a temp file (most robust
  for multi-line bodies + the footer), or
- a bash heredoc: `git commit -F - <<'EOF' … EOF`.

Reserve `-m @'…'@` for the PowerShell tool only. Consider adding a one-line note
to the ship-loop commit step in `docs/agent-rules/process-rules.md` (or the
worktree commit recipe) that the `@'…'@` form is PowerShell-only and the Bash
path uses `-F`. Cheap, prevents a silent malformed-subject commit that only the
`%s` readback catches.
