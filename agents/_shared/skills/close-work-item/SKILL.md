---
name: close-work-item
version: 1
description: Close a work item — drain Spawned.md into the canonical ledgers, collapse docs/work/items/NN-slug/ into the single frozen docs/work/closed/NN-slug.md summary, retire resolved ledger entries, and verify the collapse with work_item_lint.py plus the judgment probes the linter can't make. Use when the user says "close the item", "collapse NN-slug", or after the post-implementation review + retro of an item are done.
---

# Close a work item

At close (after the retro), the item's `docs/work/items/NN-slug/` folder collapses into a single
**`docs/work/closed/NN-slug.md`** file — the staged files (`1-specification.md`, `2-design.md`,
`3-plan.md`, `4-review-*.md`, `5-review-*.md`, `{4|5}-resolution-*.md`, `Spawned.md`, and `QA.md`
where the item had one) and their folder are deleted; git keeps the full history, so nothing is
lost. The summary is the durable record, frozen like the staged artifacts were. Ported from
Whip-Process `Procedures/ClosingItem.md` + the judgment half of `Procedures/ClosingReview.md`
(contract wording carried verbatim); the mechanical half is `work_item_lint.py`. Ledger lifecycle:
[work-items.md § Tracking](../../../../docs/agent-rules/work-items.md).

The summary opens with `> Status: Closed (YYYY-MM-DD)` and has exactly three lean sections:

- **Shipped** — what was actually built, as-shipped: the modules/types/files that resulted.
- **Key decisions & reasons** — the durable rationale a future reader needs, and the anchor living
  docs cite into.
- **Spawned work** — the `DEF/IDEA/BL-NN` items (and bug issues) this item spawned, one line each;
  the ledgers are canonical.

## Do exactly this

1. **Diff the close against the binding dispositions.** The `{4|5}-resolution-*.md` files are the
   checklist: every `fix` landed, every `defer` is in `Spawned.md`, every round resolved — a close
   over an unresolved round or an unlanded fix is not ready to run.

2. **Drain `Spawned.md`** into the canonical ledgers first — assign each captured entry its
   `DEF/IDEA/BL-NN` (the next free number in the matching ledger under `docs/work/`); a captured bug
   becomes a **GitHub Issue**, not a ledger line. The summary's *Spawned work* section then lists
   them with those IDs.

3. **Clean up the tracking ledgers** (work-items.md § Tracking → *Retire at close*): **remove**
   every `DEF-NN`/`BL-NN` this item resolved — the one it took up, plus any it implemented or
   dropped — repointing any surviving cross-reference to the resolving item. No `Done`/`Dropped`
   tombstones. To cite a now-retired id anywhere, use one of exactly two forms: a closed-item link
   (`[DEF-NNN](<path to docs/work/closed/NN-slug.md>)`) or `DEF-NNN (retired)`.

4. **Drain `QA.md` if the item has one** (a plan that carried no `§QA` produced no sheet — nothing
   to drain). This project has **no standing manual-QA checklist yet** (work-items.md flags the
   drain target as not invented), so `(durable)` lines currently have nowhere to drain: carry them
   into the summary's *Key decisions & reasons* as the item's manual-verification residue, or defer
   a standing-checklist port via `Spawned.md` if the durable set is substantial. One-off lines die
   with the folder. If a step's home looks wrong at close, that is a defect in the sheet to report,
   not to silently re-home.

5. **Write the summary, delete the folder, commit.** The collapse must **lose nothing durable**:
   every key decision/rationale (from `2-design.md`), and every review conclusion a future reader
   needs — the **binding disposition** for each finding, from the `{4|5}-resolution-*.md` rather
   than the reviewer's *suggested* disposition in the review files — carries into the summary's
   sections. Skip the archeology — addressed review nits, iteration churn, restating the process.
   Where a living doc cited a staged file (e.g. `2-design.md §7`), repoint it at the relevant
   `docs/work/closed/NN-slug.md` section so no reference dangles. A closed summary is **frozen at
   close** — anything further starts a new numbered folder.

6. **Verify the collapse — mechanical, until it prints PASS:**

   ```bash
   python agents/scripts/core/work_item_lint.py --item NN-slug
   python agents/scripts/core/work_item_lint.py --citations
   bash scripts/dev/test-docs.sh
   ```

   `--item` enforces the deterministic collapse invariants (folder deleted, single summary, no
   orphaned staged file, the `Status: Closed` line, the exact section set, resolving links); it
   reads the deleted per-item files from git, so **commit the collapse before linting** (or pass
   `--ref`). `--citations` catches the id a retirement left dangling — a close that retires ids is
   exactly when one goes dangling.

7. **Verify the judgment calls the linter can't make** (the ported closing-review probes — report
   only a probe that fires, then fix before finishing):

   - **Faithful collapse** — the summary faithfully represents the item: every durable
     decision/rationale and binding review disposition carried over, nothing durable dropped, no
     archeology or bloat added. `git show HEAD~1:<path>` compares what landed against what it
     replaced.
   - **QA drain** — *where the item had a sheet*: the `(durable)` / one-off calls were right (steps
     that guard shipped behaviour preserved, one-off steps not), and each sheet scenario was a
     sensible home for its steps.
