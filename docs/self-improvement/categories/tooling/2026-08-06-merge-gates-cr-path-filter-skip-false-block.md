- 2026-08-06 · orchestrator · [tooling] · P1 — `merge-gates.sh` misclassifies **every** CodeRabbit skip as the too-many-files size-skip and hard-blocks: the `$crskip` disjunct `contains("skip review by coderabbit.ai")` matches the HTML marker CR emits for *any* skip reason, so a **path-filter** skip on a docs-only PR blocks forever while the repo's own required CR gate has already posted SUCCESS
  Details: Observed on PR #1953 (single file, `docs/self-improvement/categories/process/…md`).
    CR posted `## Review skipped — Review was skipped due to path filters` / "`…` is excluded by
    `!docs/self-improvement/**`" — the body contains **no** "too many files" text. But
    `agents/scripts/core/merge-gates.sh:552-554` computes:
      `any(contains("skip review by coderabbit.ai") or (test("##[[:space:]]*Review skipped"; "i") and (ascii_downcase | contains("too many files"))))`
    and CR's skip comment carries the literal marker `<!-- This is an auto-generated comment: skip
    review by coderabbit.ai -->` for **every** skip reason (path filters, docs-only, trivial diff,
    too-many-files). So the first disjunct fires unconditionally on any skip, `$crskip=true`, and the
    `case NONE` size-skip arm (:957-961) short-circuits to `cr_pass=false` + `cr_size_skip_block=true`
    with the message "CodeRabbit skipped review — too many files (exceeds CR file limit); split the PR".
    On a 1-file PR that advice is unactionable. The arm is deliberately a hard short-circuit (it closed
    the hole that let a 638-file reorg merge with zero CR review), so it also pre-empts the correct
    PASS path below it, `$crreviewskipped` (:564-569) — which never runs. The poller ran to
    `GATES_TIMEOUT` at poll 60/60 on #1953 with `CI: 22/22 pass` and every CR artifact green; the PR was
    merged out-of-band by a sibling session while the poller was still blocking. Compounding it, the
    `$crreviewskipped` PASS path would not have fired here either: it keys on the `CodeRabbit`
    StatusContext **description** matching "review skipped", and on #1953 that description was
    `Review completed` — CR's status text and its comment text disagree about the same run.
    Meanwhile the repo's OWN required gate had already ruled: `CR findings (0 actionable)` = SUCCESS,
    description `self-improvement-only diff (1 file(s) under docs/self-improvement/**) — CR review exempt`.
    The poller independently re-derives a CR verdict from raw CR artifacts and can therefore contradict
    the required, fail-closed gate that already decided — and when it does, the only exit is the
    `cr-out-of-band` label, i.e. the bug manufactures override use on exactly the diffs (docs-only)
    that least need one.
  Concrete next action: two changes in `agents/scripts/core/merge-gates.sh`, plus test pins.
    (1) **Narrow `$crskip`** — drop the bare `contains("skip review by coderabbit.ai")` disjunct (it is a
    generic skip marker, not a size marker) and keep only the size-specific test, i.e.
    `any(test("##[[:space:]]*Review skipped"; "i") and (ascii_downcase | contains("too many files")))`.
    A genuine too-many-files skip still blocks — it carries that exact phrase; a path-filter/docs-only
    skip then falls through to the terminal-pass arms as intended.
    (2) **Let the repo's own required gate win** — when the `CR findings (0 actionable)` StatusContext is
    SUCCESS on the current head, pass the CR bucket without re-deriving a verdict. That context is
    already required and already fail-closed (it returns non-terminal rather than guess), so a poller
    re-derivation that disagrees with it can only produce a false block, never catch a real escape.
    Order it ahead of the `case NONE` chain.
    (3) **Pin both in `tests/bats/merge_gates.bats`**: (a) path-filter skip comment + `CR findings`
    SUCCESS → PASS; (b) `## Review skipped` + "too many files" → still BLOCK (the #638-reorg contract
    preserved); (c) `CR findings` SUCCESS + any skip comment → PASS via the new precedence rule.
    Est ~0.5d (jq edit + precedence arm + 3 bats cases).
  Cross-ref: `agents/scripts/core/merge-gates.sh` (:552-554 `$crskip`, :564-569 `$crreviewskipped`,
    :937-980 the `case NONE` arms, :1164-1167 the size-skip override message); PR #1953 (the observed
    false-block; merged externally at `ec41fe86`); `.coderabbit.yaml` (`!docs/self-improvement/**` path
    filter); `.github/actions/cr-finding-gate/action.yml` (the required gate whose SUCCESS the poller
    overrode). Related: [`process/2026-08-05-cr-finding-gate-empty-body-review-wedge.md`](../process/2026-08-05-cr-finding-gate-empty-body-review-wedge.md)
    — the mirror-image hole (gate cannot reach a verdict) in the same CR path.
