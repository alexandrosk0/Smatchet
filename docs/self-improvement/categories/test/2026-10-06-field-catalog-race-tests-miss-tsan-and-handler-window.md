- 2026-10-06 · orchestrator · [test] · P3 — Field-catalog race tests never run under TSan, and the failure handler's lock re-checks have no injection seam

  `tests/Core/AppControllerFieldCatalog.posix.test.cpp` drives the real `AppController` catalog paths
  (refresh supersede, the grid's latched catalog apply, pane kind and site matching). It links only into
  `SmatchetCommandsTests`, because only the posix-core-check archive provides a headless `AppController`.
  `SmatchetTsanTests` runs other catalog cases, but none of these, so the TSan lane never exercises
  `RefreshFieldCatalog` racing `SetFieldCatalog`.

  Separately, `HandleFieldCatalogErrorInto` (`Source/Core/src/AppController_CatalogAndFieldEdit.cpp`) loads
  the offline snapshot unlocked, then re-checks under the catalog mutex:
  - the write guard;
  - whether the catalog is still empty;
  - the banner decision on the live catalog.

  No test can land a write inside that window. The "superseded failure" case is caught earlier by the
  caller's own pre-check, so reverting the in-handler checks leaves every test green (adversarial review
  of the backlog sweep, round 4).

  The grid's own catalog apply now runs through `AppController::ApplyGridFieldCatalogResult` (#2359), which
  the posix test drives directly: the latched pane, the fetch's configuration and project, and an offline
  failure after a clear restoring that project's snapshot. Only the UI glue around it (`SmatchetUI.cpp`'s
  `fieldCatalogFuture` handling and `StartFieldCatalogFetchAsync`'s worker) stays unlinked. The old two-call
  path (`SetCurrentCatalogProject` then `SetFieldCatalog`) is gone.

  Concrete next actions:
  1. Give the TSan rig a headless `AppController` (link the posix-core-check archive, or a reduced one) and
     add `AppControllerFieldCatalog.posix.test.cpp` to it.
  2. Done (#2359): the grid's result apply is `AppController::ApplyGridFieldCatalogResult`, and
     `AppControllerFieldCatalog.posix.test.cpp` tests clear, then a failed result for FOO, then FOO's
     snapshot restored.
  3. Add a snapshot-load seam, for example a `FieldCatalogCache` loader hook that tests can override, so a
     test can apply a catalog during the unlocked load and assert the restore and banner skip it.

  Status: open
  Last-reviewed: 2026-10-21
