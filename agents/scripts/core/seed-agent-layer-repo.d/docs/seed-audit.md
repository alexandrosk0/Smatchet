# `seed-audit.md` — per-path publication verdict

> Companion to [`seed-paths.txt`](seed-paths.txt). `seed-agent-layer-repo.sh` phase 4b refuses the
> phase-5 push unless **every** manifest pathspec has **exactly one** row in § Manifest rows whose
> verdict is `CLEAR`, or `SCRUB` that is both listed in `seed-scrub-paths.txt` and gone from the
> rewrite. Phase 3 refuses to rewrite at all if any commit touched a manifest path after the pin below.

**Audited through:** `2639882a2df15262e3bff646dd32eedfbe0953de` — every commit reachable from this develop commit that touches a
manifest path. Anything later is unaudited until re-swept (`seed-audit-sweep.py --since <pin>`).

The seed is an **allowlist, not a subtraction**: nothing reaches the public repo that this table has not
cleared. Clearing a path means checking **both** the working-tree file **and its history** for
Smatchet-internal identifiers, internal hostnames, ticket URLs, customer/user data, credentials and
third-party content that cannot be re-published. A scrubbed head over a leaky history still publishes
the leak.

## Verdicts

| Verdict | Meaning |
|---|---|
| `CLEAR` | Head **and** history checked; nothing project-internal, nothing secret, nothing un-republishable. Publishes as-is. |
| `SCRUB` | Publishes only after a paired `git filter-repo --invert-paths` pass. Add the path to `seed-scrub-paths.txt`; phase 3 applies it, and phase 4b refuses unless the path is then absent from the rewritten history. |
| `EXCLUDE` | Never publishes, so it must not appear in `seed-paths.txt`. Phase 4b refuses an `EXCLUDE` verdict on a manifest row as a contradiction. Here it appears only in § Decisions already taken, as a **design note** recording a decision so it is not re-proposed. |
| `PENDING` | Not cleared. **Blocks the push.** A `PENDING` row that names an owner decision is audited but waiting on that decision. Any other verdict text, a missing row, or a duplicate row also blocks. |

## Method

What was checked, so the verdicts can be weighed rather than trusted:

1. **History sweep** — `agents/scripts/core/seed-audit-sweep.py` over exactly what filter-repo keeps:
   every added line in `git log -p --cc --full-history --no-renames HEAD -- <75 pathspecs>`, plus the
   message and identities of each of those commits. **588 commits, 98,719 added lines, 0 binaries.**
   Coverage asserted: all 436 manifest files at HEAD appear in the scanned history.
2. **Independent secret scan** — gitleaks 8.30.1 `detect` over the **entire** repository history
   (2,576 commits, 53 MB), findings then filtered to manifest paths. 16 findings total, 5 inside the
   manifest.
3. **Third-party content** — every seeded file grepped for provenance claims ("ported from",
   "verbatim", "copied from", licence/copyright markers), then an **8-word shingle overlap** scan of all
   436 seeded files against the one local upstream source found (Whip-Process), so an *unattributed*
   copy cannot slip through.
4. **Triage** — every distinct matched value in every category was read in context and classified. No
   category was cleared by count alone.

**Limits.** This is a pattern-and-overlap audit, not a line-by-line human read of 98,719 lines. It will
not catch a leak that matches no pattern — e.g. prose naming an employer or a customer in plain words.
The residual risk is bounded by one fact: **the Smatchet repository is already public**, so the seed
re-publishes a subset of already-public history and adds no new exposure. The audit's job is to stop a
live leak or an un-republishable text being *doubled* into a second, reuse-oriented repository.

**Delta audit — the row-8 correction (2026-10-04).** The first standalone simulation of the layer
(`agent-layer-sim.sh`) showed the 75-path manifest was incomplete: layer wrappers whose suites were not
seeded, and fixtures nobody had listed. **43 pathspecs were added** (5 bats suites, 38 fixture paths;
77 files), then a 44th — `tests/dev/comment_tooling/` — after review found its wrapper skipping by exit 2. Each was swept over its **full** history, not a delta, because none had been audited before:
`seed-audit-sweep.py --manifest <the 43 lines>` — **38 commits, 5,879 added lines, 0 binaries**, every
head file seen in the scanned history; gitleaks over `--all` history for the same paths — **0
findings**; the provenance grep — no third-party markers. The values triaged are in each row's notes.
The guards this change adds inside already-cleared paths are its own edits, reviewed in its diff; like
any post-pin commit they are swept by the next pin advance (§ Status).

