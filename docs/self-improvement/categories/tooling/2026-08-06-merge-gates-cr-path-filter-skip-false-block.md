- 2026-08-06 · orchestrator · [tooling] · P2 — `merge-gates.sh` misclassifies **every** CodeRabbit skip as the too-many-files size-skip: the `$crskip` disjunct `contains("skip review by coderabbit.ai")` matches the HTML marker CR emits for *any* skip reason, so a **path-filter** skip is routed into the size-skip hard-block arm. Currently **masked** for the `docs/self-improvement/**` class by the 2026-06-20 auto-exemption, so it only bites other skip classes — hence P2, not P1
  Details: The `$crskip` computation at `agents/scripts/core/merge-gates.sh:552-554` is:
      `any(contains("skip review by coderabbit.ai") or (test("##[[:space:]]*Review skipped"; "i") and (ascii_downcase | contains("too many files"))))`
    CR's skip comment carries the literal marker `<!-- This is an auto-generated comment: skip review
    by coderabbit.ai -->` for **every** skip reason (path filters, docs-only, trivial diff,
    too-many-files). The first disjunct therefore fires unconditionally on any skip, `$crskip=true`,
    and the `case NONE` size-skip arm (:957-961) short-circuits to `cr_pass=false` +
    `cr_size_skip_block=true` with "CodeRabbit skipped review — too many files (exceeds CR file
    limit); split the PR" — advice that is unactionable when the diff is one file. The arm is
    deliberately a hard short-circuit (it closed the hole that let a 638-file reorg merge with zero CR
    review), so it also pre-empts the PASS arm below it, `$crreviewskipped` (:564-569) — which would
    not have fired anyway: it keys on the `CodeRabbit` StatusContext **description** matching "review
    skipped", and on the observed PRs that description read `Review completed`. CR's status text and
    its comment text disagree about the same run, so the intended fallback is keyed on a field that
    does not carry the fact.
  Scope of the damage today — smaller than it first appeared, verified 2026-08-06: commit `4685997d`
    (2026-06-20, "feat(merge-gates): auto-exempt pure self-improvement doc PRs from CR + Bugbot
    review", #1468) added a belt-and-suspenders downgrade at :1201-1202 — when
    `self_imp_only` (tuple field 27, diff entirely under `docs/self-improvement/**`) is true, a CR
    block is downgraded to `WARN: self-improvement doc PR — CR gate auto-skipped`. So on current
    `develop` the misclassification is **printed but not load-bearing** for self-improvement doc PRs;
    confirmed on PR #1961, where the bogus size-skip BLOCK appeared every poll and the run still
    reached `GATES_PASSED` at poll 12. The bug remains live for any *other* skip class — a different
    `.coderabbit.yaml` path filter, a docs-only or trivial-diff skip on a non-self-improvement path —
    where nothing downgrades it and the only exits are the `cr-out-of-band` label or an out-of-band
    merge. That residual case is the reason to still fix it.
  Correction to the first draft of this entry: it was written from a poller run on PR #1953 that
    appeared to block indefinitely, and claimed the sanctioned merge path was structurally unusable
    for self-improvement PRs. That was wrong on two counts and the ledger entry built on it has been
    withdrawn. (1) That poller was invoked from the long-lived integration tree
    (`C:/Dev/Smatchet` @ `ff0ee7a6`), whose `merge-gates.sh` predates `4685997d` and has no downgrade
    — the block was an artefact of a stale checkout, not of current `develop`; filed separately as
    [`process/2026-08-06-gate-tooling-run-from-stale-session-branch.md`](../process/2026-08-06-gate-tooling-run-from-stale-session-branch.md).
    (2) That run did **not** end in `GATES_TIMEOUT` at 60/60 as first reported — both poll logs end
    `PR_MERGED` at poll 49, i.e. the poller observed the merge and exited normally. No gate was
    escaped on #1953: CI was 22/22, `CR findings (0 actionable)` was SUCCESS with description
    `self-improvement-only diff (1 file(s) under docs/self-improvement/**) — CR review exempt`, there
    were no unresolved threads and no override label, and a sibling session merging an open green PR
    on the shared login is documented-expected
    ([`process-rules.md`](../../../agent-rules/process-rules.md) § Git/p4 discipline).
  Concrete next action: two changes in `agents/scripts/core/merge-gates.sh`, plus test pins.
    (1) **Narrow `$crskip`** — drop the bare `contains("skip review by coderabbit.ai")` disjunct (a
    generic skip marker, not a size marker) and keep only the size-specific test, i.e.
    `any(test("##[[:space:]]*Review skipped"; "i") and (ascii_downcase | contains("too many files")))`.
    A genuine too-many-files skip still blocks — it carries that exact phrase; a path-filter /
    docs-only / trivial-diff skip then falls through to the terminal-pass arms as intended. This is
    the whole fix; the rest is defence in depth.
    (2) **Let the repo's own required gate win** — when the `CR findings (0 actionable)` StatusContext
    is SUCCESS on the current head, pass the CR bucket without re-deriving a verdict. That context is
    required and already fail-closed (it returns non-terminal rather than guess — see
    `docs/self-improvement/postmortems.md`, 2026-08-06 · PR #1948), so a poller re-derivation that
    disagrees with it can only produce a false block, never catch a real escape. Order it ahead of the
    `case NONE` chain. Note this also makes the :1201 self-improvement downgrade redundant for the
    common case rather than load-bearing, which is the healthier arrangement — today a masking
    downgrade is the only thing standing between the misclassification and a false block.
    (3) **Pin in `tests/bats/merge_gates.bats`**: (a) path-filter skip comment on a NON-self-improvement
    path + `CR findings` SUCCESS → PASS (the currently-unmasked case); (b) `## Review skipped` +
    "too many files" → still BLOCK (the #638-reorg contract preserved); (c) skip comment +
    `CR findings` SUCCESS → PASS via the new precedence rule, with `self_imp_only=false` so the test
    cannot pass merely via the :1201 downgrade.
    Est ~0.5d (jq edit + precedence arm + 3 bats cases).
  Cross-ref: `agents/scripts/core/merge-gates.sh` (:552-554 `$crskip`, :564-569 `$crreviewskipped`,
    :937-980 the `case NONE` arms, :1201-1202 the self-improvement downgrade that masks it,
    :1164-1167 the size-skip override message); `4685997d` / PR #1468 (the masking exemption);
    PR #1953 + PR #1961 (observations); `.coderabbit.yaml` (`!docs/self-improvement/**` path filter);
    `.github/actions/cr-finding-gate/action.yml` (the required gate whose SUCCESS the poller
    re-derives). Related: [`process/2026-08-05-cr-finding-gate-empty-body-review-wedge.md`](../process/2026-08-05-cr-finding-gate-empty-body-review-wedge.md)
    — the mirror-image hole (gate cannot reach a verdict) in the same CR path.
