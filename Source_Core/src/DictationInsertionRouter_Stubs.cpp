// No-op implementation of DictationInsertionRouter. Compiled when
// SMATCHET_WITH_WHISPER=OFF (the OFF branch of the source-list conditional in
// the root CMakeLists.txt). Mirrors the AppController_LuaStubs.cpp discipline:
// same exported symbols as the real TU, so call sites under SmatchetUI /
// SmatchetAiAssistantUi / TicketFieldEditor / SmatchetCommandPaletteUi need
// no per-callsite SMATCHET_WITH_WHISPER ifdefs.
//
// Drift between this stubs TU and DictationInsertionRouter_Whisper.cpp is
// caught at CI link time by the build-windows-no-whisper job in
// .github/workflows/build-and-test.yml — same regression-prevention shape as
// the existing Lua bindings / stubs gate.

#include "DictationInsertionRouter.h"

#include <cstddef>
#include <string>

DictationInsertionRouter g_dictationRouter;

DictationInsertionRouter::DictationInsertionRouter() = default;
DictationInsertionRouter::~DictationInsertionRouter() = default;

void DictationInsertionRouter::RegisterInputText(char* /*buf*/, std::size_t /*cap*/, int* /*cursor*/) {}

void DictationInsertionRouter::UnregisterInputText(char* /*buf*/) {}

void DictationInsertionRouter::Insert(const std::string& /*text*/) {}

bool DictationInsertionRouter::IsRecording() const {
    return false;
}

void DictationInsertionRouter::InsertIntoFocusedInputText(const std::string& /*text*/) {}

std::size_t DictationInsertionRouter::RegisteredCountForTest() const {
    return 0;
}
