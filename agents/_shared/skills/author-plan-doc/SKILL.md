---
name: author-plan-doc
description: Author a new plan-doc at docs/plans/active/<slug>.md from a one-line request — copy the template, set the slug to the basename, fill every section (N/A — <reason>, never delete), include the Perf-review-system-gates section when the diff would touch the project's perf-gated core source tree, keep the three mandatory Verification/Out-of-scope items the template bakes in (doc-validation suite, grill-with-docs stress-test, deferral residue-sweep), then git add + wip(plan) commit immediately (working-tree-only plans are lost on checkout). Use when the user says "write a plan", "draft a plan", "new plan", "plan for X", or "plan this". The authoring counterpart to grill-with-docs (the stress-test). Read-write — creates + commits the plan file.
version: 2
---

# author-plan-doc

The invocation shortcut for the most-repeated structured procedure in the
project: authoring a template-conformant plan-doc. The rules it encodes live in
[`docs/agent-rules/process-rules.md`](../../../../docs/agent-rules/process-rules.md)
§ Plan-doc family / Plan-doc safety / Plan template and
[`docs/plans/active/_plan-template.md`](../../../../docs/plans/active/_plan-template.md);
this skill drives them in one shot. It is the *authoring* counterpart to
[`grill-with-docs`](../grill-with-docs/SKILL.md) (the *stress-test*) — together
they bracket the plan lifecycle: author → grill → ship → revise (PR-only).

## When

- User says "write a plan" / "draft a plan" / "new plan" / "plan for X" / "plan this".
- Any non-trivial work that § Project rules requires a plan-doc for, before implementation starts.

## Steps

1. **Derive the slug.** Kebab-case, describes the work; the plan path is
   `docs/plans/active/<slug>.md`. The slug MUST equal the basename without `.md`
   (a doc-validation gate enforces this).
2. **Copy the template.** Start from `docs/plans/active/_plan-template.md` — never
   author from blank. Set the `> **Slug**:` line to the derived slug.
3. **Fill every section.** Context, Approach, Files to modify, Existing utilities
   reused, UX Pillar callouts, Perf gate, Risks / non-goals, Verification. A
   section that does not apply gets `N/A — <one-line reason>` — **never delete a
   section** (the empty section is the forcing function that catches a skipped
   concern).
   - **Three Verification/Out-of-scope items the template bakes in are MANDATORY —
     keep them in every plan** (they are the recurring plan-doc misses CodeRabbit
     flags via learned repo rules): (a) the **doc-validation** bullet enumerating
     the `test-docs.sh` suite; (b) the **`grill-with-docs` stress-test** bullet;
     (c) the **deferral residue-sweep** note under Out-of-scope. Fill them with
     the plan's specifics; never strip them.
4. **Perf-gate section is mandatory** when the planned diff would touch the
   project's perf-gated core source tree (the `project.config.json` `lint.zones`
   / `perf` paths; the plan template marks which) — assert the PR-fast / Pillar-2-scanner / dispatcher-drain /
   bucket-E / marker-inventory state per `AGENTS.md` § Project rules. If the diff
   is pure-docs / agentic-shell / CI-config, say so and mark the gates N/A.
5. **Commit immediately** — `git add docs/plans/active/<slug>.md` then
   `git commit -m "wip(plan): <slug>"`. This is **mandatory, not optional**:
   working-tree-only plan files are silently lost on the next checkout
   (plan-doc-safety). Do the commit BEFORE handing off to grill-with-docs.
6. **Stress-test — required before finalising.** § Plan-doc family requires a
   `grill-with-docs` pass before a plan is final — mandatory, not optional. Keep
   the `grill-with-docs` Verification bullet (step 3c) and run `grill-with-docs`
   to challenge the plan against the domain model + sharpen terminology before
   finalising.
7. **Post-ship is PR-only.** Note that § Implementation log / § Deviations /
   § Verification(actual) get filled after the work ships, via a PR (never a
   direct push to develop) — and the plan is `git mv`'d active → shipped once its
   post-ship sections are populated and all cited PRs are merged.

## Out of scope

- Stress-testing the plan (that's `grill-with-docs`).
- Implementing the plan (that's the orchestrator + the subsystem agents).
- Authoring a plan blank or skipping the immediate commit — both are failure modes
  this skill exists to prevent.
- Stripping the three mandatory Verification / Out-of-scope items (doc-validation
  suite, `grill-with-docs` stress-test, deferral residue-sweep) — they are baked
  into the template on purpose and are the recurring plan-doc misses.
