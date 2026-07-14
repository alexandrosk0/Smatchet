#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/OutputDevice.h"

/**
 * Fixed-size ring buffer of recent Output Log lines, registered with GLog for the
 * plugin module's lifetime. Feeds the `logTail` field of the host-context snapshot
 * (SmatchetHostContextCollector) that Smatchet prefills into quick-create issue
 * descriptions.
 *
 * Serialize() may run on any thread (CanBeUsedOnAnyThread) — the ring is guarded by
 * a critical section and each stored line is length-capped so a log flood costs a
 * bounded amount of memory. Verbose/VeryVerbose lines are skipped: the snapshot is
 * a human-readable summary, not a full log capture.
 */
class FSmatchetOutputLogTail final : public FOutputDevice {
  public:
    explicit FSmatchetOutputLogTail(int32 InCapacity = 400);
    virtual ~FSmatchetOutputLogTail() override = default;

    // FOutputDevice
    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
    virtual bool CanBeUsedOnAnyThread() const override { return true; }

    /** Copy of the buffered lines, oldest first. Safe from any thread. */
    TArray<FString> SnapshotLines() const;

  private:
    mutable FCriticalSection Mutex;
    TArray<FString> Ring; // pre-sized to Capacity; Head is the next write slot
    int32 Head = 0;
    int32 Count = 0;
    int32 Capacity = 0;
};
