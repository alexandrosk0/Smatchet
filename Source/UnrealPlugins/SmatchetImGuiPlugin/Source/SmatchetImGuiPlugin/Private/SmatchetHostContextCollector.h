#pragma once

#include "CoreMinimal.h"

class FSmatchetOutputLogTail;

/**
 * Build the host-context snapshot JSON pushed to Smatchet via
 * SmatchetHost_SetHostContextJson (see that C ABI doc comment for the schema).
 * GAME THREAD ONLY — reads GEditor / world / selection state. Editor-only fields
 * (PIE, selected actors, editor world) are omitted in non-editor builds; Smatchet
 * skips missing keys, so the payload degrades cleanly in packaged games.
 * `LogTail` may be null (the logTail field is omitted).
 */
FString SmatchetHostContextCollector_BuildJson(const FSmatchetOutputLogTail* LogTail);
