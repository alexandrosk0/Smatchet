// GlobalHotkey_Win32 implementation — Win32-only translation unit. The
// Plugins/Whisper subtree is CMake-conditional + Win32-gated so this file is
// never visited on non-Windows builds.
//
// Thread model:
//   * Register() spawns a single message-pump thread (`hookThreadMain`).
//   * That thread:
//       - Installs a low-level keyboard hook via SetWindowsHookEx WH_KEYBOARD_LL.
//       - Creates a message-only window via CreateWindowExW + HWND_MESSAGE and
//         registers the in-focus hotkey with RegisterHotKey on it. (Both the
//         hook and the registered hotkey must live on the message thread.)
//       - Pumps the message loop (GetMessage / DispatchMessage). The LL hook
//         is process-global; the WM_HOTKEY arrives on the same thread.
//   * Unregister() posts WM_QUIT to the thread, joins it, and tears down the
//     hook + window in the thread's finaliser path (so the OS resource owner
//     matches the OS thread that created it).

#include "GlobalHotkey_Win32.h"

#include "HotkeyParse.h"
#include "Logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace smatchet {
namespace whisper {

#if defined(_WIN32)

namespace {

// A single global pointer to the active LL hook owner. The low-level keyboard
// hook callback is a free function (LRESULT CALLBACK), so it needs a way to
// reach the Impl that owns its state. We keep at most one GlobalHotkey_Win32
// registered at a time (one push-to-talk descriptor per process); on second
// Register without an intervening Unregister, the call fails before touching
// any state, so the global never has competing writers.
class HookOwnerSlot {
  public:
    void Set(struct GlobalHotkey_Win32::Impl* p) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        owner_ = p;
    }
    struct GlobalHotkey_Win32::Impl* Get() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return owner_;
    }

  private:
    std::mutex mutex_;
    struct GlobalHotkey_Win32::Impl* owner_ = nullptr;
};

HookOwnerSlot& OwnerSlot() {
    static HookOwnerSlot s;
    return s;
}

// In-focus hotkey id passed to RegisterHotKey + WM_HOTKEY. Any nonzero int
// works; we pick a distinctive value so a stray WM_HOTKEY (very unusual) is
// recognisable in a debugger trace.
constexpr int kRegisteredHotkeyId = 0xCAFE;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

} // namespace

struct GlobalHotkey_Win32::Impl {
    hotkey::Hotkey hk;
    PressCallback onPress;
    ReleaseCallback onRelease;

    HHOOK hook = nullptr;
    HWND messageWindow = nullptr;
    DWORD threadId = 0;
    std::thread thread;

    std::atomic<bool> registered{false};
    // Rising-edge guard for the LL hook path. WASAPI / multi-event devices
    // occasionally repeat WM_KEYDOWN even with LLKHF_REPEAT clear; we treat the
    // logical "pressed" state as edge-triggered.
    std::atomic<bool> pressed{false};

    // Startup hand-shake so Register() returns success only after the worker
    // has finished installing the hook + window. The worker writes
    // `startupError` (empty on success), signals `startupDone`, and only then
    // enters the message pump.
    std::mutex startupMutex;
    std::condition_variable startupCv;
    bool startupDone = false;
    std::string startupError;

    bool RequiredModsHeld(DWORD vk) const {
        // Auto-suppress on the hotkey-key itself — modifier-state queries on
        // the modifier VKs are unreliable in LL hooks (the key event being
        // processed hasn't propagated to async key state yet).
        if (vk == hk.vk) {
            const bool needCtrl = (hk.mods & hotkey::mod::kControl) != 0;
            const bool needAlt = (hk.mods & hotkey::mod::kAlt) != 0;
            const bool needShift = (hk.mods & hotkey::mod::kShift) != 0;
            const bool needWin = (hk.mods & hotkey::mod::kWin) != 0;

            const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool winDown = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                                 ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

            if (needCtrl != ctrlDown) return false;
            if (needAlt != altDown) return false;
            if (needShift != shiftDown) return false;
            if (needWin != winDown) return false;
            return true;
        }
        return false;
    }
};

