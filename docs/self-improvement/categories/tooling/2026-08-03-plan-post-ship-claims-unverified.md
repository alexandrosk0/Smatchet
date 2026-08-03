- 2026-08-03 · orchestrator · [tooling] · P1 — a shipped plan's § Deviations / § Implementation log can assert a delivery that never landed and **no gate reads it**: `msvc-build-onboarding-hardening.md:85` claims "`build_standalone.ps1` (plan file 1) already had the MSVC bootstrap from slice 1" — `git log -S vcvars` on that file is **empty across all history**; the promised vcvars/vswhere env import was never written, in any revision
  Details: Surfaced while validating the `dev-onboarding-first-run-quickstart` plan, whose § Context
    premise ("MSVC bootstrap already exists, just needs a root entry point") was inherited from that
    line. Chain: PR #493 planned "locate `vcvars64.bat` through `vswhere.exe` … import via
    `cmd /c \"...vcvars64.bat && set\"`" for `build_standalone.ps1`. PR #495 (`da36b45f`, 2026-05-28)
    shipped the *other* items and closed the row with the § Deviations line above. The blameless root
    cause is a **name conflation**: the file did contain a `Use-Msys2Ucrt64Environment` call (an **MSYS2**
    UCRT64 env bootstrap) which #495 replaced with the retirement `throw` — that pre-existing env-setup
    call was read as "the bootstrap", so the row was closed as already-done rather than dropped. The
    file's only `vswhere` use is `Get-VsWherePath` (:71-95), which locates **MSBuild.exe**, not vcvars.
    Nothing contradicted the claim: § Verification (actual) lists `test-build-wrapper.ps1` 3/3 green, but
    all three cases test the msys2-retirement throw, the `Exe :`/`Time:` print, and the stale-sibling
    table — **none exercises an MSVC env bootstrap**, so a passing verification block is fully consistent
    with the capability being absent. And `postmortem-owed.sh --list` returns "no gate escapes owed": the
    nudge reads merge signals (non-SUCCESS checks, override labels, `Revert`, overdue deviations), so an
    *untrue prose claim* in a doc is structurally invisible to it. The claim then sat load-bearing for
    ~2 months and seeded a false premise into a downstream plan.
  Concrete next action: add gate rule **`plan-claim-anchor`** —
    `agents/scripts/core/test-plan-claim-anchors.sh`, joining the existing plan-doc gate family
    (`test-plan-index.sh` / `test-plan-ref-integrity.sh` / `test-markdown-links.sh`) in the
    "Doc anchors + agent contract" doc-validation job. Rule: inside a plan's **§ Deviations** or
    **§ Implementation log** sections only, a line matching the pre-existing-delivery claim set
    (`already had|has|have|exists|existed|implemented|landed|shipped`, `was already`) MUST carry a
    verifiable citation — a markdown link or backticked ref with a `:<line>` suffix, or a `#<PR>` /
    commit-sha reference. Delta-gated vs `origin/develop` and baseline-grandfathered like every other
    rule (measured: **22 such claims exist today across `plans/{shipped,active}`, 21 unanchored** — all
    grandfathered; only NEW claims must anchor). Escape:
    `SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=…; owner=…; revisit=…)` for claims about state
    outside the repo (e.g. `solo-merge-review-gate.md:91` cites GitHub branch-protection API state,
    which has no `file:line`). This does not prove a claim true — it forces the author to point at the
    code, and **there is no line to point at for a vcvars import that does not exist**, which is exactly
    where #495 would have stopped. Est ~0.5d (bash gate + `--selftest` + AGENTS.md contract-card row).
    Explicitly NOT proposed: extending `postmortem-owed.sh` — this class carries no merge signal, so
    detection belongs at doc-gate time, not at merge-nudge time.
  Cross-ref: `docs/plans/shipped/msvc-build-onboarding-hardening.md` (:82 impl-log, :85 the false
    § Deviations claim, § Verification (actual) 3/3 non-covering tests); `scripts/dev/local/build_standalone.ps1`
    (:71-95 `Get-VsWherePath` → MSBuild only, zero vcvars); PR #493 (`a9058b96`, plan) / **PR #495**
    (`da36b45f`, the escaping PR); `scripts/dev/with-msvc.ps1` :53-139 (where a real vcvars import DOES
    live — the capability exists in the tree, just not in the file the plan named);
    `docs/plans/active/dev-onboarding-first-run-quickstart.md` (downstream plan that inherited the false
    premise); `docs/self-improvement/postmortems.md` (ledger entry).
  Status: open
  Last-reviewed: 2026-08-03
