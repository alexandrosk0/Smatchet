# `seed-audit.md` — per-path publication verdict

> Generated companion to [`seed-paths.txt`](seed-paths.txt). `seed-agent-layer-repo.sh`
> phase 4b asserts that **every** manifest pathspec appears here with a verdict that is
> not `PENDING`, and refuses the phase-5 push otherwise.

The seed is an **allowlist, not a subtraction**: nothing reaches the public repo that
this table has not cleared. Clearing a path means reading **both** the working-tree file
**and its history** (`git log -p --follow <path>`) for Smatchet-internal identifiers,
internal hostnames, ticket URLs, customer/user data and credentials. A scrubbed head over
a leaky history still publishes the leak, which is why the head-only read is not enough
and why the history-wide secret scan in phase 4b runs *in addition to* this table rather
than instead of it.

## Verdicts

| Verdict | Meaning |
|---|---|
| `CLEAR` | Head **and** history read; nothing project-internal, nothing secret. Publishes as-is. |
| `SCRUB` | Publishes only after a paired `git filter-repo --invert-paths` / `--replace-text` pass. Add the path to `seed-scrub-paths.txt`; phase 3 applies it. |
| `EXCLUDE` | Never publishes. Must not appear in `seed-paths.txt` at all — an `EXCLUDE` row here is a **design note**, recording a decision so it is not re-proposed. |
| `PENDING` | Not yet audited. **Blocks the push** — phase 4b treats `PENDING` as no verdict. |

## Decisions already taken (not rows in the manifest)

These paths are deliberately **absent** from `seed-paths.txt`. Recorded so the omission
reads as a decision rather than an oversight:

| Path | Verdict | Why |
|---|---|---|
| `agents/project/` | `EXCLUDE` | Project-scoped by construction. This is the reason the manifest enumerates four explicit `agents/*` subtrees instead of one bare `agents/` prefix. |
| `agents/project/workflows/historical-review-sweep.js` | `EXCLUDE` | Self-identifies as non-portable. Already excluded wholesale by the `agents/project/` decision above, so it needs **no** paired scrub pass — the plan's row-8 text predates its move out of `agents/scripts/core/`. |
| `agents/README.md` | `EXCLUDE` | Sits directly under `agents/`, inside no allowed subtree, and its prose is host-specific ("every Smatchet subagent"). The layer needs its own. |
| `docs/self-improvement/categories/`, `postmortems.md`, `applied.md`, `*.jsonl` | `EXCLUDE` | Entries stay host-side (grill decision 4 / ADR-0025). Only the framework spec `AGENT_SELF_IMPROVEMENT.md` moves. |
| `docs/plans/`, `docs/work/`, `Source/`, `CMakeLists.txt` | `EXCLUDE` | Host content. `seed-agent-layer-repo.sh` phase 2 hard-fails if any of these ever appears in the manifest. |

## Manifest rows

Reviewer + date are filled **when the row is cleared**, by the person or agent who read
the file and its history. Do not pre-fill them.

