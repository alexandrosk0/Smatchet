- 2026-10-06 · agent-surface extraction · [tooling] · P2 — the layer repo's owner-only setup steps are still outstanding

Details:
`seed-agent-layer-repo.sh` scripts everything about the layer repo that a token can do, and it
published `the-unwilling-agentic-bunch`. Three steps need the owner's credentials. None is done yet,
but the prerequisites for steps 2 and 3 are in place:

1. **Branch protection on `develop`.** The seed's protection step (the three lane contexts,
   conversation resolution, no force-push or deletion) got 403 from the protection API in the session
   that seeded the repo, so it was not applied. Until it is, the layer's lanes report but do not block.
2. **Row 9 Accept, the blocked half.** The probe PR the-unwilling-agentic-bunch#3 is open: it adds a
   broken `AGENTS.md §` reference, and `Doc anchors + agent contract` and the bats lane went red on
   it, on nothing else (plan row 96). While `develop` is unprotected GitHub calls it `unstable`. Once
   protection is on, confirm it reads `blocked`, record that in row 81, and close the PR unmerged.
3. **Auto-bump.** `auto-bump.yml` stays off until the layer has the `AGENT_LAYER_BUMP_PAT` secret
   (contents and pull-requests write on Smatchet) and the `AUTO_BUMP=on` variable. Until then,
   git-janitor's backstop proposes bumps. `ship-loops.md` § Two-repo ship-loop (d) allows turning it
   on only after `agent-layer-integration.yml` has been seen passing on a gitlink-only bump PR. That
   happened on Smatchet #2322 (plan row 95), so the secret and the variable can be added now.

Concrete next action:
- Owner: from a standalone clone of the layer (not Smatchet's `agent-layer/` mount, where the
  script reads Smatchet's config and would require Smatchet's contexts), run
  `REPO=alexandrosk0/the-unwilling-agentic-bunch bash agents/scripts/core/setup-branch-protection.sh --dry-run`
  and confirm the body lists exactly the three layer lanes. Then run it without `--dry-run` (the command
  the seed prints on that failure). Then check step 2 on the open probe PR and record the result in
  row 81 of `docs/plans/agent-surface-extraction-repo.md`.
- Owner: add the secret and the variable. The first auto-bump PR then proves the automation itself.

Status: open
Last-reviewed: 2026-10-06
