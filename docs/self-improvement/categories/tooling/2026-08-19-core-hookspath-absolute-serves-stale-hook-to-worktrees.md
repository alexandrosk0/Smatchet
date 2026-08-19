# `core.hooksPath` is set to an absolute path into the main checkout, so every worktree runs whatever revision of a hook `main` happens to have checked out

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-19
- **Found during**: verifying the stage-(B) refspec-scoping fix end-to-end ([PR #2131](https://github.com/alexandrosk0/Smatchet/pull/2131), `docs/plans/shipped/pre-push-refspec-scope.md`)

## Symptom

The fix that teaches `scripts/git-hooks/pre-push` stage (B) to stop refusing `refs/locks/*`
deletes was merged to `develop` (`56f5be77`). The live proof was the routine
`lock-release.sh` push from the just-merged checkout:

```
git push origin :refs/locks/pre-push-refspec-scope
```

It printed the full refusal banner the fix was supposed to remove:

```
pre-push: REFUSING push. … PR #2131 state: MERGED
```

Nothing was wrong with the fix. The hook that ran was not the fixed one.

## Cause

`core.hooksPath` in this clone is an **absolute** path into the **main** checkout:

```
$ git config --show-origin --get-all core.hooksPath
file:C:/Dev/Smatchet/.git/config                                       C:\Dev\Smatchet\scripts\git-hooks
file:C:/Dev/Smatchet/.git/worktrees/<id>/config.worktree               C:\Dev\Smatchet\scripts\git-hooks
```

So the hook body a worktree executes is a function of **whatever branch the main checkout
is parked on** — not of the worktree's own tree, not of `origin/develop`. Main was sitting
on `claude/peaceful-faraday-6jm1w5`, which predates the fix:

```
$ grep -c branch_content_push C:/Dev/Smatchet/scripts/git-hooks/pre-push   # installed
0
$ grep -c branch_content_push scripts/git-hooks/pre-push                   # worktree + origin/develop
4
```

The path was **not** set by the installer.
[`setup-harness.sh` `install_git_hooks()`](../../../../agents/scripts/core/setup-harness.sh)
sets the **relative** `target="scripts/git-hooks"`, and its third branch explicitly refuses
to trample a differing value (`WARNING: core.hooksPath is '…' … Skipping.`). The absolute
value was introduced out-of-band and is now sticky precisely because the installer will not
overwrite it.

The relative form does not have this defect. Git chdirs to the **worktree root** before
running a hook, so a relative `core.hooksPath` resolves per-worktree. Verified directly —
marker hook placed only in the worktree, invoked from a subdirectory, with no such directory
in the main checkout:

```
$ (cd <worktree>/docs && GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.hooksPath \
     GIT_CONFIG_VALUE_0=tmp-hooktest git hook run pre-commit)
MARKER: worktree copy ran
$ ls C:/Dev/Smatchet/tmp-hooktest
ls: cannot access 'C:/Dev/Smatchet/tmp-hooktest': No such file or directory
```

Working around it per-invocation needs the env form, because `git -c core.hooksPath=…` does
not propagate into the git calls the hook itself makes:

```
GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.hooksPath \
GIT_CONFIG_VALUE_0=<worktree>/scripts/git-hooks git push …
```

## Why it matters

Three distinct failures, none of which announce themselves:

1. **A hook fix is not live for any session until `main`'s branch carries it.** Merging to
   `develop` is not enough; the main checkout has to be moved. The repo's own concurrency
   rule (§ Concurrent sessions — one worktree per session, never `checkout` in a shared
   tree) means main is routinely parked on some unrelated agent branch for days.
2. **It fails as a false negative on the fix, not as a config error.** The banner is the
   hook's own correct output for the code it was running. The first and most natural reading
   is "the fix does not work" — the actual diagnosis needs someone to think of grepping the
   installed file, which is at a path nothing in the diff mentions.
3. **It runs the wrong gate in the other direction too.** If main is parked on a branch with
   a *stricter* or *broken* hook, every concurrent worktree inherits it, and vice versa — a
   worktree can push past a guard `develop` currently enforces.

## Proposed fix

1. **Normalise the value to the relative form** (~2 min, plus a decision on how to roll it
   out to existing clones): `git config --local core.hooksPath scripts/git-hooks`, and the
   same for any `config.worktree`. Empirically verified above to resolve per-worktree.
2. **Teach `install_git_hooks()` to repair an absolute path that points at this repo's own
   `scripts/git-hooks`.** Today that value lands in the "custom path, skip" branch, which is
   right for a genuinely foreign path and wrong for this one — it is the same directory,
   spelled in the form that breaks worktrees. Rewrite it to the relative form and say so;
   keep the WARN-and-skip for anything else.
3. **Add a `doctor.sh` check** (`scripts/dev/doctor.sh`) asserting `core.hooksPath` is
   relative — in both the shared local config and any `config.worktree`. This is the class of
   defect that only ever surfaces mid-incident, so it wants a check that runs when nothing is
   wrong.
4. **Say it in `docs/agent-rules/process-rules.md` § Concurrent interactive sessions.** That
   section already owns the "worktrees share one `.git`, so X leaks across sessions" hazards;
   "the hook you run comes from main's branch, not yours" is the same shape and is missing.
