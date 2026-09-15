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
| **Parent group** | Reorder the grid so every issue sits directly under its parent, nested by depth. The regular column sort still decides the order *among* siblings and among top-level rows. |
| **Hide parent stories** | Drop every row that is the parent of another row, leaving only the leaf tasks/bugs, drawn flat: no indent and no parent tint even when *Parent group* is also on. Handy for a personal "what do I actually work on" view. |

Flipping either toggle marks the view as changed, so the *Unsaved layout
changes* strip appears and you can **Save**, **Save as new...** or **Discard**
exactly like a column or sort edit. In `smatchet_views.json` the two switches
are stored as `story_group_sort` and `hide_parents` on the view.

## What the grid shows

- **Indent**: with *Parent group* on, the Id cell is indented 20 px per nesting
  level, so a sub-task under a task under a story is indented twice.
- **Parent tint**: a row that is the parent of another visible row gets a
  lavender background. A parent that is itself nested gets a half-strength
  tint so the tree reads top-down. A status colour, when the view assigns one,
  wins over the parent tint.
- **Parent group + quick filter**: when you type into the quick filter, the
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
already present, and asks the tracker for the rest through the backend-agnostic
issue reader interface. If those parents reference parents of their own,
Smatchet chases them too — one request per hop, in the same sync — until a
hop turns up nothing new or 16 hops have run. A chain deeper than that
(unusual outside cyclic or malformed data) resolves the remaining levels on
the next sync instead of stalling this one.

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

## How missing children are loaded

The parent fetch above only reaches upward: a view like `assignee =
currentUser()` gets ancestors of what it already matched, never a ticket's
descendants. A narrow view — `key = EPIC-1`, say — streams only the epic
itself, so without a separate fetch the tree would show a single row no
matter how large the epic's story/task/subtask tree really is.

Smatchet closes that gap the same way, mirrored: after the parent fetch,
every ticket the view's own query actually streamed becomes a candidate
parent, and Smatchet asks the tracker for its children. Each fetched child
becomes the next hop's candidate parent in turn — child, grandchild,
great-grandchild — until a hop comes back empty or 16 hops have run. Unlike
the upward direction, confirming "no more children" always costs one request
per branch: there is no local field that says a ticket has no children the
way a ticket's own `parent` field says who its ancestor is, so a leaf ticket
still needs one (empty) round-trip to rule out further descendants.

- Fetched children are cached in SQLite like any other issue and take part in
  the tree, the tint and the indent, same as a fetched parent.
- **Not gated on Hide parent stories** — unlike the parent fetch. That toggle
  drops rows that are themselves a parent of a visible row; the descendants
  this fetch supplies are exactly what *survives* the filter, so skipping the
  fetch there would defeat the toggle (an epic-only view with Hide parent
  stories on would show nothing) rather than save pointless work.
- Governed by the same **Load parent issues** preference as the parent fetch
  — turning it off skips both directions.
- A failed children fetch never fails the sync, same as a failed parent
  fetch: a warning such as `children of 2 issue(s) could not be loaded:
  <detail>` is appended to the sync summary and the streamed rows still land.

## Backend support

The hierarchy works for any backend whose issue mapping emits a `parent`
field:

| Backend | Parent source | Children query |
|---|---|---|
| Jira | The `parent` field. When it is absent (classic projects, some issue types) the inward **is part of** issue link is used as a fallback. | JQL `parent in (...)`. |
| Plane | The `parent` field. | Not implemented — a Plane view only ever gets ancestors, never descendants, today. |
| GitHub Issues, Linear | No parent field is mapped today, so the toggles have no effect on those backends. | Not implemented. |

## Related

- [Keyboard shortcuts](keyboard-shortcuts.md) for the view-apply and view-create
  shortcuts used alongside the Sort By popup.
- [CLI Guide](cli.md) for scripting view changes.
