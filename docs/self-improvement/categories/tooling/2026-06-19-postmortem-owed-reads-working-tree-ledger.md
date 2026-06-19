- 2026-06-19 · orchestrator · [tooling] · P3 — postmortem-owed.sh reads the cwd working-tree ledger instead of origin/develop, so a session whose integration tree is parked on a feature branch gets phantom "owed" nags (and, inversely, a locally-staged-but-unmerged entry can false-suppress a genuinely-owed postmortem)
  Details: The 2026-06-19 session-start postmortem-owed hook reported `PR #1409 — override:
    tests-out-of-band` owed, but that postmortem was already merged to develop — #1414, squash
    96e79412, entry `## 2026-06-19 · PR #1390, #1409` in postmortems.md. The detector ran from the
    integration tree (C:/Dev/Smatchet) which was checked out on feat/tsan-subset-sync-layer, a branch
    predating #1414. has_entry / has_sha_entry grep `LEDGER` (postmortem-owed.sh:64 default
    `docs/self-improvement/postmortems.md`, used at :124 and :143) as a cwd-relative WORKING-TREE path,
    so they read the stale checked-out file (0 matches) and nagged. Re-running with POSTMORTEM_LEDGER
    pointed at `git show origin/develop:…postmortems.md` reported `no gate escapes owed (last 20 merges
    clean)`. The false-positive direction is just startup noise; the dangerous inverse is a
    false-NEGATIVE — if the working tree holds a postmortem entry that is NOT yet merged to develop,
    has_entry returns true and silently suppresses a genuinely-owed postmortem on develop. The detector
    already scans the merges themselves against origin/develop (`git log origin/develop …` at :448/:492),
    so only the ledger read diverges from the ref everything else trusts.
  Concrete next action: pin the ledger read to the same ref the merges are scanned against. In
    has_entry / has_sha_entry, resolve the ledger via `git show "origin/develop:$LEDGER"` (cache once to
    a temp file, grep that) instead of reading the cwd working-tree path; keep POSTMORTEM_LEDGER as an
    explicit override so the bats rig can still point at a fixture. Add bats cases (sibling of the
    existing postmortem-owed tests): (1) working tree on a branch LACKING the entry while origin/develop
    HAS it → detector reports clean, no phantom owe; (2) entry present ONLY in the working tree, absent
    on origin/develop → still owes (guards the false-negative). Est ~0.5d.
  Cross-ref: postmortem-owed.sh:64 (LEDGER default), :124-133 (has_entry), :143-146 (has_sha_entry),
    :448/:492 (`git log origin/develop` merge scan); postmortems.md 2026-06-19 PR #1390, #1409 (the entry
    falsely reported owed); merge 96e79412 (#1414); session-start postmortem-owed hook.
  Status: open
  Last-reviewed: 2026-06-19
