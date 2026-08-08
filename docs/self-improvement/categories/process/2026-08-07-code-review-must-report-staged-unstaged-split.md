- 2026-08-07 · claude-code · [process] · P2 — `code-review` told "review the staged diff" silently skips working-tree hunks on `MM` paths; it must report the split before reviewing

  Concrete miss: the worktree A review of [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966)
  was scoped to `git diff --cached`, but `Source/Core/src/Ui/SmatchetWindowExpand.cpp` was
  `MM` — the most substantive hunk under discussion (the `PushStyleColor` array/loop refactor
  the requester explicitly asked to have its push/pop balance verified) existed **only in the
  working tree**. The agent caught it, but only because it happened to run `git status`; a
  reviewer that goes straight to `git diff --cached` reviews code the requester is not
  looking at, and reports green on a file whose real content it never read.

  The requester's mental model of "what I'm about to commit" is wrong precisely on `MM`
  files — that is what `MM` means.

  Fix, in [`agents/core/code-review.md`](../../../../agents/core/code-review.md): make step 1
  run `git status --short` and, for **any** `MM` path in scope, state the staged/unstaged
  split up front and ask which one is under review (default: review the **working tree**,
  since that is what will be built and tested). Cheap — one command — and it converts a
  silent scope hole into an explicit question.
