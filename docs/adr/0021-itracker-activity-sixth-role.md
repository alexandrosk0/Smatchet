# 21. ITrackerActivity as a sixth role interface

Date: 2026-06-10

## Status

Accepted (plan-stage; lands with `docs/plans/active/user-info-window.md` slice 3)

## Decision

User-activity feeds and group membership get their own nullable role interface, `ITrackerActivity` (`ITrackerBackend::Activity()`), instead of living on `ITrackerCollaboration`. A backend that supports activity but not comments/watchers/votes (Plane, GitHub) returns a non-null `Activity()` and a null `Collaboration()`.

## Considered options

- **Keep activity methods on `ITrackerCollaboration`** (where the pre-revision plan put them). Rejected: UI gates comment/watcher/vote/worklog feature visibility on `Collaboration() != nullptr`. Making Plane/GitHub return non-null to expose activity would un-hide those features, and every call would then hit the default-impl "not supported by this backend" virtuals at runtime — visible broken UI instead of hidden unsupported UI.
- **New narrow role (chosen).** Capability slicing stays honest: each nullable role answers exactly one "does this backend support X?" question. Cost is one more virtual accessor and a sixth interface header.

## Consequences

- `ITrackerBackend` aggregates six roles; the last four (`FieldCatalog`, `Mutations`, `Collaboration`, `Activity`) are nullable.
- `TrackerActivityEntry` / `TrackerActivityProgress` and the fetch/clear virtuals move to `ITrackerActivity.h`; a `nullptr`-Activity backend yields an empty User Info Window feed.
- Docs updated alongside: `Source/Core/src/Tracker/CONTEXT.md` (six roles), `Source/Core/src/Tracker/AGENTS.md` invariant list (five → six) when the code lands.
