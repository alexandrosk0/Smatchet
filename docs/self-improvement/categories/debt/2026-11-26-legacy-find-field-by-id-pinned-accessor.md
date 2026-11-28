- 2026-11-26 · legacy-find-field-by-id-pinned-accessor · [debt] · P3 — `AppController::FindFieldById` survives as a per-thread pinned raw-pointer accessor for callers in four files

Details:
The fix for #2501 moved every caller that goes through `IAppFields`, `IAppTicketMutations` or `IEditMetaDeps` to
`FindFieldSnapshotById`, which returns a handle that shares the lifetime of the catalog generation the field lives
in. The raw-pointer `AppController::FindFieldById` could not be removed in the same change: two of its callers sit in
files another branch held a plan-lock on (`Source/Core/src/Ui/SmatchetGridUiSupport.cpp`,
`tests/ui/grid_spreadsheet_keys.test.cpp`). It stays as a non-virtual accessor backed by a `thread_local` pin
(`Source/Core/src/AppController_CatalogAndFieldEdit.cpp`), so the pointer is valid until the calling thread's next
call. That guarantee is a documented rule, not something the type enforces, and the pin keeps one superseded
generation alive per calling thread. Two more test files were left on it to keep the change small:
`tests/ui/bulk_field_edit.test.cpp` and `tests/ui/offline_first.test.cpp`. Every remaining caller reads or copies
the field at once.

Concrete next action:
When the `input-ux-w3-grid-s4` lock lifts, move the callers in those four files to `FindFieldSnapshotById`, delete
`AppController::FindFieldById` and its pin, and drop the legacy-pin case from
`tests/Core/AppControllerFieldCatalog.posix.test.cpp`.

Status: open
Last-reviewed: 2026-11-26
