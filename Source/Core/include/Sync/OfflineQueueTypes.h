#ifndef SMATCHET_SYNC_OFFLINE_QUEUE_TYPES_H
#define SMATCHET_SYNC_OFFLINE_QUEUE_TYPES_H

// Leaf home for the six offline-queue summary result structs shared by
// Sync/OfflineQueueService.h and AppController.h. Relocated out of AppController.h
// (where they were nested in `class AppController`) to TOP-LEVEL (global namespace)
// so the Sync layer can name them WITHOUT including AppController.h — severing the
// Sync -> AppController include back-edge (core-include-dag Phase 3).
//
// AppController re-exports each as `using DeadLetterRestoreSummary = ::DeadLetterRestoreSummary;`
// so its ~113 includers that reference `AppController::DeadLetterRestoreSummary` see an
// identical name (the structs were `AppController::*` nested types before this move).
// All six are trivial PODs (two ints) — dependency-free beyond the language.

/** Result of moving dead-letter create rows back to the active offline queue. */
struct DeadLetterRestoreSummary {
    int Restored = 0;
    int Failed = 0;
};

/** Result of permanently removing dead-letter create rows. */
struct DeadLetterDeleteSummary {
    int Deleted = 0;
    int Failed = 0;
};

/** Result of permanently removing active offline-queue create rows. */
struct PendingQueueDeleteSummary {
    int Deleted = 0;
    int Failed = 0;
};

/** Result of permanently removing active offline-queue field-edit rows. */
struct PendingFieldEditDeleteSummary {
    int Deleted = 0;
    int Failed = 0;
};

/** Result of permanently removing dead-letter field-edit rows. */
struct DeadFieldEditDeleteSummary {
    int Deleted = 0;
    int Failed = 0;
};

/** Result of moving dead-letter field-edit rows back to the active queue. */
struct DeadFieldEditRestoreSummary {
    int Restored = 0;
    int Failed = 0;
};

#endif // SMATCHET_SYNC_OFFLINE_QUEUE_TYPES_H
