- 2026-08-07 · claude-code · [tooling] · P2 — repo paths written as inline code spans in backlog entries are never checked, so a backlog entry can cite a file that does not exist

  [`agents/scripts/core/test-markdown-links.sh`](../../../../agents/scripts/core/test-markdown-links.sh)
  resolves markdown **links** — its `LINK_RE` matches only the bracketed-label-then-parenthesised-
  href form. A repo path written as a bare code span
  — `` `scripts/dev/test-ui-window-expand.sh` `` — is invisible to it. In this session I wrote a
  [tooling] entry whose entire proposal was anchored on such a path, and the file does not exist
  on `develop` (it lives only on a feature branch). The docs gate went green. A `code-review`
  pass caught it as a Critical; nothing mechanical would have.

  This matters more in `docs/self-improvement/categories/**` than elsewhere: a backlog entry is
  read months later by someone who will act on it, and its whole value is that the cited
  file:line is real. A stale entry there wastes the reader's time in exactly the way the backlog
  exists to avoid.

  Proposed: extend the markdown-link checker (or add a sibling) with a **WARN-first** rule scoped
  to `docs/self-improvement/categories/**` — for each inline code span that looks like a repo path
  (leading `scripts/`, `Source/`, `docs/`, `agents/`, `tests/`, `tools/` plus a file extension),
  assert it resolves — **at `HEAD` first, falling back to `origin/develop`**. Checking only
  `origin/develop` would false-warn on every path added by the same PR that adds the entry, which
  is the common case; checking only `HEAD` would miss the failure this entry exists for. WARN-first
  because a *deliberate* reference to a path on some other unmerged branch is legitimate; that
  entry should then carry the "not on `develop` yet" caveat in prose, which is exactly the review
  the warning prompts.

  Same delta-gate shape as the other doc gates: only newly-added or modified lines, so the whole
  existing backlog does not have to be clean on day one.

  The blindness cuts both ways, and this entry tripped the other edge while being written: prose
  quoting the *shape* of a markdown link inside a code span is read by the checker as a real link
  and reported as a dangling one. So the same fix — teach the tokenizer about inline code spans —
  removes a false negative (paths in spans never checked) and a false positive (link-shaped spans
  checked as if they were links). Until then, a doc that needs to discuss link syntax has to
  describe it in words, which is why the sentence above does.
