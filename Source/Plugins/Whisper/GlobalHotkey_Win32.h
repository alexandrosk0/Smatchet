#pragma once

// GlobalHotkey_Win32 — push-to-talk hotkey listener.
//
// Two-tier strategy so the hotkey works both when Smatchet is the foreground
// window and when another app is foreground (the common dictation case —
// drafting a long-form email in a browser, holding the hotkey, releasing into
// Smatchet's AI Assistant):
//
//   1. `RegisterHotKey` for in-focus use. Cheap, no security prompts, no
//      global state — the OS dispatches `WM_HOTKEY` to our message-only
//      window when Smatchet has focus.
//   2. `SetWindowsHookEx WH_KEYBOARD_LL` for global use. Required to receive
//      key events while another app is foreground; runs on a dedicated
//      message-pump thread.
//
// Callbacks fire on the hook thread, NOT the UI thread. Callers MUST treat
// `onPress` / `onRelease` as worker entry points (typical pattern:
// `AppController::LaunchBackgroundTask` to do anything non-trivial; never
// touch ImGui state directly).
//
// Debounce: the LL hook flag `LLKHF_REPEAT` (auto-repeat) is filtered, and
// rising-edge detection is enforced by a `pressed_` atomic so synthetic
// duplicate down-events do not re-fire onPress while the key is still held.
//
// RAII: the hook thread is joined on `Unregister` / destruction; `HHOOK` and
// the registered hotkey atom live behind the same lifetime gate so a Win32
// failure leaves no leaked OS handles.

#include "HotkeyParse.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace smatchet {
namespace whisper {

class GlobalHotkey_Win32 {
  public:
    using PressCallback = std::function<void()>;
    using ReleaseCallback = std::function<void()>;

    GlobalHotkey_Win32();
    ~GlobalHotkey_Win32();

    GlobalHotkey_Win32(const GlobalHotkey_Win32&) = delete;
    GlobalHotkey_Win32& operator=(const GlobalHotkey_Win32&) = delete;

    /// Spin up the hook thread + register the OS-level hotkey. Returns false
    /// on `RegisterHotKey` / `SetWindowsHookEx` failure; `outError` then
    /// carries a short human-readable diagnostic ("hotkey already claimed by
    /// another process", "LL hook install failed"). Re-registering while
    /// already registered fails with "hotkey already registered".
    ///
    /// Callbacks may be empty (`nullptr` / default-constructed) — the
    /// listener will still install but the hook is a no-op. Useful for
    /// "register but disable temporarily" flows.
    bool Register(const hotkey::Hotkey& hk,
                  PressCallback onPress,
                  ReleaseCallback onRelease,
                  std::string& outError);

    /// Tear down the hook + join the thread. Idempotent — safe to call on a
    /// non-registered listener. Called by the destructor.
    void Unregister();

    /// True between a successful `Register` and `Unregister` / destruction.
    bool IsRegistered() const noexcept;

    // Forward-declared internals — the anonymous-namespace helpers in
    // GlobalHotkey_Win32.cpp (LL hook proc + message-pump driver) name this
    // type when reading members. Holding it public keeps those helpers free
    // of `friend` boilerplate; consumer code outside the plugin TU has no
    // reason to instantiate or touch it.
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace whisper
} // namespace smatchet
