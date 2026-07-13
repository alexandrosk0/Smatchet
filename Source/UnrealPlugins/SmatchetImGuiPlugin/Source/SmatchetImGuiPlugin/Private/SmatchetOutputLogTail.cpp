#include "SmatchetOutputLogTail.h"

#include "Misc/ScopeLock.h"

namespace {

// Per-line cap keeps a single pathological log line (serialized blobs, minified
// JSON) from dominating the snapshot payload.
constexpr int32 kMaxLineChars = 512;

} // namespace

FSmatchetOutputLogTail::FSmatchetOutputLogTail(int32 InCapacity) : Capacity(FMath::Max(1, InCapacity)) {
    Ring.SetNum(Capacity);
}

void FSmatchetOutputLogTail::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) {
    if (!V || Verbosity > ELogVerbosity::Log) {
        return; // skip Verbose/VeryVerbose noise — the tail is a summary, not a capture
    }
    FString Line = FString::Printf(TEXT("[%s] %s: %s"), ToString(Verbosity), *Category.ToString(), V);
    if (Line.Len() > kMaxLineChars) {
        Line.LeftInline(kMaxLineChars);
        Line += TEXT("…");
    }

    FScopeLock Lock(&Mutex);
    Ring[Head] = MoveTemp(Line);
    Head = (Head + 1) % Capacity;
    Count = FMath::Min(Count + 1, Capacity);
}

TArray<FString> FSmatchetOutputLogTail::SnapshotLines() const {
    FScopeLock Lock(&Mutex);
    TArray<FString> Out;
    Out.Reserve(Count);
    const int32 Start = (Head - Count + Capacity) % Capacity;
    for (int32 i = 0; i < Count; ++i) {
        Out.Add(Ring[(Start + i) % Capacity]);
    }
    return Out;
}