## Findings

### Secrets — none real (two scanners agree)

Every secret-shaped string in the seeded history is a synthetic fixture in the redaction test machinery
(`agents/scripts/core/redact-intent.py`, `agents/scripts/core/redaction-escape-oracle.py`,
`tests/bats/capture_intent.bats`), each checked by hand:

Values below are **deliberately truncated** so this document does not itself match the patterns it
reports on — otherwise phase 4b's own secret scan would flag the audit table.

| Shape | Value (truncated) | Why it is not a credential |
|---|---|---|
| GitHub | `ghp_ABCDEF…`, `github_pat_11ABCDEF…` | Sequential alphabet + digits |
| AWS | `AKIAIOSFODNN7…` | AWS's own documented example key |
| Stripe | `sk_live_abcd…` | Sequential placeholder (all 5 in-manifest gitleaks findings) |
| Slack | `xoxb-12345…` | Sequential placeholder |
| OpenAI / Google | `sk-proj-abcd…`, `AIzaabcd…` | Sequential placeholders |
| JWT | `eyJhbGciOiJIUzI1NiJ9…` | Decodes to `{"alg":"HS256"}.{"sub":"1"}` |
| Private keys | RSA header + `MIIEowIBAAKCAQEA…`; OpenSSH header + `b3BlbnNzaC1rZXktdjE…` | RSA-2048 DER prefix + `abcdef`; the OpenSSH `openssh-key-v1…none…ssh-ed` preamble. No key material |
| URL credentials | `https://user:<placeholder>@…`, `postgres://admin:<template slot>@…` | Literal placeholders / template slots |

The 11 remaining gitleaks findings are all host-side (`Source/`, `tests/Core/`, `tests/fuzz/`) and are
not seeded.

**How phase 4b accepts them.** On the gitleaks path, phase 4b scans the *rewritten* history as a hard
gate, and there these fixtures surface as 25 findings (22 distinct fingerprints, two commits). The
scaffold ships a `.gitleaksignore` at the layer root that lists exactly those fingerprints, so the
gitleaks path passes on the triaged set and still fails on any gitleaks finding not listed. A rehearsal
checked both: the rewritten history scans clean with the file, and a planted token fails with it. Every
listed value matched a shape in the table above. For gitleaks, a new fixture means a new fingerprint and
a new line here, never a broader rule. The TruffleHog fallback (used only when gitleaks is not on `PATH`)
runs with `--results=verified`, so it fails only on credentials it can verify live; an unverified
finding does not fail that path. Run the seed with gitleaks installed to get the stricter gate.

### Hosts, tickets, P4 — none internal

27 distinct URL hosts, all public (`github.com`, `claude.ai`, `agents.md`, `gradle.org`,
`msdl.microsoft.com`, `api.deepseek.com`, `docs.github.com`, `git-scm.com`, …), loopback, or placeholders
(`db.internal`, a hypothetical `*.proxy.corp` in one commit message). **Zero** Atlassian tenants, Jira
hosts, private IPs or `/browse/` ticket URLs. The 31 ticket-shaped keys are ADR / PR / UTF-8 / ISO-8601 /
CWE-78 / OSV ids and the repo's own work-item codes. P4: only generic `//depot/path` examples and
`localhost:1666`; one commit message names a local workspace (`smatchet_main_alexk`).

### Third-party content

- **`agents/_shared/skills/grill-with-docs/`** — three files verbatim from `mattpocock/skills`, which is
  **MIT**. MIT requires its notice to travel with copies and none did; **resolved** by adding
  `UPSTREAM-LICENSE` beside them.