namespace {

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (k != nullptr) {
            struct GlobalHotkey_Win32::Impl* impl = OwnerSlot().Get();
            if (impl != nullptr) {
                const DWORD vk = k->vkCode;
                const bool autoRepeat = (k->flags & LLKHF_UP) == 0 &&
                                        impl->pressed.load(std::memory_order_acquire);
                if (vk == impl->hk.vk) {
                    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                        if (!autoRepeat && impl->RequiredModsHeld(vk)) {
                            const bool wasPressed =
                                impl->pressed.exchange(true, std::memory_order_acq_rel);
                            if (!wasPressed && impl->onPress) {
                                try {
                                    impl->onPress();
                                } catch (...) {
                                    LOG_ERROR("GlobalHotkey_Win32: onPress threw");
                                }
                            }
                        }
                    } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                        const bool wasPressed =
                            impl->pressed.exchange(false, std::memory_order_acq_rel);
                        if (wasPressed && impl->onRelease) {
                            try {
                                impl->onRelease();
                            } catch (...) {
                                LOG_ERROR("GlobalHotkey_Win32: onRelease threw");
                            }
                        }
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Message-only window class used for the in-focus RegisterHotKey path. WM_HOTKEY
// routes to whatever window registered the hotkey, so we need a window to act
// as the dispatch target. Message-only windows (`HWND_MESSAGE` parent) never
// render and are cheap.
constexpr const wchar_t* kMessageWindowClass = L"SmatchetWhisperHotkeyMsgWindow";

LRESULT CALLBACK HotkeyMsgWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY && static_cast<int>(wParam) == kRegisteredHotkeyId) {
        // In-focus path: RegisterHotKey fired. The LL hook also fired (LL hook
        // is process-global and sees all key events) so onPress already ran
        // and `pressed` is true — we suppress the duplicate here. Release is
        // detected by the LL hook on WM_KEYUP.
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EnsureMessageWindowClass() {
    // Race-free one-shot registration. Multiple threads can hit Register() at
    // once (Preferences rebind triggers ReregisterHotkey on the UI thread while
    // the legacy hook thread is unwinding); call_once guarantees a single
    // RegisterClassExW call and consistent visibility of the s_ok result.
    static std::once_flag s_once;
    static bool s_ok = false;
    std::call_once(s_once, []() {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HotkeyMsgWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kMessageWindowClass;
        if (RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            s_ok = true;
        }
    });
    return s_ok;
}

void HookThreadMain(GlobalHotkey_Win32::Impl* impl) {
    impl->threadId = GetCurrentThreadId();

    auto signalStartup = [impl](const std::string& err) {
        {
            std::lock_guard<std::mutex> lk(impl->startupMutex);
            impl->startupError = err;
            impl->startupDone = true;
        }
        impl->startupCv.notify_all();
    };

    if (!EnsureMessageWindowClass()) {
        signalStartup("failed to register message-only window class");
        return;
    }

    const HWND msgWindow = CreateWindowExW(0, kMessageWindowClass, L"", 0, 0, 0, 0, 0,
                                           HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (msgWindow == nullptr) {
        signalStartup("CreateWindowExW(HWND_MESSAGE) failed");
        return;
    }
    impl->messageWindow = msgWindow;

    if (!RegisterHotKey(msgWindow, kRegisteredHotkeyId, impl->hk.mods, impl->hk.vk)) {
        const DWORD lastErr = GetLastError();
        DestroyWindow(msgWindow);
        impl->messageWindow = nullptr;
        signalStartup(std::string("RegisterHotKey failed (likely already claimed) — GetLastError=") +
                      std::to_string(lastErr));
        return;
    }

    OwnerSlot().Set(impl);
    impl->hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                   GetModuleHandleW(nullptr), 0);
    if (impl->hook == nullptr) {
        const DWORD lastErr = GetLastError();
        OwnerSlot().Set(nullptr);
        UnregisterHotKey(msgWindow, kRegisteredHotkeyId);
        DestroyWindow(msgWindow);
        impl->messageWindow = nullptr;
        signalStartup(std::string("SetWindowsHookEx(WH_KEYBOARD_LL) failed — GetLastError=") +
                      std::to_string(lastErr));
        return;
    }

    impl->registered.store(true, std::memory_order_release);
    signalStartup(std::string());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Tear-down on this same thread (OS resource owner == OS thread).
    if (impl->hook != nullptr) {
        UnhookWindowsHookEx(impl->hook);
        impl->hook = nullptr;
    }
    OwnerSlot().Set(nullptr);
    if (impl->messageWindow != nullptr) {
        UnregisterHotKey(impl->messageWindow, kRegisteredHotkeyId);
        DestroyWindow(impl->messageWindow);
        impl->messageWindow = nullptr;
    }
    impl->registered.store(false, std::memory_order_release);
}

} // namespace

GlobalHotkey_Win32::GlobalHotkey_Win32() : impl_(new Impl()) {}

GlobalHotkey_Win32::~GlobalHotkey_Win32() {
    Unregister();
}

bool GlobalHotkey_Win32::Register(const hotkey::Hotkey& hk,
                                  PressCallback onPress,
                                  ReleaseCallback onRelease,
                                  std::string& outError) {
    outError.clear();
    if (impl_->registered.load(std::memory_order_acquire) || impl_->thread.joinable()) {
        outError = "hotkey already registered";
        return false;
    }
    if (hk.vk == 0) {
        outError = "hotkey has no key";
        return false;
    }
    if (hk.mods == 0) {
        // Refuse modifier-free hotkeys at registration time. A bare global key
        // (e.g. just "Space") would steal every key event from the rest of the
        // system; it is never what the user means even if they hand-edit the
        // Preferences descriptor. The Preferences capture UI already refuses
        // bare-key combos, but we belt-and-suspender it here so config-file or
        // CLI callers cannot bypass.
        outError = "hotkey must include at least one modifier (Ctrl / Alt / Shift / Win)";
        return false;
    }
    if (OwnerSlot().Get() != nullptr) {
        outError = "another GlobalHotkey_Win32 instance already owns the LL hook in this process";
        return false;
    }

    impl_->hk = hk;
    impl_->onPress = std::move(onPress);
    impl_->onRelease = std::move(onRelease);
    impl_->pressed.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(impl_->startupMutex);
        impl_->startupDone = false;
        impl_->startupError.clear();
    }

    impl_->thread = std::thread(HookThreadMain, impl_.get());

    // Block briefly until the worker either reports success or fails to
    // install the hook. A 2-second ceiling is generous — Win32 SetWindowsHookEx
    // returns in microseconds in normal operation.
    std::unique_lock<std::mutex> lk(impl_->startupMutex);
    if (!impl_->startupCv.wait_for(lk, std::chrono::seconds(2),
                                   [this]() { return impl_->startupDone; })) {
        outError = "hotkey worker thread did not initialise within 2s";
        // Force the worker to exit so the destructor doesn't deadlock on join.
        if (impl_->threadId != 0) {
            PostThreadMessageW(impl_->threadId, WM_QUIT, 0, 0);
        }
        lk.unlock();
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
        impl_->threadId = 0;
        return false;
    }
    if (!impl_->startupError.empty()) {
        outError = impl_->startupError;
        lk.unlock();
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
        impl_->threadId = 0;
        return false;
    }
    return true;
}

void GlobalHotkey_Win32::Unregister() {
    if (!impl_) {
        return;
    }
    const DWORD tid = impl_->threadId;
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->threadId = 0;
    impl_->pressed.store(false, std::memory_order_release);
    impl_->onPress = PressCallback();
    impl_->onRelease = ReleaseCallback();
}

bool GlobalHotkey_Win32::IsRegistered() const noexcept {
    return impl_ && impl_->registered.load(std::memory_order_acquire);
}

#else // !_WIN32

// Stub for non-Windows hosts. The Plugins/Whisper subtree is Windows-gated at
// the CMake level so this branch is never linked, but the visible class needs
// to exist for any future cross-platform smoke compile.

struct GlobalHotkey_Win32::Impl {};

GlobalHotkey_Win32::GlobalHotkey_Win32() : impl_(new Impl()) {}
GlobalHotkey_Win32::~GlobalHotkey_Win32() = default;

bool GlobalHotkey_Win32::Register(const hotkey::Hotkey& /*hk*/,
                                  PressCallback /*onPress*/,
                                  ReleaseCallback /*onRelease*/,
                                  std::string& outError) {
    outError = "GlobalHotkey_Win32 is Windows-only";
    return false;
}

void GlobalHotkey_Win32::Unregister() {}

bool GlobalHotkey_Win32::IsRegistered() const noexcept {
    return false;
}

#endif // _WIN32

} // namespace whisper
} // namespace smatchet
