# Tracker

The subsystem that talks to issue trackers (Jira, Plane, GitHub) behind one backend-agnostic interface. Rules: [`AGENTS.md`](AGENTS.md). Orientation / request flow: [`README.md`](README.md). System-wide glossary: [`../../../../docs/CONTEXT.md`](../../../../docs/CONTEXT.md).

## Language

### Backend abstraction

**ITrackerBackend**:
The backend-agnostic facade for one active tracker, exposing six role interfaces; one live instance is held per active tracker and swapped on a tracker switch.
_Avoid_: ITrackerClient (deleted — see Flagged ambiguities), "the client", "the API"

**ITrackerIssueReader**:
The role that fetches issues — synchronously or streamed in batches — as `CachedTicket` rows.

**ITrackerConnectivity**:
The role that probes reachability (authenticated / auth-or-config-error / transport-down / service-unavailable), reports the tracker-type string, and lists/extracts projects.

**ITrackerFieldCatalog**:
The role that fetches a project's field catalog, per-issue edit metadata, and components.

**ITrackerIssueMutations**:
The role that builds create/update payloads and performs issue create, field update, attachment, and sprint-assignment writes.

**ITrackerCollaboration**:
The role that handles comments, watchers, votes, worklogs, and user search. Nullable — a backend that returns `nullptr` here (e.g. `PlaneClient` today) supports none of these; callers hide the features (UI gates on the null-check), never crash.

**ITrackerActivity**:
The role that produces a user's tracker activity feed and group membership (`TrackerActivityEntry` rows, group rosters). Nullable, separate from **ITrackerCollaboration** so a backend can support activity without claiming comments/watchers/votes ([ADR-0021](../../../../docs/adr/0021-itracker-activity-sixth-role.md); lands with the User Info Window plan).

**Role interface**:
One of the six capability-sliced interfaces (`ITrackerIssueReader`, `ITrackerConnectivity`, `ITrackerFieldCatalog`, `ITrackerIssueMutations`, `ITrackerCollaboration`, `ITrackerActivity`) that `ITrackerBackend` aggregates; the last four are nullable — a backend returns `nullptr` for a capability it doesn't support.

**Backend** (concrete):
A per-tracker implementation of the roles it supports — `JiraClient`, `PlaneClient`, or `GitHubClient`.
_Avoid_: "driver", "adapter"

**Fixture backend**:
A read-only backend (`GitHubFixtureBackend`, `PlaneFixtureBackend`) that loads canned JSON from disk instead of a live API; writes are logged no-ops.

**TrackerIssueKey**:
The canonical, opaque issue identifier carried across the backend virtuals, with a per-backend shape (Jira `PROJ-123`, Plane UUID, GitHub `owner/repo#N`); only the owning backend parses it.

### User & activity

**TrackerActivityEntry**:
One row of a user's tracker activity feed — timestamp, issue key, issue URL, summary, action label, details. Transient (in-memory), rebuilt per User-Info-Window open and dropped by `ClearUserActivity`; never persisted (it is **not** a `CachedTicket`). Sourced per-backend through **ITrackerActivity**, always scoped to one project/repo — never instance-wide (Jira: JQL `assignee was/reporter was` window + `project =` clause + changelog scan; Plane: issue history; GitHub: repo issue events); a `nullptr`-Activity backend yields an empty feed.

**Group roster**:
The in-memory map of group name → member names, loaded on demand from the active backend's **ITrackerActivity** role (`GroupMemberCache`); group membership lives on Activity, not **ITrackerCollaboration**. Transient — no SQLite, no schema. _Avoid_: "group catalog" (collides with **Field catalog**, which is the project field schema).

### Field model

**TrackerField**:
The schema of one editable field — id, name, backend type, normalized family, and allowed options.

**TrackerFieldFamily**:
The normalized kind of a field (Text, Number, Date, Labels, UserSingle/Multi, SelectSingle/Multi, CascadingSelect, Sprint, Status, IssueType, …) that abstracts over per-backend native type strings.
_Avoid_: "field type" (ambiguous — see Flagged ambiguities)

**TrackerFieldOption**:
One allowed value of a select/option field — id, value, and children for cascading selects.

**Field catalog**:
A project's full set of fields + components + issue-type metadata (`TrackerFieldCatalogResult`), fetched per project and persisted in `FieldCatalogCache`.

**Field payload**:
The backend-shaped JSON that a field's raw string values build into via `TrackerFieldPayload` — the write side of the field flow.

### Issue creation

**IssueDraft**:
The in-progress create-or-update payload — project, issue type, field values, staged attachments — before it reaches a backend.

**IssueCreatePipeline**:
The orchestration that validates an `IssueDraft`, builds its payload, POSTs/PUTs it, attaches files, and seeds the local cache with the new row.