- **Whip-Process** — see **D1**. Absorbed into Smatchet in August 2026 (plan
  `docs/plans/absorb-whip-process.md`, host-side). The only copy of the source found is a local folder with
  **no licence file and no git remote**, and its README describes it as "extracted from the project that grew
  it", so neither authorship nor licence can be established from the repository. The overlap scan found
  substantial upstream text in exactly the nine files that already say so, and nothing unattributed:

  | Seeded file | Overlap | Upstream source |
  |---|---|---|
  | `docs/agent-rules/work-items.md` | 62% | `Process.md`, `Conventions.md` |
  | `agents/_shared/skills/close-work-item/SKILL.md` | 24% | `Procedures/ClosingItem.md`, `ClosingReview.md` |
  | `docs/agent-rules/review-panels.md` | 22% | `Procedures/ReviewBasics.md` |
  | `agents/scripts/core/run-review.sh` | 18% | `Tools/run-review.ps1` |
  | `agents/_shared/skills/pre-implementation-review/SKILL.md` | 18% | `Procedures/PreImplementationReview.md` |
  | `agents/scripts/core/lib/review-guard.sh` | 17% | `Tools/review-guard.ps1` |
  | `agents/scripts/core/work_item_lint.py` | 17% | `Tools/check-docs.ps1` |
  | `agents/_shared/skills/address-review-feedback/SKILL.md` | 14% | `Procedures/AddressReviewFeedback.md` |
  | `agents/_shared/skills/adversarial-code-review/SKILL.md` | 3% | `Procedures/PostImplementationReview.md` |

  Everything else seeded is at or below 0.8% — incidental shared phrasing.

### Personal data

Commit identities on the seeded history: the owner's name with their **personal e-mail address** (631
author/committer lines), `GitHub <noreply@github.com>` (531, squash-merge committer), `Claude
<noreply@anthropic.com>` (10), `claude[bot]` (4). The same personal address also appears as a
redaction fixture in `redact-intent.py` and `capture_intent.bats`. See **D2**. (This document names
the address only by description, so it does not add one more copy.) Local user paths are the
owner's `alexk` handle, only inside redaction fixtures.

## Decisions for the owner

### D1 — Whip-Process-derived content · **RESOLVED (a), 2026-09-14**

**Decision:** option (a). The owner confirmed that the Whip-Process author gave permission to use the
text under this repository's MIT licence. The author is not named here: none was provided for
publication, and naming a private person is the author's or owner's call, not the audit's. The record of
that permission is held by the owner, not by this repository.

**Applied:** the nine ported files and the two ported test suites each carry
"used with its author's permission under this repository's MIT licence" beside their existing provenance
note, so the attribution travels with every copy — in the layer and in the host alike. The five rows
below were flipped to `CLEAR` on that basis.

The options as they were weighed, kept for the record:

- **(a) Rights confirmed.** The owner authored Whip-Process or holds permission under an MIT-compatible
  licence. Record the licence and attribution beside the ported files (as `grill-with-docs` now does) and
  flip the five rows to `CLEAR`.
- **(b) Rewrite.** Re-express the nine files in original words. Their *history* still contains the
  verbatim versions, so the paths also go in `seed-scrub-paths.txt` (history dropped) and the rewritten
  files are committed to the layer after the seed.
- **(c) Leave the subsystem out.** Scrub the nine files (plus the two ported test suites) from the seed.
  The layer then ships without the review-panel / work-item loop.

### D2 — owner's personal e-mail · non-blocking

Already public through Smatchet's history, so this is a preference, not a leak. If wanted, the seed can
map it to `alexandrosk0@users.noreply.github.com` with `git filter-repo --mailmap` and replace the two
fixture occurrences with `--replace-text`. That needs a small seed-script change and is not wired today.

## Decisions already taken (not rows in the manifest)

These paths are deliberately **absent** from `seed-paths.txt`. Recorded so the omission reads as a
decision rather than an oversight:

| Path | Verdict | Why |
|---|---|---|
| `agents/project/` | `EXCLUDE` | Project-scoped by construction. This is the reason the manifest enumerates four explicit `agents/*` subtrees instead of one bare `agents/` prefix. |
| `agents/project/workflows/historical-review-sweep.js` | `EXCLUDE` | Self-identifies as non-portable. Already excluded wholesale by the `agents/project/` decision above, so it needs **no** paired scrub pass — the plan's row-8 text predates its move out of `agents/scripts/core/`. |
| `agents/README.md` | `EXCLUDE` | Sits directly under `agents/`, inside no allowed subtree, and its prose is host-specific ("every Smatchet subagent"). The layer needs its own. |
| `docs/self-improvement/categories/`, `postmortems.md`, `applied.md`, `*.jsonl` | `EXCLUDE` | Entries stay host-side (grill decision 4 / ADR-0025). Only the framework spec `AGENT_SELF_IMPROVEMENT.md` moves. |
| `docs/plans/`, `docs/work/`, `Source/`, `CMakeLists.txt` | `EXCLUDE` | Host content. `seed-agent-layer-repo.sh` phase 2 hard-fails if any of these ever appears in the manifest. |

