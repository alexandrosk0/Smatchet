# Offline field-edit conflict detection: 2-way display-value compare, and a based edit never dies silently

**Status:** accepted (2026-06-04)

The offline-queue replay loop detects concurrent server changes ("someone changed the value in between") for **all** field edits, not just rich text. Before this decision only rich-text fields captured a base and ran a 3-way text merge; scalar fields (status, assignee, priority, labels, dates) blind-overwrote the server on replay, and even the rich-text path silently overwrote when its server re-fetch failed.

Four decisions define the new behaviour:

1. **Scalar conflict detection is 2-way on display values.** A scalar edit captures `base = CachedTicket::GetFieldValue(fieldId)` at edit-open time (the display string), persisted in an additive `pending_field_edits.original_value` column. At replay, `theirs = fresh.GetFieldValue(fieldId)`. If `base != theirs` (after a whitespace trim), the server moved since the user looked → suspend and ask. There is no `mine` term in the *detection* decision — `mine` is shown in the resolution modal for the user's benefit only.
2. **Detection covers all scalar fields with a captured base.** No field-type allowlist.
3. **A field edit that captured a base never dies silently.** On a pre-replay fetch failure: a *transient* (transport) error retries on the next tick, and when it would reach the attempt cap it routes to an `unverified` conflict **instead of** the dead-letter; a *permanent* error (404/403) raises the `unverified` conflict immediately. The edit either replays or asks — it is never silently overwritten *and* never silently dead-lettered.
4. **"Discard my edit" hard-deletes the queue row** and writes a `BackendAuditTrail` entry. The dead-letter bucket keeps meaning "failed replay," not "user threw it away."

The conflict is surfaced asynchronously in the existing Offline Queue UI (record `has_merge_conflict` + a kind-tagged `conflict_context_json`, suspend the row) — never a blocking modal mid-replay, which is correct because replay runs off the UI thread on reconnect.

## Considered options

- **3-way scalar compare `(base, mine, theirs)`** — *Rejected for v1*. The queued `fields_payload_json` is in backend format (account ids, transition ids, ADF), while `theirs` is the `GetFieldValue` display string; comparing them is apples-to-oranges and false-conflicts nearly every scalar. A 3-way that also auto-skips when the server already equals the user's intended value needs `mine` in display form (sourced from the locally-applied cache value), which carries a multi-queued-edits ordering caveat. The only win is suppressing the rare "both sides set the same value" no-op conflict — not worth the complexity given the locked "prefer ask over silent overwrite" stance.
- **Field-type allowlist** (detect only "stable" fields, skip labels/dates) — *Rejected*. Reintroduces exactly the silent-overwrite gap the change closes. Because `base` and `theirs` share one serializer, real-world false positives are rare; the residual (server re-orders a multi-value set) is arguably a real change anyway.
- **Immediately raise `unverified` conflict on any fetch failure** — *Rejected*. A transient network blip would pop a user-facing conflict; noisy.
- **Skip + retry on fetch failure, dead-letter at cap** (no conflict) — *Rejected*. Dead field-edits have no restore path, so a capped-out edit would be lost unrecoverably with no user decision.
- **Discard → dead-letter tombstone** (reason `user_discarded`) — *Rejected*. Conflates a deliberate discard with the failure bucket; the audit-log append already provides traceability without dead-list clutter.
- **HTTP-layer optimistic concurrency (If-Match / etag / version)** — *Out of scope*. Confirmed absent today across `Source/Core/src/Tracker`. A true server-version guard is a separate, larger effort; this decision stays at the offline-queue base-vs-theirs level.

## Consequences

- **Additive schema**: one new `pending_field_edits.original_value TEXT` column (+ dead-table twin), following the existing `original_rich_value` migration pattern. No destructive migration (Persistence invariant).
- **Capture point**: scalar base is set once where `PendingFieldEdit` is built with a ticket in scope (`TicketFieldEditor.cpp`); rich base capture (`OriginalRichValue`) is unchanged. Single choke point per kind.
- **Replay branch**: `ReplayOneFieldEdit` dispatches rich (existing 3-way text merge) vs scalar (new `ServerMovedFromBase` 2-way); the fetch-failure branch is unified under decision 3 for both kinds. The pure decision helper lives in a banned-include-free `OfflineFieldConflictPolicy.h` (sibling of `OfflineQueueReplayPolicy.h`) so `test-rig` exercises it without the SQLite/cpr cascade.
- **Conflict context** gains a top-level `kind` discriminator (`text` | `scalar` | `unverified`); rows written before this change carry no `kind` and default to `text` so the existing rich-text modal path stays byte-identical. This `kind` is **orthogonal to** the existing per-field `richKind` key (`adf`|`html`, written at `OfflineQueueService.cpp:788`): `kind` selects the conflict *category*, while `richKind` is **unchanged** and keeps driving the rich-branch markdown→ADF/HTML reconversion at resolve. New rich conflicts write `kind:"text"` alongside the existing `richKind`.
- **Resolution UI**: `DrawOfflineConflictModal` branches on `kind` — `text` unchanged; `scalar` shows mine/theirs with Use Mine / Use Theirs / editable value; `unverified` shows Force Mine / Discard. Resolve NULLs **both** `original_rich_value` and `original_value` so the next replay performs the now-consented overwrite.
- **Known residue**: legacy queued rows with no captured base still last-write-wins on replay — we cannot ask about a divergence we have no reference for. No backfill; the next edit captures a base naturally.
- Full design + verification buckets: `docs/plans/shipped/offline-conflict-ask-all-fields.md`.
