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

// Bounded length scan: returns the index of the first NUL byte in [0, cap),
// or cap if no terminator exists within the buffer. Prevents the overread UB
// that std::strlen risks on transiently-unterminated buffers (e.g. ImGui can
// resize/move buffer contents between frames; a worker thread arriving with
// a stale pointer must not walk past the registered capacity).
std::size_t BoundedStrlen(const char* buf, std::size_t cap) {
    if (buf == nullptr || cap == 0) {
        return 0;
    }
    std::size_t n = 0;
    while (n < cap && buf[n] != '\0') {
        ++n;
    }
    return n;
}

} // namespace

DictationInsertionRouter g_dictationRouter;

DictationInsertionRouter::DictationInsertionRouter() = default;
DictationInsertionRouter::~DictationInsertionRouter() = default;

void DictationInsertionRouter::RegisterInputText(char* buf, std::size_t cap, int* cursor) {
    // Legacy entry point — leaves ItemId at its existing value (0 for fresh
    // entries) so callers that don't carry an ImGui item id behave exactly as
    // before. Callers from ImGui wrappers should use
    // `RegisterInputTextWithItemId` to enable focused-target dispatch.
    RegisterInputTextWithItemId(buf, cap, cursor, 0);
}

void DictationInsertionRouter::RegisterInputTextWithItemId(char* buf, std::size_t cap, int* cursor,
                                                            unsigned int itemId) {
    if (buf == nullptr || cap == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Update the sticky shadow — survives the wrapper's blur-time
    // unregister so a slow transcription post-back still has a target.
    shadowBuf_ = buf;
    shadowCap_ = cap;
    shadowCursor_ = cursor;
    if (itemId != 0) {
        shadowItemId_ = itemId;
    }
    // Update-in-place when re-registering an already-tracked buffer — common in
    // ImGui frames where the same widget re-asserts its registration each draw.
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].Buf == buf) {
            entries_[i].Cap = cap;
            entries_[i].Cursor = cursor;
            // Only overwrite ItemId if the caller supplied a non-zero one;
            // preserves the legacy "registered without id" entry's value when
            // mixed with another caller (defensive — should not happen in
            // practice since each buf has one owner).
            if (itemId != 0) {
                entries_[i].ItemId = itemId;
            }
            return;
        }
    }
    Entry e;
    e.Buf = buf;
    e.Cap = cap;
    e.Cursor = cursor;
    e.ItemId = itemId;
    entries_.push_back(e);
}

