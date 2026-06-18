- 2026-06-18 · orchestrator · [process] · P3 — new backlog entries land as one file per entry
  Details: Self-improvement entries now go in their own file under
    docs/self-improvement/categories/<category>/<YYYY-MM-DD>-<slug>.md instead of
    being appended to the monolithic categories/<category>.md. Two concurrent PRs
    that each add an entry then touch disjoint paths, so adds never merge-conflict;
    archiving an entry is removing/moving that one file, so deletes never conflict
    either. The ~135 legacy entries stay in the monolith files and are still read
    in union by every reader (the count gate and the triggered-follow-up nudge glob
    both sources). This is the incremental, new-entries-only slice of the deferred
    self-improvement-one-entry-per-file plan; the 135-entry migration is not done.
  Concrete next action: none — this entry exists to exercise the new per-entry
    path end-to-end and document the switchover. Producers: write per-entry files
    from now on per docs/self-improvement/AGENT_SELF_IMPROVEMENT.md § Workflow.
  Status: observational
  Last-reviewed: 2026-06-18
