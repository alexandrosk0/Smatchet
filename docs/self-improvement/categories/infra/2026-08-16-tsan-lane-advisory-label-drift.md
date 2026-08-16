- 2026-08-16 · orchestrator · [infra] · P2 — the TSan lane is documented as advisory in four places but its check name carries no `advisory` token, so the merge poller blocks on it
  Details: Surfaced while grounding `docs/plans/shipped/autonomous-debug-live-evidence.md`;
    unrelated to that plan's subject, filed here per `docs/agent-rules/ship-loops.md`
    § "Unrelated work never shares a PR".
    `.github/workflows/tsan-linux-nightly.yml:51` publishes the check name
    **`TSan Linux subset (Clang)`**. The poller's only exemption is a
    case-insensitive `contains("advisory")` on the check NAME
    (`agents/scripts/core/merge-gates.d/10-gate-filter.sh:66-70`, `:91-95`) under
    `MERGE_GATES_BLOCK_ALLOWLIST_RE="."`. That name has no such token, so a red
    TSan run **blocks the merge gate** — while four places assert the opposite:
    `tsan-linux-nightly.yml:13-14` ("ADVISORY / non-required"),
    `docs/plans/active/tsan-imgui-linked-target.md:79` and `:92`, and
    `docs/plans/active/build-quality-velocity-hardening.md:244`.
    The exposure is bounded but real: the workflow's `pull_request` trigger is
    paths-scoped (`CMakeLists.txt`, `CMakePresets.json`, `cmake/Sanitizers.cmake`,
    `tests/CMakeLists.txt`, `Source/Core/src/Sync/**`, `.../Persistence/**`,
    `.../Config/**`, `GridLiveContext.cpp`), so it only lands on PR heads touching
    those paths — but when it does, "advisory" is documentation, not mechanism.
    This is the inverse of the sibling entry
    infra/2026-08-16-stale-advisory-lane-docs: there the docs under-state what
    blocks; here they over-state what doesn't.
    Note the decision is genuinely open, not merely a doc fix — a required-ish
    TSan lane may well be *desirable*. What is not defensible is the current state,
    where the behaviour and the four documents disagree.
  Concrete next action: pick one and make the other side match. Either
    (a) rename the check to `TSan Linux subset (Clang, advisory)` so the mechanism
    matches the four docs, or (b) keep it blocking and correct all four docs plus
    `AGENTS.md` § Merge gates / `docs/agent-rules/merge-gates.md`'s advisory-token
    user list. Prefer (a) unless the lane's green-rate on the scoped paths is
    already high enough to gate on — check the last ~20 scoped runs before
    deciding. Also worth extending `tests/bats/merge_gates.bats` with a
    name-vs-intent case so a future lane cannot claim advisory in prose while
    blocking in fact.
  Status: open
  Last-reviewed: 2026-08-16