## Manifest rows

| # | Path | Verdict | Reviewer | Date | Notes |
|---|---|---|---|---|---|
| 1 | `agents/core/` | `CLEAR` | Claude (agent) | 2026-09-13 | No secrets, internal hosts or tickets in head or history. `p4-janitor.md` shows `P4USER=alexk` (the owner's handle, already public). Host-literal prose is the de-Smatchet-ification follow-up, not a publication blocker. |
| 2 | `agents/_shared/` | `CLEAR` | Claude (agent) | 2026-09-14 | Whip-Process-derived: `skills/address-review-feedback`, `skills/close-work-item`, `skills/pre-implementation-review` (14–24% verbatim) and the post-implementation section of `skills/adversarial-code-review` (3%) carry Whip-Process text. Everything else clear; `skills/grill-with-docs` is MIT upstream and now ships its notice (`UPSTREAM-LICENSE`). **D1 resolved (a), 2026-09-14:** the owner confirmed the Whip-Process author gave permission to use it under this repository's MIT licence; each ported file now says so beside its provenance note. |
| 3 | `agents/scripts/core/` | `CLEAR` | Claude (agent) | 2026-09-14 | Whip-Process-derived: `lib/review-guard.sh`, `run-review.sh`, `work_item_lint.py` are ports of Whip-Process PowerShell tools (17–18% textual overlap). Every secret-shaped string in this subtree is a synthetic redaction fixture (see § Findings, Secrets). **D1 resolved (a), 2026-09-14:** the owner confirmed the Whip-Process author gave permission to use it under this repository's MIT licence; each ported file now says so beside its provenance note. |
| 4 | `agents/scripts/project/` | `CLEAR` | Claude (agent) | 2026-09-13 | Generic `//depot/path` examples, `localhost:1666`, the public gradle.org checksum page. No findings. |
| 5 | `docs/agent-rules/` | `CLEAR` | Claude (agent) | 2026-09-14 | Whip-Process-derived: `work-items.md` is **62% verbatim** from Whip-Process `Process.md` + `Conventions.md`; `review-panels.md` 22% from `Procedures/ReviewBasics.md`. Rest clear: public hosts only (`msdl.microsoft.com`, `api.deepseek.com`), the owner's `C:/Dev/Smatchet` checkout layout in `process-rules.md`. **D1 resolved (a), 2026-09-14:** the owner confirmed the Whip-Process author gave permission to use it under this repository's MIT licence; each ported file now says so beside its provenance note. |
| 6 | `docs/harness/` | `CLEAR` | Claude (agent) | 2026-09-13 | `SETUP.md` names the owner's `C:/Dev/Smatchet` checkout (machine layout, not sensitive). The pi subagent example is copied at setup time into gitignored `.pi/` and is never tracked, so nothing third-party ships from here. |
| 7 | `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` | `CLEAR` | Claude (agent) | 2026-09-13 | Framework spec only — no entry text. Its `## Index` links to host-side category files, which will dangle layer-side: a link-checker concern, not a publication one. |
| 8 | `docs/high-integrity/portable-purity-baseline.txt` | `CLEAR` | Claude (agent) | 2026-09-13 | Path + literal lists only. |
| 9 | `docs/high-integrity/agent-size-baseline.md` | `CLEAR` | Claude (agent) | 2026-09-13 | Path + line-count lists only. |
| 10 | `AGENTS.md` | `CLEAR` | Claude (agent) | 2026-09-13 | Links to public third-party docs only (`agents.md`, `raw.githubusercontent.com/mattpocock/skills`). Root rulebook only — leaf `AGENTS.md` files stay host-side (asserted in phase 4b). |
| 11 | `scripts/dev/project-config.sh` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings. |
| 12 | `scripts/dev/test-all.sh` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings. |
| 13 | `scripts/dev/test-docs.sh` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings. |
| 14 | `tests/dev/comment_tooling/` | `CLEAR` | Claude (agent) | 2026-10-04 | `test_comment_tooling.py`, the suite behind `test-comment-tooling.sh` for the seeded `comment_audit.py` / `comment_strip.py`; synthetic in-memory fixtures. Added after the review of the row-8 correction found its wrapper skipping by exit 2 in the standalone image. Full-history sweep (7 commits, 326 lines) + gitleaks: no findings beyond commit-trailer `claude.ai` / noreply values. `SMATCHET_DEVIATION` literals are the gate's own grammar — de-Smatchet-ification follow-up. |
| 15 | `tests/fixtures/function_size/` | `CLEAR` | Claude (agent) | 2026-10-04 | 4 synthetic C++ fixtures for `function_size_audit.py`. Full-history sweep (2026-10-04) + gitleaks: no findings. Each carries `namespace smatchet` — a host literal, the de-Smatchet-ification follow-up (fixtures are outside the portable-purity scope), not a publication blocker. |
| 16 | `tests/fixtures/lint_rules/` | `CLEAR` | Claude (agent) | 2026-10-04 | 17 synthetic C++ fixtures for the lint-rules gates. Full-history sweep (2026-10-04) + gitleaks: no findings. The only host tokens are `SMATCHET_DEVIATION` markers (the gate's own escape grammar) and one `SmatchetLocalizedImGui` name — de-Smatchet-ification follow-up. |
| 17 | `tests/fixtures/shell_lint/` | `CLEAR` | Claude (agent) | 2026-10-04 | 16 known-good / known-bad shell fixtures for `test-shell-lint.sh`. Full-history sweep (2026-10-04) + gitleaks: no findings. URLs are `example.com` only. |
| 18 | `tests/fixtures/lint_hook_probe.cpp` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic probe for `test-lint-hook-split.sh`. Full-history sweep (2026-10-04) + gitleaks: no findings. One `smatchet_lint_probe` namespace — de-Smatchet-ification follow-up. |
| 19 | `tests/fixtures/ci_parity_config_clean.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 20 | `tests/fixtures/ci_parity_config_filtered.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 21 | `tests/fixtures/ci_parity_config_nopr.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 22 | `tests/fixtures/ci_parity_config_selfgated.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 23 | `tests/fixtures/ci_parity_config_templated.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 24 | `tests/fixtures/ci_parity_config_unresolvable.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 25 | `tests/fixtures/ci_parity_wf_clean.yml` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 26 | `tests/fixtures/ci_parity_wf_filtered.yml` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 27 | `tests/fixtures/ci_parity_wf_nopr.yml` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 28 | `tests/fixtures/ci_parity_wf_selfgated.yml` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 29 | `tests/fixtures/ci_parity_wf_templated.yml` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic workflow/config fixture for `required_context_parity.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 30 | `tests/fixtures/merge_gates_bb_clean.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 31 | `tests/fixtures/merge_gates_bb_findings.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 32 | `tests/fixtures/merge_gates_bb_stale.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 33 | `tests/fixtures/merge_gates_bb_terminal.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 34 | `tests/fixtures/merge_gates_ci_fail.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 35 | `tests/fixtures/merge_gates_ci_pending.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 36 | `tests/fixtures/merge_gates_cr_changes.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 37 | `tests/fixtures/merge_gates_cr_current_clean.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 38 | `tests/fixtures/merge_gates_cr_findings.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 39 | `tests/fixtures/merge_gates_cr_size_skip.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 40 | `tests/fixtures/merge_gates_cr_stale.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 41 | `tests/fixtures/merge_gates_cr_stale_clean.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 42 | `tests/fixtures/merge_gates_cr_stale_findings.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 43 | `tests/fixtures/merge_gates_cr_stale_resolved.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 44 | `tests/fixtures/merge_gates_dedup_rerun_pass.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 45 | `tests/fixtures/merge_gates_label_intent_oob.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 46 | `tests/fixtures/merge_gates_label_oob_other_fail_blocks.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 47 | `tests/fixtures/merge_gates_label_perf_oob.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 48 | `tests/fixtures/merge_gates_label_tests_oob.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 49 | `tests/fixtures/merge_gates_pagination.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 50 | `tests/fixtures/merge_gates_pass.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. One `"login"` is the owner's GitHub handle, already public through this repository and its commit metadata — the D2 class (non-blocking), not a leak. Every other login is a bot or a placeholder. |
| 51 | `tests/fixtures/merge_gates_state.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 52 | `tests/fixtures/merge_gates_user_comment.json` | `CLEAR` | Claude (agent) | 2026-10-04 | Synthetic GraphQL payload for `merge_gates.bats`. Full-history sweep (2026-10-04) + gitleaks: no findings. Logins are bots (`coderabbitai[bot]`, `cursor[bot]`, `github-actions`) or placeholders. |
| 53 | `tests/bats/agent_size.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 54 | `tests/bats/android_openssl_failfast.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 55 | `tests/bats/archive_backlog_entry.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | `example.com` link fixture only. |
| 56 | `tests/bats/capture_intent.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | Synthetic token fixtures only — verified placeholders (see § Findings, Secrets). Also uses the owner's real e-mail as a redaction fixture: **D2**, non-blocking. |
| 57 | `tests/bats/coderabbit_triage.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 58 | `tests/bats/cr_oob_review_backfill.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 59 | `tests/bats/dead_export_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 60 | `tests/bats/dup_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 61 | `tests/bats/fail_open_authoring.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 62 | `tests/bats/fleet_preflight.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 63 | `tests/bats/fleet_rescope.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 64 | `tests/bats/followup_due_nudge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 65 | `tests/bats/function_size.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 66 | `tests/bats/fuzz_closure_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 67 | `tests/bats/gate_selftests.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 68 | `tests/bats/git_janitor.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 69 | `tests/bats/guard_head_drift.bats` | `CLEAR` | Claude (agent) | 2026-10-04 | Harness-hook suite, newly seeded by the wrapper-derived bats rule (2026-10-04). Full-history sweep (2026-10-04) + gitleaks: no findings. `SMATCHET_*` env knobs are host literals (de-Smatchet-ification follow-up); `CR-953` is a review-thread id, not a ticket. |
| 70 | `tests/bats/guard_plan_lock.bats` | `CLEAR` | Claude (agent) | 2026-10-04 | Harness-hook suite, newly seeded by the wrapper-derived bats rule (2026-10-04). Full-history sweep (2026-10-04) + gitleaks: no findings. `SMATCHET_*` env knobs are host literals (de-Smatchet-ification follow-up); `CR-953` is a review-thread id, not a ticket. |
| 71 | `tests/bats/guard_shared_tree.bats` | `CLEAR` | Claude (agent) | 2026-10-04 | Newly seeded by the wrapper-derived bats rule (2026-10-04): its wrapper already lived in `agents/scripts/**`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 72 | `tests/bats/harness_provisioned_doctor.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 73 | `tests/bats/historical_review_ledger_reconcile.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 74 | `tests/bats/historical_review_survivors.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 75 | `tests/bats/include_cycle_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 76 | `tests/bats/is_pure_docs_diff.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 77 | `tests/bats/issue_sweep.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 78 | `tests/bats/lint_rules.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 79 | `tests/bats/lock_claim.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 80 | `tests/bats/lock_staleness_sweep.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 81 | `tests/bats/markdown_links.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | `example.com` / `a.b` / `ftp://server` link fixtures only. |
| 82 | `tests/bats/merge_gates.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 83 | `tests/bats/merge_watcher.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 84 | `tests/bats/merge_watcher_integration.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 85 | `tests/bats/migrate_bugs_to_issues.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 86 | `tests/bats/oob_label_impl.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 87 | `tests/bats/panel_verdicts.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | Comment reference to the Whip-Process absorption plan only; suite authored first-party (absorption Phase 4), not a port. |
| 88 | `tests/bats/plan_archival_owed.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 89 | `tests/bats/plan_index_robustness.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 90 | `tests/bats/plan_lock_gate.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 91 | `tests/bats/postmortem_owed.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 92 | `tests/bats/pre_push_guard.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 93 | `tests/bats/preship_review_artifact.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 94 | `tests/bats/pretool_workflow_fleet_preflight.bats` | `CLEAR` | Claude (agent) | 2026-10-04 | Newly seeded by the wrapper-derived bats rule (2026-10-04): its wrapper already lived in `agents/scripts/**`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 95 | `tests/bats/project_config_roots.bats` | `CLEAR` | Claude (agent) | 2026-10-04 | Newly seeded by the wrapper-derived bats rule (2026-10-04): its wrapper already lived in `agents/scripts/**`. Full-history sweep (2026-10-04) + gitleaks: no findings. |
| 96 | `tests/bats/repo_health_facts_nudge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 97 | `tests/bats/required_context_adr_consistency.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 98 | `tests/bats/required_context_parity.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 99 | `tests/bats/resolve_py.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 100 | `tests/bats/resolve_repo.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 101 | `tests/bats/review_ack_gate.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 102 | `tests/bats/review_guard.bats` | `CLEAR` | Claude (agent) | 2026-09-14 | Whip-Process-derived: self-declares "test semantics ported from Tools/test-run-review.ps1". Textual overlap below the scan floor — a translation, cleared together with the tool it tests. **D1 resolved (a), 2026-09-14:** the owner confirmed the Whip-Process author gave permission to use it under this repository's MIT licence; each ported file now says so beside its provenance note. |
| 103 | `tests/bats/run_review.bats` | `CLEAR` | Claude (agent) | 2026-09-14 | Whip-Process-derived: self-declares "test semantics ported from Tools/test-run-review.ps1". Textual overlap is low (0.5%) — a translation, cleared together with the tool it tests. **D1 resolved (a), 2026-09-14:** the owner confirmed the Whip-Process author gave permission to use it under this repository's MIT licence; each ported file now says so beside its provenance note. |
| 104 | `tests/bats/safe_admin_merge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 105 | `tests/bats/safe_merge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 106 | `tests/bats/script_freshness.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 107 | `tests/bats/session_registry.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 108 | `tests/bats/setup_branch_protection.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 109 | `tests/bats/shell_lint.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 110 | `tests/bats/small_helper_audit.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 111 | `tests/bats/subsystem_docs.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | `t@t.test` fixture identity only. |
| 112 | `tests/bats/sync_issue_labels.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 113 | `tests/bats/unwatched_pr_nudge.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 114 | `tests/bats/verifier_preship_wiring.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | Loopback (`127.0.0.1`) test endpoints only. |
| 115 | `tests/bats/verifier_review_gate.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | Comment reference to the Whip-Process absorption plan only; the panel end-to-end section is first-party, not a port. |
| 116 | `tests/bats/work_item_owed.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 117 | `tests/bats/workflow_job_mask.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 118 | `tests/bats/workflow_watchdog.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |
| 119 | `tests/bats/worktree_prune.bats` | `CLEAR` | Claude (agent) | 2026-09-13 | No findings in head or history. |

## Status

**119 of 119 rows cleared** (0 `PENDING`), so the **verdict** gate (phase 4b) is satisfied, and as of
the pin above the audit is **current**: the **freshness** guard (phase 3) passes until a later commit
touches a manifest path. The pin moved from `3e6c0c75` to `5e9dd311` as the last step before the seed:
that sweep covered 19 commits and 5,738 added lines, with no binaries and no secret, internal-host or
ticket-URL hit. The first real seed then found defects in the seed script itself, a manifest path, so the
fixes (#2319) left the pin stale by one move. The sweep `--since 5e9dd311` on `2639882a` covered 2 commits
and 109 added lines, again with no binaries and no secret, internal-host or ticket-URL hit. Every value
either sweep raised was benign and of a kind already triaged: `claude.ai` session links, commit-trailer
`noreply` addresses, the `t@t.test` fixture identity, `ADR-`/`CR`/`PR` words read as ticket keys, and the
owner's `C:/Dev/Smatchet` layout (in one commit message, and quoted in this file's own status text).

**A pin can only name a commit that already exists on develop.** A change to manifest paths therefore
always lands after the pin it sets. Its content can be swept before merge, but the pin cannot name its
squash until that squash exists. The gap is closed by a follow-up change touching **only this file**, which
the freshness guard ignores: re-sweep `--since` the current pin, then move **Audited through:** to the new
develop tip. That is how the pin reached `3e6c0c75` after the D1 change merged as #2220, `5e9dd311` before the seed, and its current value after the seed fixes.

The same applies whenever the tree moves past the pin (it will — the surface takes several commits a day):
re-sweep only the delta, triage the new values, update the affected rows, and move **Audited through:**:

```bash
python3 agents/scripts/core/seed-audit-sweep.py --since <pin>
```
