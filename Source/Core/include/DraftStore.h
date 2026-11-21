#pragma once

// DraftStore.h — what the user typed and has not sent yet, kept across a close, a restart and a crash
// (docs/plans/input-ux-w2-drafts-undo.md). One store per process, keyed by (surface, scope); the bounds
// and the file format are in DraftStorePure.h.
//
// Any thread may call any function. The UI thread only takes a short lock around an in-memory map; the
// file is read and written on the store's own worker (Pillar 2). Start/Stop mirror ConfigSaveWorker.h:
// Start once the UI has a config, Stop at shutdown (writes what is pending, then joins). Without Start
// (unit tests, the CLI) drafts live in memory only.

#include "DraftStorePure.h"

#include <string>

namespace smatchet {
namespace drafts {

/// `<user data directory>smatchet_drafts.json`, next to smatchet_views.json.
std::string DefaultPath();

/// Starts the worker: it reads `path` once, merges it under any draft typed meanwhile, then writes the
/// store back each time it changes (after typing pauses, at least every couple of seconds). Idempotent —
/// a second Start while running is logged and ignored.
void Start(const std::string& path);

/// Writes a pending change, then joins the worker. Idempotent; safe when never started. After Stop the
/// store keeps working in memory, without writing.
void Stop();

/// True once the file read at Start has finished (or failed), and always when the store was never
/// started. A surface that restores at app start waits for this before deciding there is no draft.
bool IsLoaded();

/// Stores `text` for (surface, scope); empty text erases. False when the draft is over a bound and
/// was not kept (DraftStorePure.h). `base` is what the field held when the draft started (optional).
bool Put(const std::string& surface, const std::string& scope, const std::string& text,
         const std::string& base = std::string());
bool Get(const std::string& surface, const std::string& scope, DraftEntry& out);
void Erase(const std::string& surface, const std::string& scope);
/// Erases only while the stored text is `text` (see DraftMap::EraseIfText).
void EraseIfText(const std::string& surface, const std::string& scope, const std::string& text);
void EraseSurface(const std::string& surface);

/// Test hook: forget every draft (the next write empties the file). Any thread.
void ResetForTests();

} // namespace drafts
} // namespace smatchet
