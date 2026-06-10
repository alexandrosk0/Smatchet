# Persistence

The subsystem that owns Smatchet's per-user on-disk state — the local cache of tracker issues, the offline write queue, and the audit trail. Rules: [`AGENTS.md`](AGENTS.md). System-wide glossary: [`../../../../docs/CONTEXT.md`](../../../../docs/CONTEXT.md).

## Language

**Local cache**:
The whole per-user on-disk store — cached tickets, the pending-write queues, meta flags, and AI chat history — owned by one `LocalCacheManager` per app.
_Avoid_: "the database", "the SQLite" (substrate, not the concept)

**Sync cache** (`ISyncCache`):
The slice of the local cache that the sync/replay machinery reads and writes — cached tickets, pending writes, dead letters, and meta flags; chat and schema migration are NOT part of it.
_Avoid_: ILocalCache (the renamed working title — it overclaimed the whole local cache), "the cache interface"

**Cached ticket**:
The local copy of one tracker issue's field values, namespaced by backend key.

**Backend key**:
The namespace string that partitions cached tickets per tracker backend, so a Jira row can never serve a GitHub view.
_Avoid_: "tracker type" (a config concept; the key is the cache-side namespace)

**Pending write**:
A create or field edit queued while offline (or after a transport failure), awaiting replay against the live backend. Two kinds: **pending create** and **pending field edit**.
_Avoid_: "offline edit" (ambiguous with the UI editing mode)

**Replay**:
Draining a pending write through the same live pipeline a connected write uses; replay order is FIFO by enqueue order.

**Dead letter**:
A pending write archived after replay gave up on it; restorable back into the queue by user action, listed newest-first.
_Avoid_: "failed edit" (a dead letter is parked, not lost)

**Meta flag**:
An idempotent, one-way named marker on the local cache (set-once, never unset) used to remember that a one-time transition already happened.

## Relationships

- The **local cache** contains the **sync cache** plus chat history and migration state.
- A **cached ticket** belongs to exactly one **backend key** namespace.
- A **pending write** either completes **replay** (deleted), or becomes a **dead letter** (archived, restorable).
- The Sync subsystem consumes the **sync cache**; nothing outside Persistence touches the rest of the **local cache** except through `LocalCacheManager`.

## Example dialogue

> **Dev:** "After the user switches from Jira to GitHub, does replay write the queued Jira edit into the GitHub view?"
> **Domain expert:** "No — the **pending write** replays under the **backend key** it was queued against, so the resulting **cached ticket** lands in the Jira namespace. And if replay keeps failing it becomes a **dead letter**, not a silent drop."

## Flagged ambiguities

- "ILocalCache" was the working name for the service-facing cache interface — resolved: the interface is the **sync cache** (`ISyncCache`); **local cache** stays the name of the whole store. An interface to the whole store does not exist and should not be implied.
