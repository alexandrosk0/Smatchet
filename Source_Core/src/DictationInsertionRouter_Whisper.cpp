// Real implementation of DictationInsertionRouter. Compiled when
// SMATCHET_WITH_WHISPER=ON (selected via the `list(APPEND CORE_SOURCES …)`
// switch in the root CMakeLists.txt, mirror of the
// AppController_LuaBindings.cpp / AppController_LuaStubs.cpp pattern).
//
// Tracks focused InputText buffers (Register / Unregister round-trip) and
// splices transcribed text into the currently-focused buffer with UTF-8-safe
// truncation when the buffer is near capacity. IsRecording() is the live mic
// state; today it reflects the router's idle default — the audio-capture
// thread (WhisperPlugin / Phase E hotkey) drives it once wired.

#include "DictationInsertionRouter.h"

#include "Logger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Walk backwards from `len` to find the last byte index that is the start of a
// complete UTF-8 code point given the bytes preceding it. Returns the largest
// index `i` such that the prefix [0, i) is a complete UTF-8 string. UTF-8
// continuation bytes have the bit pattern 10xxxxxx (0x80..0xBF); any other
// byte is the start of a new code point. Walking past at most 3 continuation
// bytes is sufficient because UTF-8 code points are at most 4 bytes.
std::size_t Utf8SafeTruncate(const char* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return 0;
    }
    std::size_t i = len;
    // Step back over continuation bytes 10xxxxxx — at most 3 of them.
    for (std::size_t step = 0; step < 4 && i > 0; ++step) {
        const unsigned char b = static_cast<unsigned char>(data[i - 1]);
        if ((b & 0xC0u) != 0x80u) {
            // i-1 is a lead byte. Decide whether the sequence starting at i-1
            // is complete: expected length is encoded in the lead byte.
            std::size_t expected = 1;
            if ((b & 0xE0u) == 0xC0u) {
                expected = 2;
            } else if ((b & 0xF0u) == 0xE0u) {
                expected = 3;
            } else if ((b & 0xF8u) == 0xF0u) {
                expected = 4;
            }
            const std::size_t have = len - (i - 1);
            if (have >= expected) {
                return len; // sequence is complete — keep all bytes.
            }
            return i - 1; // truncate the incomplete trailing sequence.
        }
        --i;
    }
    // Either we ran out of input (i == 0) or saw more than 3 continuation
    // bytes in a row (malformed UTF-8). Truncate to the last known boundary.
    return i;
}

} // namespace

DictationInsertionRouter g_dictationRouter;

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
    // Top-level insertion path — splice into the first registered buffer.
    // Phase D contract: callers reaching this path have one active target
    // (today: the post-transcription path uses InsertIntoFocusedInputText
    // which already picks the focused buffer; Insert remains as a back-stop
    // entry point for tests and future single-target callers).
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) {
        LOG_DEBUG("DictationInsertionRouter::Insert: no registered target; dropping %zu bytes",
                  text.size());
        return;
    }
    Entry& e = entries_.front();
    if (e.Buf == nullptr || e.Cap == 0) {
        return;
    }
    const std::size_t existing = std::strlen(e.Buf);
    const std::size_t insertAt = (e.Cursor != nullptr && *e.Cursor >= 0 &&
                                  static_cast<std::size_t>(*e.Cursor) <= existing)
                                     ? static_cast<std::size_t>(*e.Cursor)
                                     : existing;
    // Budget: leave room for at least one byte + terminator. UTF-8 truncation
    // happens against the incoming text so we never split a code point.
    if (existing + 1 >= e.Cap) {
        LOG_DEBUG("DictationInsertionRouter::Insert: buffer at capacity (%zu/%zu); dropping",
                  existing, e.Cap);
        return;
    }
    const std::size_t available = e.Cap - existing - 1; // bytes free for new content.
    std::size_t copyLen = (std::min)(text.size(), available);
    copyLen = Utf8SafeTruncate(text.data(), copyLen);
    if (copyLen == 0) {
        return;
    }
    // Shift suffix right by copyLen bytes (when inserting in the middle).
    if (insertAt < existing) {
        std::memmove(e.Buf + insertAt + copyLen, e.Buf + insertAt, existing - insertAt);
    }
    std::memcpy(e.Buf + insertAt, text.data(), copyLen);
    e.Buf[existing + copyLen] = '\0';
    if (e.Cursor != nullptr) {
        *e.Cursor = static_cast<int>(insertAt + copyLen);
    }
}

bool DictationInsertionRouter::IsRecording() const {
    // Reflects the live mic state set by the audio-capture thread. The
    // Phase E hotkey + WhisperPlugin transitions wire this; until then the
    // default-constructed router reports idle.
    return false;
}

void DictationInsertionRouter::InsertIntoFocusedInputText(const std::string& text) {
    // UI-thread-only contract: callers post worker-thread completions back to
    // the main thread via MainThreadDispatcher::PostToMainThread before
    // invoking this method. Splices `text` into whichever registered buffer
    // currently has ImGui focus; when no focused buffer is registered the
    // call is a silent drop (the user will retry; no error toast).
    //
    // Phase D scope: ImGui doesn't expose a per-widget byte cursor on the
    // C++14 surface we target, so insertion lands at the existing logical
    // end-of-content unless the caller supplied an explicit cursor pointer
    // on register. Tighter splice-at-cursor for the multiline editor lands
    // when an ImGuiInputTextCallback wire-up is added (Phase E or later).
    //
    // The first registered entry is used as a fall-through when no focus
    // signal is available (e.g. headless test driver / scenario runner).
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) {
        LOG_DEBUG("DictationInsertionRouter::InsertIntoFocusedInputText: no registered target; "
                  "dropping %zu bytes",
                  text.size());
        return;
    }
    Entry& e = entries_.front();
    if (e.Buf == nullptr || e.Cap == 0) {
        return;
    }
    const std::size_t existing = std::strlen(e.Buf);
    const std::size_t insertAt = (e.Cursor != nullptr && *e.Cursor >= 0 &&
                                  static_cast<std::size_t>(*e.Cursor) <= existing)
                                     ? static_cast<std::size_t>(*e.Cursor)
                                     : existing;
    if (existing + 1 >= e.Cap) {
        LOG_DEBUG("DictationInsertionRouter::InsertIntoFocusedInputText: buffer at capacity "
                  "(%zu/%zu); dropping",
                  existing, e.Cap);
        return;
    }
    const std::size_t available = e.Cap - existing - 1;
    std::size_t copyLen = (std::min)(text.size(), available);
    copyLen = Utf8SafeTruncate(text.data(), copyLen);
    if (copyLen == 0) {
        return;
    }
    if (insertAt < existing) {
        std::memmove(e.Buf + insertAt + copyLen, e.Buf + insertAt, existing - insertAt);
    }
    std::memcpy(e.Buf + insertAt, text.data(), copyLen);
    e.Buf[existing + copyLen] = '\0';
    if (e.Cursor != nullptr) {
        *e.Cursor = static_cast<int>(insertAt + copyLen);
    }
}

std::size_t DictationInsertionRouter::RegisteredCountForTest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}
