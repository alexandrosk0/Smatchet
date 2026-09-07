<!-- tier: project -->
# Parent issue hierarchy

Smatchet can show a view's issues as a tree instead of a flat list: child
tasks and bugs are grouped under their parent story, nested rows are indented,
and parent rows carry a distinct background tint. Parents that the view's
filter did not return are fetched automatically so the tree is never missing
its roots. This page covers what the feature does, how to turn it on, and what
to expect from each tracker backend.

## Turning it on

Both switches live in the grid header's **Sort By ↕** popup and are saved with
the active view (per-backend, alongside the view's filter, columns and sort):

| Toggle | Effect |
|---|---|
| **Story group** | Reorder the grid so every issue sits directly under its parent, nested by depth. The regular column sort still decides the order *among* siblings and among top-level rows. |
| **Hide parent stories** | Drop every row that is the parent of another row, leaving only the leaf tasks/bugs, drawn flat: no indent and no parent tint even when *Story group* is also on. Handy for a personal "what do I actually work on" view. |

Flipping either toggle marks the view as changed, so the *Unsaved layout
changes* strip appears and you can **Save**, **Save as new...** or **Discard**
exactly like a column or sort edit. In `smatchet_views.json` the two switches
are stored as `story_group_sort` and `hide_parents` on the view.

## What the grid shows

- **Indent**: with *Story group* on, the Id cell is indented 20 px per nesting
  level, so a sub-task under a task under a story is indented twice.
- **Parent tint**: a row that is the parent of another visible row gets a
  lavender background. A parent that is itself nested gets a half-strength
  tint so the tree reads top-down. A status colour, when the view assigns one,
  wins over the parent tint.
- **Story group + quick filter**: when you type into the quick filter, the
  ancestors of every matching row are put back into the grid so the match
  keeps its context. With *Hide parent stories* on they stay hidden: the
  leaf-only list is meant to be flat.
- **Hide parent stories** resets the indent and the tint: the surviving leaf
  rows keep their story-group order but draw like a plain list.
- Nesting depth is capped at 64 levels; a parent cycle in the tracker data
  (A's parent is B, B's parent is A) is broken at that cap instead of hanging.

## How missing parents are loaded

A view's query usually returns children without their parents (for example a
JQL filter on `assignee = currentUser()`). After every sync batch Smatchet
collects the parent keys referenced by the fetched issues, subtracts the ones
already present, and asks the tracker for the rest in one request through the
backend-agnostic issue reader interface. Only one level is fetched per sync
(FS parity): grandparents appear on the next batch if the newly fetched
parents reference them.

- Fetched parents are cached in SQLite like any other issue and take part in
  the tree, the tint and the indent.
- When the view has *Hide parent stories* on, the extra fetch is skipped
  entirely: the rows would be dropped anyway.
- **Load parent issues** (Preferences → Editing → Grid behaviour, on by
  default; `load_parent_issues` in the config file, `config.set
  loadParentIssues` from the CLI) turns the extra fetch off for every view.
  Children of a missing parent then show as top-level rows. Takes effect on
  the next sync.
- A failed parent fetch never fails the sync. The grid keeps the children and
  the sync summary carries a warning such as
  `3 parent issue(s) could not be loaded: <detail>`.

## Backend support

The hierarchy works for any backend whose issue mapping emits a `parent`
field:

| Backend | Parent source |
|---|---|
| Jira | The `parent` field. When it is absent (classic projects, some issue types) the inward **is part of** issue link is used as a fallback. |
| Plane | The `parent` field. |
| GitHub Issues, Linear | No parent field is mapped today, so the toggles have no effect on those backends. |

## Related

- [Keyboard shortcuts](keyboard-shortcuts.md) for the view-apply and view-create
  shortcuts used alongside the Sort By popup.
- [CLI Guide](cli.md) for scripting view changes.
