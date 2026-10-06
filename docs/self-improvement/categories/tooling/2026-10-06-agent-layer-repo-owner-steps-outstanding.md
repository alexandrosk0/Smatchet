- 2026-10-06 · agent-surface extraction · [tooling] · P2 — the layer repo's owner-only setup steps are still outstanding

Details:
`seed-agent-layer-repo.sh` scripts everything about the layer repo that a token can do, and it
published `the-unwilling-agentic-bunch`. Three steps need the owner's credentials, and none is done yet:

1. **Branch protection on `develop`.** The seed's protection step (the three lane contexts,
   conversation resolution, no force-push or deletion) got 403 from the protection API in the session
   that seeded the repo, so it was not applied. Until it is, the layer's lanes report but do not block.
2. **Row 9 Accept.** It runs once protection is on. Open a throwaway layer PR with a broken
   `AGENTS.md §` reference, confirm `Doc anchors + agent contract` goes red and blocks the merge, then
   close it.
3. **Auto-bump.** `auto-bump.yml` stays off until the layer has the `AGENT_LAYER_BUMP_PAT` secret
   (contents and pull-requests write on Smatchet) and the `AUTO_BUMP=on` variable. Until then,
   git-janitor's backstop proposes bumps. Per `ship-loops.md` § Two-repo ship-loop (d), turn it on only
   after `agent-layer-integration.yml` has been seen firing and passing on a gitlink-only bump PR.

Concrete next action:
- Owner: from a standalone clone of the layer (not Smatchet's `agent-layer/` mount, where the
  script reads Smatchet's config and would require Smatchet's contexts), run
  `REPO=alexandrosk0/the-unwilling-agentic-bunch bash agents/scripts/core/setup-branch-protection.sh --dry-run`
  and confirm the body lists exactly the three layer lanes. Then run it without `--dry-run` (the command
  the seed prints on that failure), run step 2, and record the result in row 81 of
  `docs/plans/agent-surface-extraction-repo.md`.
- After a gitlink-only bump PR in Smatchet shows `agent-layer-integration.yml` firing and passing, the
  owner adds the secret and the variable. The first auto-bump PR then proves the automation itself.

Status: open
Last-reviewed: 2026-10-06