**Set-replace**:
The `UpdateField` contract for multi-value fields (labels/assignees/components) — `values` is the intended **full set** after the edit, not a delta.

### Query

**JqlProjectScope**:
The helper set that reads or rewrites the `project = …` clause of a JQL string.

**Suggest engine**:
The per-query-language autocomplete (`JqlSuggestEngine` for Jira JQL, `PlaneQuerySuggestEngine` for Plane filters) producing a `QuerySuggestBuild`.

**GitHubQueryFromJql**:
The translator that maps a JQL query into GitHub search qualifiers, since GitHub has no JQL.

**Key clause**:
The per-language query clause naming explicit issue keys. Jira JQL (`key in (…)`) filters server-side; Plane (`key:PROJ-123`) and GitHub (`key = "owner/repo#N"`) filter client-side as a post-fetch step — the **only** clauses Plane evaluates locally (see Flagged ambiguities).

### Change monitoring

**Salient field**:
A tracker field whose change raises a ticket-change notification. The salient set is a fixed roster of canonical **roles** — Status, Assignee, Priority, Summary, DueDate, Sprint, Labels, Components — each resolved per-backend to its concrete field id(s). A change to a non-salient field (description, an arbitrary custom field, comment/watcher churn) does **not** notify.
_Avoid_: "watched field" / "tracked field" (collides with **Tracked set**); "canonicalId" (no such concept — salience is a role roster, not a field-id namespace).

**Tracked set**:
The set of issue keys a pane's change-monitor currently treats as in-view — the pane's in-memory `ActiveTickets` keys at poll time. Per-pane and transient: never persisted, because the local cache is keyed per-backend (not per-view) and so cannot supply it. A pane whose `ActiveTickets` was evicted has no tracked set and is not monitored.

**Since anchor**:
The per-pane timestamp marking the last successful change-poll; the monitor asks the backend for issues updated at/after (anchor minus one interval) so a skipped or slept poll never silently drops a change. In-memory and session-only; the first poll establishes the baseline silently (no backfill notifications).

## Relationships

- An **ITrackerBackend** aggregates exactly six **role interfaces**; `FieldCatalog`, `Mutations`, `Collaboration`, and `Activity` may be `nullptr` (capability unsupported).
- A concrete **Backend** (`JiraClient` / `PlaneClient` / `GitHubClient`) implements the roles it supports for one tracker; a **Fixture backend** stands in for a live one in tests.
- An **IssueDraft** flows through the **IssueCreatePipeline** → **ITrackerIssueMutations** → the backend's HTTP layer.
- **ITrackerIssueReader** returns **CachedTicket** rows (owned by Persistence, not Tracker).
- Every write enqueues through **OfflineQueueService** (owned by Sync) and emits a **BackendAuditTrail** pair (owned by Persistence).
- A **TrackerField**'s **TrackerFieldFamily** decides which **Field payload** builder runs; **set-replace** governs the multi-value ones.
- A pane's change-monitor diffs its **tracked set** each poll (bounded by the **since anchor**); only a change to a **salient field** raises a notification.

## Example dialogue

> **Dev:** "I'm adding label editing for GitHub — do I call `UpdateField` with just the new label?"
> **Domain expert:** "No. `UpdateField` is **set-replace** — pass the full intended label set. Jira and Plane are natively set-replace; `GitHubClient` reconciles internally (it pre-fetches the current labels and diffs), so from your side it's still one set-replace call on `ITrackerIssueMutations`."
> **Dev:** "And that `ITrackerIssueMutations` — that's part of the old `ITrackerClient`?"
> **Domain expert:** "There is no `ITrackerClient` anymore. It was split into `ITrackerBackend` plus the **role interfaces**; `ITrackerIssueMutations` is the write one."

## Flagged ambiguities

- **`ITrackerClient`** was the original monolithic backend interface. **Resolved:** deleted and split into **ITrackerBackend** + the **role interfaces** (five at the split; **ITrackerActivity** added later as the sixth). Never reintroduce the name — any surviving reference is stale.
- **`CachedTicket`** looks Tracker-owned because `ITrackerIssueReader` returns it, but it is **owned by Persistence** (the SQLite cache row). **Resolved:** Tracker produces/reads it; Persistence owns its schema and lifetime.
- **"field type"** was used for both the backend-native type string and the normalized kind. **Resolved:** the cross-backend abstraction is **TrackerFieldFamily**; "type" alone means the raw backend string only.
- **Plane filter mini-language** looks like a query language but has **no general evaluator** — clauses drive autocomplete and project-scope extraction only; the fetch returns the whole project. **Resolved:** only the **key clause** is evaluated (client-side post-filter); don't describe other Plane clauses as "filtering" anything.
