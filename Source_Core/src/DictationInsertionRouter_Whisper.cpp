// Real implementation of DictationInsertionRouter. Compiled when
// SMATCHET_WITH_WHISPER=ON (selected via the `list(APPEND CORE_SOURCES …)`
// switch in the root CMakeLists.txt, mirror of the
// AppController_LuaBindings.cpp / AppController_LuaStubs.cpp pattern).
//
// Phase A is the minimum viable registry: tracks focused InputText buffers
// (Register / Unregister round-trip) and reports IsRecording() == false.
// Insert() is the scaffold for Phase D — when no buffer is registered the
// call is a no-op log; once a real audio-capture / transcription path lands
// the live implementation will splice text at cursor under the mutex.

#include "DictationInsertionRouter.h"

#include "Logger.h"

#include <algorithm>
#include <cstddef>

DictationInsertionRouter::DictationInsertionRouter() = default;
DictationInsertionRouter::~DictationInsertionRouter() = default;

void DictationInsertionRouter::RegisterInputText(char* buf, std::size_t cap, int* cursor) {
    if (buf == nullptr || cap == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Update-in-place when re-registering an already-tracked buffer — common in
    // ImGui frames where the same widget re-asserts its registration each draw.
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].Buf == buf) {
            entries_[i].Cap = cap;
            entries_[i].Cursor = cursor;
            return;
        }
    }
    Entry e;
    e.Buf = buf;
    e.Cap = cap;
    e.Cursor = cursor;
    entries_.push_back(e);
}

void DictationInsertionRouter::UnregisterInputText(char* buf) {
    if (buf == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::remove_if(entries_.begin(), entries_.end(),
                             [buf](const Entry& e) { return e.Buf == buf; });
    entries_.erase(it, entries_.end());
}

void DictationInsertionRouter::Insert(const std::string& text) {
    // Phase A scaffold — full UTF-8-safe splice-at-cursor logic lands in Phase D.
    // Until then Insert() is exercised only by tests and (later) the cloud-
    // transcription smoke path; log + drop when no buffer is registered.
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) {
        LOG_DEBUG("DictationInsertionRouter::Insert: no registered target; dropping %zu bytes", text.size());
        return;
    }
    LOG_DEBUG("DictationInsertionRouter::Insert: %zu bytes targeted at %zu registered buffer(s) "
              "(scaffold — full splice lands Phase D)",
              text.size(), entries_.size());
}

bool DictationInsertionRouter::IsRecording() const {
    // Phase A: audio-capture thread is not wired yet (Phase B+ owns WASAPI).
    return false;
}

std::size_t DictationInsertionRouter::RegisteredCountForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}
