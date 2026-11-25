# Editing one field on many rows (bulk edit)

Select several rows in a ticket grid and set one field on all of them in a single step, for example reassigning five tickets or moving ten tickets to the same status. Each row is still saved, queued offline, audited and reported on its own, exactly as if you had edited that cell by hand.

## Quick start

1. **Select the rows.** Click the ID column, then Shift+click or Ctrl+click for more, or drag down the ID column. `Ctrl+A` selects every row in the view.
2. **Right-click a cell** in a selected row, in the column you want to change. A selection of one row shows the normal menu. With two or more selected rows the menu also shows **Set \<Field\> for N rows…**.
3. **Pick a value** in the dialog that opens, then press **Apply** (or `Enter` in the list).
4. **Read the summary toast**: how many rows were saved, queued, failed or skipped.

The selection survives the edit. When the grid re-sorts after the first save, the same tickets stay selected, so you can follow up with a second bulk edit straight away.

## Which fields

| Field kind | Examples | Picker |
|---|---|---|
| Single choice | Assignee, Priority, Status, single-select custom fields | One value from a searchable list |
| Multi choice | Components, Versions, multi-select custom fields | Tick-box list with **Replace / Add / Remove** |
| Labels | Labels | Tick-box list with **Replace / Add / Remove** |

Text, date, number, cascading and special columns are not offered yet; the menu item does not appear for them.

- **Replace** sets every row to exactly the ticked values.
- **Add** keeps each row's existing values and adds the ticked ones.
- **Remove** keeps each row's existing values and drops the ticked ones.

The dialog header shows what the rows hold now: `N rows · <value>` when they all agree, `N rows · mixed (k values)` when they do not.

### Status offers only the moves that are allowed

A status is changed by a workflow transition, not by writing a value, so not every status is reachable from every ticket. The status picker in a bulk edit lists only statuses that **every** selected row can move to right now, the same rule the status dropdown follows on a single ticket:

- Allowed moves are read live from the tracker when it is reachable. Until the answer arrives, the picker shows the workflow Smatchet has learned before, then the project's status catalog.
- A small cue above the list says where the options came from (live, learned, catalog) and how fresh they are, the same cue as the single-ticket status dropdown.
- A row that is already in the status you pick is skipped as *already has this value*, not treated as an error. When the same status has a different id in two projects' workflows, a row may instead attempt the move to its own status and the tracker decides.
- If the selected rows share no status they can all move to, the list says `(no status that every selected row can move to)`. Select fewer or more similar tickets, or change the status in smaller groups.
- With a very large selection the picker asks the tracker for one ticket per group of rows that share a project, issue type and status, and at most six requests at a time. The other rows use the workflow learned from that answer, and the cue says so until each row has its own live answer.
- In a split pane over two trackers, the transitions come from the focused pane's tracker. Bulk-edit the status from the pane you are working in.

If a transition is still refused at save time (a required field, a permission), that row fails with the tracker's message and the other rows are unaffected.

## Guardrails

- **Skipped rows.** Before anything is sent, every row is checked. A row is skipped, and listed with a reason, when the field is not editable on that ticket, when it already holds the value, when it is no longer in the view, or when its current value cannot be matched to the option list. Skipped rows are never sent.
- **Confirmation above 10 rows.** A selection of more than 10 rows shows a confirm step first: `Apply to N rows? K will be skipped.` with the value you chose. Nothing is sent until you confirm.
- **Read-only grid.** If the grid is read-only the dialog reports `Nothing was changed: the grid is read-only.` and sends nothing.
- **A disallowed value still fails per row.** The tracker's own message appears in the summary for that row; the rest of the job continues.

## Offline and failures

Bulk edit never bypasses the offline queue.

- Online, rows are saved as normal ticket edits.
- Offline, or when the tracker is unreachable, each row is **queued** and replayed when the connection returns. The toast counts these as *queued*, not *failed*.
- If someone changed a row's value after you selected it, a queued row that replays later raises the usual edit conflict instead of overwriting their change.
- Every row writes its own audit entry, visible in the backend audit trail.

Quitting the app while a job is still sending drops the rows not yet sent (a warning is logged). Save-or-queue results already reported are kept. This is tracked as GitHub Issue #2448.

## The summary toast

Titled `Bulk edit: <field>`, with the counts that apply, for example `12 saved, 2 queued, 1 failed, 3 skipped`. The first error message is added under the counts, cut to 160 characters. A one-shot `Saving N rows...` toast shows when the job starts; it does not count up. The toast is a success toast when everything saved, an info toast when some rows are queued, a warning when some failed and others saved, and an error when nothing could be saved.

## Command line, MCP and Lua

The same job runs from the command registry as `ticket.set_field_bulk`: at most 500 ids, `field`, `value` or `values`, and `mode` (`replace`, `add`, `remove`). It needs `--yes` because it writes, supports `--dry-run`, and returns a result for each row. Details and examples: [cli.md](cli.md#command-catalogue), [mcp.md](mcp.md) and [lua.md](lua.md). Status values go through the same transition rules; a row that cannot make the move reports `failed` with the tracker's message.

## Known limits

- No Undo / Revert for a finished bulk edit yet.
- No cancel button for a running job.
- Text, date and number fields are not bulk-editable yet.
- Rows are saved one after another, so a very large selection takes a while; the summary toast appears when the last row finishes.

Design and status: [plans/active/bulk-edit-rows.md](../plans/active/bulk-edit-rows.md).
