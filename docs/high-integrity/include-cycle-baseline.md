# Include-cycle — grandfathered baseline

_Auto-generated. Do not hand-edit; run `bash agents/scripts/project/test-lint-rules.sh --include-cycle-baseline` and commit._
_The gate is a live merge-base delta vs `origin/develop` (include_cycle_audit.py --diff); this file is the informational ratchet snapshot — each later phase of `core-include-dag` deletes the line for the edge it kills._

## include-cycle (5 grandfathered edges)
- `AppController.h:47` -> `Ui/CancelToken.h` — back-edge (`layer 5 -> 6 (AppController.h -> Ui/CancelToken.h)`)
- `Commands/MainThreadDispatch.h:30` -> `AppController.h` — back-edge (`layer 4 -> 5 (Commands/MainThreadDispatch.h -> AppController.h)`)
- `Config/ConfigManager.h:27` -> `Ui/SmatchetUiModeIds.h` — back-edge (`layer 1 -> 6 (Config/ConfigManager.h -> Ui/SmatchetUiModeIds.h)`)
- `Sync/OfflineQueueService.h:32` -> `AppController.h` — back-edge (`layer 2 -> 5 (Sync/OfflineQueueService.h -> AppController.h)`)
- `Sync/TicketSyncService.h:20` -> `AppController.h` — back-edge (`layer 2 -> 5 (Sync/TicketSyncService.h -> AppController.h)`)

## Totals
- violating edges grandfathered: 5