| # | Path | Verdict | Reviewer | Date | Notes |
|---|---|---|---|---|---|
| 1 | `agents/core/` | `PENDING` | | | Portable agent prompts. Head is portable by design; history predates the portability split, so the read is not a formality. |
| 2 | `agents/_shared/` | `PENDING` | | | Shared skills + scripts. `portable-purity-baseline.txt` already lists known host-literal residue in this subtree — that residue is a de-Smatchet-ification follow-up, NOT a publication blocker, but confirm it is prose-only. |
| 3 | `agents/scripts/core/` | `PENDING` | | | The gate scripts. Highest-risk subtree for hardcoded org/repo names and tokens in history. |
| 4 | `agents/scripts/project/` | `PENDING` | | | Project-named but portable-by-contract. Confirm the name is the only project-specific thing about it. |
| 5 | `docs/agent-rules/` | `PENDING` | | | Rule-docs. Known host-literal residue per the purity baseline. |
| 6 | `docs/harness/` | `PENDING` | | | Harness adapters, including hook scripts. Check hooks for absolute local paths. |
| 7 | `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` | `PENDING` | | | Framework spec only. Confirm it carries no entry text. |
| 8 | `docs/high-integrity/portable-purity-baseline.txt` | `PENDING` | | | Moves so the layer can re-baseline against layer-relative paths (plan row 16). |
| 9 | `docs/high-integrity/agent-size-baseline.md` | `PENDING` | | | Agent-size grandfather list; same rationale as the purity baseline. |
| 10 | `AGENTS.md` | `PENDING` | | | Root rulebook. Prefix match takes the root file ONLY — phase 4b asserts `git ls-files '*AGENTS.md'` == `AGENTS.md`. |
| 11 | `scripts/dev/project-config.sh` | `PENDING` | | | Canonical copy. Host keeps a byte-identical mirror under the row-8g `cmp -s` drift gate. |
| 12 | `scripts/dev/test-all.sh` | `PENDING` | | | Lane runner; sanctioned mirror (row 8g / `mirrored-paths.txt`). |
| 13 | `scripts/dev/test-docs.sh` | `PENDING` | | | Lane runner; sanctioned mirror (row 8g / `mirrored-paths.txt`). |
| 14 | `tests/bats/agent_size.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 15 | `tests/bats/android_openssl_failfast.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 16 | `tests/bats/archive_backlog_entry.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 17 | `tests/bats/capture_intent.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 18 | `tests/bats/coderabbit_triage.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 19 | `tests/bats/cr_oob_review_backfill.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 20 | `tests/bats/dead_export_audit.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 21 | `tests/bats/dup_audit.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 22 | `tests/bats/fail_open_authoring.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 23 | `tests/bats/fleet_preflight.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 24 | `tests/bats/fleet_rescope.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 25 | `tests/bats/followup_due_nudge.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 26 | `tests/bats/function_size.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 27 | `tests/bats/fuzz_closure_audit.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 28 | `tests/bats/gate_selftests.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 29 | `tests/bats/git_janitor.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 30 | `tests/bats/harness_provisioned_doctor.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 31 | `tests/bats/historical_review_ledger_reconcile.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 32 | `tests/bats/historical_review_survivors.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 33 | `tests/bats/include_cycle_audit.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 34 | `tests/bats/is_pure_docs_diff.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 35 | `tests/bats/issue_sweep.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 36 | `tests/bats/lint_rules.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 37 | `tests/bats/lock_claim.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 38 | `tests/bats/lock_staleness_sweep.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 39 | `tests/bats/markdown_links.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 40 | `tests/bats/merge_gates.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 41 | `tests/bats/merge_watcher.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 42 | `tests/bats/merge_watcher_integration.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 43 | `tests/bats/migrate_bugs_to_issues.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 44 | `tests/bats/oob_label_impl.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 45 | `tests/bats/panel_verdicts.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 46 | `tests/bats/plan_archival_owed.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 47 | `tests/bats/plan_index_robustness.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 48 | `tests/bats/plan_lock_gate.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 49 | `tests/bats/postmortem_owed.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 50 | `tests/bats/pre_push_guard.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 51 | `tests/bats/preship_review_artifact.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 52 | `tests/bats/repo_health_facts_nudge.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 53 | `tests/bats/required_context_adr_consistency.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 54 | `tests/bats/required_context_parity.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 55 | `tests/bats/resolve_py.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 56 | `tests/bats/resolve_repo.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 57 | `tests/bats/review_ack_gate.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 58 | `tests/bats/review_guard.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 59 | `tests/bats/run_review.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 60 | `tests/bats/safe_admin_merge.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 61 | `tests/bats/safe_merge.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 62 | `tests/bats/script_freshness.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 63 | `tests/bats/session_registry.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 64 | `tests/bats/setup_branch_protection.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 65 | `tests/bats/shell_lint.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 66 | `tests/bats/small_helper_audit.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 67 | `tests/bats/subsystem_docs.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 68 | `tests/bats/sync_issue_labels.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 69 | `tests/bats/unwatched_pr_nudge.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 70 | `tests/bats/verifier_preship_wiring.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 71 | `tests/bats/verifier_review_gate.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 72 | `tests/bats/work_item_owed.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 73 | `tests/bats/workflow_job_mask.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 74 | `tests/bats/workflow_watchdog.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |
| 75 | `tests/bats/worktree_prune.bats` | `PENDING` | | | Layer-coupled bats suite (generated block). Check fixtures for real hostnames, tokens or ticket IDs. |

## Status

**0 of 75 rows cleared.** The push is blocked until every row carries a non-`PENDING`
verdict. This table was generated with all rows `PENDING` — the per-path read is the
remaining human/agent work before a real seed, and it is deliberately **not** something
the generator guesses on the reviewer's behalf.

Regenerate the row skeleton (preserves nothing — re-fill verdicts afterwards) only if the
manifest changes shape; normally you edit this file in place.
