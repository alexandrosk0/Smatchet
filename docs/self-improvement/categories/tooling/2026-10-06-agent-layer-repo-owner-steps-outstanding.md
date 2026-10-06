- 2026-10-06 · agent-surface extraction · [tooling] · P3 — the layer repo's last owner-only setup step (auto-bump) is outstanding

Details:
`seed-agent-layer-repo.sh` scripts everything about the layer repo that a token can do, and it
published `the-unwilling-agentic-bunch`. Three steps needed the owner's credentials:

1. **Branch protection on `develop` — done 2026-10-06.** The seeding session's proxy got 403 from the
   protection API, so the owner applied it. The branch API shows it requiring exactly the three layer
   lanes, with enforcement `non_admins`.
2. **Row 9 Accept — done 2026-10-06.** The probe PR the-unwilling-agentic-bunch#3 went red on its planted
   broken `AGENTS.md §` reference, and on nothing else. It read `unstable` before protection and `blocked`
   after, then was closed unmerged (plan rows 81 and 96).
3. **Auto-bump — open, optional.** `auto-bump.yml` stays off until the layer has the
   `AGENT_LAYER_BUMP_PAT` secret (contents and pull-requests write on Smatchet) and the `AUTO_BUMP=on`
   variable. Until then, git-janitor's backstop proposes bumps. `ship-loops.md` § Two-repo ship-loop (d)
   allows turning it on only after `agent-layer-integration.yml` has been seen passing on a gitlink-only
   bump PR. That happened on Smatchet #2322 (plan row 95), so nothing blocks it.

Concrete next action:
- Owner, when wanted: add the secret and the variable. The first auto-bump PR then proves the
  automation itself. Close this entry once it has merged.

Status: open
Last-reviewed: 2026-10-06