void DictationInsertionRouter::UnregisterInputText(char* buf) {
    if (buf == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Clear the AI Assistant slot if its buffer is the one being unregistered.
    // Otherwise the next `IsFocusedTargetAiAssistant()` call would compare
    // against a freed/reused buffer address — `char*` allocators routinely
    // reuse the same address after free, which would silently misroute the
    // auto-send-on-punctuation flow to a non-AI-Assistant input.
    if (aiAssistantBuf_ == buf) {
        aiAssistantBuf_ = nullptr;
    }
    // Clear the sticky shadow too when its buf is the one being unregistered.
    // Without this, closing the AI Assistant panel (which Unregister-s the chat
    // input buffer) left `shadowBuf_` pointing at the now-detached buffer; a
    // subsequent transcription with no other focused InputText would fall back
    // to the shadow and silently splice into the closed panel's hidden buffer.
    // The user-perceived bug was: "I dictated, the log says inserted N bytes,
    // but I see nothing anywhere." See DictationInsertionRouter.test.cpp
    // "shadow cleared when its buf is unregistered" for the regression gate.
    if (shadowBuf_ == buf) {
        shadowBuf_ = nullptr;
        shadowCap_ = 0;
        shadowCursor_ = nullptr;
        shadowItemId_ = 0;
    }
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
    std::size_t existing = BoundedStrlen(e.Buf, e.Cap);
    if (existing == e.Cap) {
        // Buffer lacks a NUL terminator within its registered capacity; force
        // one in and clamp `existing` so the rest of the splice math is sound.
        e.Buf[e.Cap - 1] = '\0';
        existing = e.Cap - 1;
    }
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
    // Reflects the live mic state set by the WhisperPlugin hotkey hook
    // (see Plugins/Whisper/WhisperPlugin.cpp). Hook thread flips this on
    // press / release; UI thread polls every frame for the status-bar `REC`
    // indicator + the floating amplitude-meter overlay.
    return recording_.load(std::memory_order_acquire);
}

void DictationInsertionRouter::SetRecording(bool active) {
    recording_.store(active, std::memory_order_release);
    if (!active) {
        // Drop the residual amplitude reading so the overlay doesn't show a
        // stale peak after the hotkey is released.
        lastPeakAmplitude_.store(0.0f, std::memory_order_release);
    }
}

bool DictationInsertionRouter::IsTranscribing() const {
    return transcribing_.load(std::memory_order_acquire);
}

void DictationInsertionRouter::SetTranscribing(bool active) {
    transcribing_.store(active, std::memory_order_release);
}

void DictationInsertionRouter::SetLastPeakAmplitude(float peak0to1) {
    float clamped = peak0to1;
    if (clamped < 0.0f) clamped = 0.0f;
    if (clamped > 1.0f) clamped = 1.0f;
    lastPeakAmplitude_.store(clamped, std::memory_order_release);
}

float DictationInsertionRouter::GetLastPeakAmplitude() const {
    return lastPeakAmplitude_.load(std::memory_order_acquire);
}

void DictationInsertionRouter::InsertIntoFocusedInputText(const std::string& text) {
    // Legacy entry point — no caller-supplied active id; fall through to the
    // explicit overload with activeId=0 (entries_.front() fallback path).
    InsertIntoFocusedInputText(text, 0);
}

void DictationInsertionRouter::InsertIntoFocusedInputText(const std::string& text, unsigned int activeId) {
    // UI-thread-only contract: callers post worker-thread completions back to
    // the main thread via MainThreadDispatcher::PostToMainThread before
    // invoking this method. Splices `text` into whichever registered buffer
    // currently has ImGui focus; when no focused buffer is registered the
    // call is a silent drop (the user will retry; no error toast).
    //
    // Selection: prefer the registered entry whose ItemId matches `activeId`
    // (caller passes `ImGui::GetActiveID()`); fall back to `entries_.front()`
    // when no entry matches (legacy contract — headless test driver, scenario
    // runner, or pre-#246 callers passing activeId=0).
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* selected = nullptr;
    if (activeId != 0) {
        for (Entry& candidate : entries_) {
            if (candidate.ItemId == activeId) {
                selected = &candidate;
                break;
            }
        }
    }
    if (selected == nullptr && !entries_.empty()) {
        selected = &entries_.front();
    }
    // Sticky-shadow fallback when entries_ is empty (wrapper-hook blur
    // unregistered the widget between hotkey-release and transcription
    // completion). Materialise into a local Entry so the splice math
    // below works unchanged. See header for the dangling-pointer
    // discussion — risk is bounded because every Smatchet dictation
    // surface uses caller-static buffers.
    Entry shadowEntry;
    if (selected == nullptr && shadowBuf_ != nullptr && shadowCap_ > 0) {
        shadowEntry.Buf = shadowBuf_;
        shadowEntry.Cap = shadowCap_;
        shadowEntry.Cursor = shadowCursor_;
        shadowEntry.ItemId = shadowItemId_;
        selected = &shadowEntry;
        LOG_DEBUG("DictationInsertionRouter::InsertIntoFocusedInputText: entries_ empty, "
                  "using shadow target (buf=%p)", static_cast<const void*>(shadowEntry.Buf));
    }
    if (selected == nullptr) {
        LOG_DEBUG("DictationInsertionRouter::InsertIntoFocusedInputText: no registered target "
                  "and no shadow; dropping %zu bytes", text.size());
        return;
    }
    // Boundary log — worker -> UI hand-off + which buffer was actually picked.
    // This is the single most valuable diagnostic line for "I dictated but
    // nothing landed in the field I expected": it confirms the splice target,
    // the path taken (active-id match vs entries.front() vs shadow), and
    // whether the AI Assistant chat input was the target. Kept permanent at
    // LOG_DEBUG so verbose logs are opt-in but reachable without rebuilding.
    LOG_DEBUG("DictationInsertionRouter: splice activeId=%u entries=%zu picked buf=%p itemId=%u "
              "isAiAssistant=%d (%zu bytes)",
              activeId, entries_.size(), static_cast<const void*>(selected->Buf),
              selected->ItemId, selected->Buf == aiAssistantBuf_ ? 1 : 0, text.size());
    Entry& e = *selected;
    if (e.Buf == nullptr || e.Cap == 0) {
        return;
    }
    std::size_t existing = BoundedStrlen(e.Buf, e.Cap);
    if (existing == e.Cap) {
        e.Buf[e.Cap - 1] = '\0';
        existing = e.Cap - 1;
    }
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

void DictationInsertionRouter::RegisterAiAssistantInputText(char* buf, std::size_t cap, int* cursor) {
    // Same register path as the generic InputText hook; the additional state is
    // a stored buffer pointer the post-insertion auto-send check uses to
    // identify the splice target as the AI Assistant input. AI Assistant calls
    // this every frame the panel is open (idempotent on re-register).
    RegisterInputText(buf, cap, cursor);
    std::lock_guard<std::mutex> lock(mutex_);
    aiAssistantBuf_ = buf;
}

bool DictationInsertionRouter::IsFocusedTargetAiAssistant() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty() || aiAssistantBuf_ == nullptr) {
        return false;
    }
    const Entry& front = entries_.front();
    if (front.Buf != aiAssistantBuf_) {
        return false;
    }
    // Require a non-zero ItemId on the front entry. The AI Assistant panel
    // registers the chat-input buffer once per frame (panel-level call) with
    // ItemId=0 to keep the sticky shadow up to date; the wrapper-level
    // InputTextMultiline hook then either re-registers with the real ImGui
    // item id (when actually focused) or unregisters the buffer (when blurred).
    // So front.ItemId != 0 is the cheap, race-free way to say "the AI input
    // was genuinely the focused widget at last-frame end", which is the only
    // state in which the auto-send-on-punctuation flow should fire. Without
    // this gate, the panel-level idempotent re-register on a frame where the
    // wrapper-unregister had not yet run could let auto-send misfire and ship
    // the dictated text to the model without the user intending a send.
    return front.ItemId != 0u;
}

void DictationInsertionRouter::SetAiAssistantSendCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    aiAssistantSendCb_ = std::move(cb);
}

void DictationInsertionRouter::TriggerAiAssistantSend() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = aiAssistantSendCb_;
    }
    if (cb) {
        cb();
    }
}
