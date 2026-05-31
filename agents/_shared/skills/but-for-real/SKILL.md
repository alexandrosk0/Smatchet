---
name: but-for-real
description: Force a skeptical second pass on your own work before declaring completion. Use when the user invokes but-for-real, says "for real", "prove it", "double-check your work", "don't just say it should work", or when an agent has made non-trivial edits and needs an adversarial self-review with git diff, request matching, edge-case review, and actual verification.
---

# But For Real

Stop before claiming success. Do not say "done", "fixed", "updated", or "this should work" until the work has survived a hostile second pass.

Use this skill after making changes, before the final answer, or whenever the user demands proof instead of confidence. The tone can be blunt internally, but the output to the user must stay useful: evidence, findings, fixes, and any remaining risk.

## Required Pass

1. Re-read the user's actual request.
   - Identify the minimum thing they asked for.
   - Identify anything you added beyond that.
   - Remove unrelated improvements, speculative features, and drive-by refactors unless they are required for the requested outcome.

2. Run `git diff`.
   - Read every changed line.
   - Include staged and unstaged changes when relevant: `git diff --cached` and `git diff`.
   - For untracked files, inspect the full file.
   - Check that each hunk maps to the request or to necessary verification/supporting changes.

3. Review as if an adversarial reviewer wrote the comments.
   Look specifically for:
   - Logic that feels plausible but does not actually satisfy the requirement.
   - Empty, null, missing, duplicated, stale, malformed, or permission-denied states.
   - Off-by-one boundaries, inclusive/exclusive range mistakes, and first/last item bugs.
   - Async ordering, stale reads, cache invalidation, races, and partial failure paths.
   - Unused imports, variables, functions, files, unfinished markers, placeholders, debug output, and dead branches.
   - Copy-paste leftovers: wrong names, wrong labels, wrong config keys, wrong tests.
   - Type escapes such as broad `any`, unchecked casts, lossy conversions, or silently swallowed errors.
   - Hardcoded values that should come from existing config, constants, or local patterns.

4. Check integration with the surrounding system.
   - Confirm names, APIs, file locations, and conventions match the repo.
   - Confirm generated or metadata files are updated when the primary artifact requires them.
   - Confirm no unrelated user changes were reverted or overwritten.
   - Confirm the change respects project instructions, lint rules, and compatibility constraints.

5. Prove the change.
   - Run the most targeted meaningful verification available.
   - Prefer existing tests, validators, type checks, builds, linters, or app smoke checks over invented manual reasoning.
   - If a full verification is too expensive or impossible, run the strongest cheap check and explicitly name what was not run.
   - If verification fails, fix the issue and repeat the relevant pass. Do not explain away a failing check as "probably unrelated" without evidence.

## Response Contract

Only after the required pass, answer with:

- What changed, briefly.
- What proof was run, with exact command names or validation method.
- Any remaining risk or skipped verification.

Do not use "should work" as proof. Say "validated by X" or "not validated because Y".

## If The Pass Finds A Problem

Fix it before finalizing when it is in scope and safe. Then re-run the targeted diff review and verification.

If the problem is out of scope, dangerous to change, or requires user judgment, say so plainly and include the exact reason. Do not bury it under a success summary.

## Git Diff Checklist

For each changed file, ask:

- Why is this file changed?
- Is every changed line necessary?
- Does this preserve existing behavior outside the request?
- Is the failure path handled?
- Is there a test or validation signal covering the change?
- Would I be comfortable with this diff being reviewed line-by-line in public?

If any answer is weak, keep working.
