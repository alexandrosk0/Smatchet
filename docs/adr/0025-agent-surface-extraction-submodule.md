# Agent surface extracted to a public layer repo, consumed as a submodule — entries stay host-side

# Status

**Accepted (2026-08-29).** Locked by the `grill-with-docs` pass on [`../plans/agent-surface-extraction-repo.md`](../plans/agent-surface-extraction-repo.md) (six decisions, recorded in that plan's § Context — Grill outcomes). Implementation is phased; until Phase C lands, the surface still lives in this repo.

# Context

The agentic surface (agents, agent-rules docs, harness adapters, self-improvement framework, generic gate scripts, `AGENTS.md`'s structure) was designed portable from the start — [`../STRUCTURE.md`](../STRUCTURE.md) § The boundary and [`../PORTABILITY.md`](../PORTABILITY.md) define the seam, and `test-portable-purity` guards it. But "portable" today still means "copy + adapt out of Smatchet's tree": the layer has no independent existence, its ~500 commits of history are interleaved with product history, and a second project cannot track upstream improvements without manual diffing. Meanwhile the layer's own churn (agents, rules, gates) competes for review bandwidth with product PRs in one repo.

# Decision

The full agent surface moves to its own **public** repository, **`the-unwilling-agentic-bunch`** ("The Unwilling Agentic Bunch"), with history preserved via `git filter-repo`. Smatchet consumes it as a **git submodule mounted at `agent-layer/`**, referenced by a **relative, tokenless `.gitmodules` URL** (same GitHub owner). Layer changes reach Smatchet only through **pointer-bump PRs**, opened automatically per layer merge (`auto-bump.yml`), gated by a host-side integration lane (`agent-layer-integration.yml`) so a bump can never land unexercised.

Three boundary calls shaped the split:

- **Self-improvement *entries* stay host-side** (categories, postmortems, applied ledger, `.jsonl` ledgers) — a reversal of the original full-surface scope. They are project-specific per the STRUCTURE/PORTABILITY boundary, they are the hottest write path (525 commits since June vs ~500 for the whole rest of the surface — routing them through two-repo pointer bumps would dominate the friction budget), and a submodule working tree is not a safe write target (writes under `agent-layer/` are silently discarded by the next `submodule update`). Only the *framework* (`AGENT_SELF_IMPROVEMENT.md` + the category-structure spec) moves.
- **A dual-root config seam** replaces the single repo root: `PROJECT_ROOT` (host content — plans, backlog, source) vs `AGENT_LAYER_ROOT` (layer content), both exported by `project-config.sh`, with `project.config.json` resolved env-var → superproject root → layer's own root. The layer ships its own real `project.config.json` and is its own first consumer, so standalone layer CI is just `PROJECT_ROOT=$AGENT_LAYER_ROOT`.
- **The bats suite partitions cleanly**: the 61 of 94 suites referencing `agents/` move with the layer; 33 stay. No shared helpers cross the cut.

Considered and rejected: **git subtree** (keeps one-repo ergonomics but merges histories back together and makes upstreaming from a second host notoriously error-prone) and **copy-per-project** (the status quo PORTABILITY.md describes — no shared history, no upstream tracking, divergence guaranteed). The submodule's known ergonomic cost (the extra `submodule update`, the pointer-bump indirection) is paid deliberately in exchange for a hard boundary, independent CI/review, and a single upstream every host tracks.

# Consequences

- Layer development gains its own repo, CI, and review lane; host review bandwidth is no longer shared with agent-surface churn.
- Every layer change reaching Smatchet is now a two-step (layer PR → bump PR). Mitigated by per-merge auto-bump plus the entries-stay split, which removes the highest-frequency writes from the two-step path entirely; `git-janitor` backstops missed bumps.
- Cross-repo automation needs a token with write access to Smatchet (auto-bump's PR creation) — a user-provisioned secret, and a new operational dependency.
- Public visibility means the layer's content is world-readable — acceptable because the boundary already excludes project-specific content, and it is what makes the `.gitmodules` URL tokenless for every clone.
- The CR/Bugbot self-improvement auto-exemption and the `applied.md` merge=union attribute become permanently host-side artifacts (their subjects never move).
