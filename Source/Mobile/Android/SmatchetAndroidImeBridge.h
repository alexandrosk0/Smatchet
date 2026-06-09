#pragma once

// JNI bridge to host-Activity services: soft keyboard + Unicode text input, plus window insets.
// imgui_impl_android only emits raw AddKeyEvent (keycodes) and cannot raise the IME or produce
// characters (upstream FIXMEs, imgui issue #3446), so the host owns text input: SmatchetActivity
// exposes showSoftInput / hideSoftInput / pollUnicodeChar, and this bridge drives them from the
// render thread. Calls are debounced on io.WantTextInput edges to keep JNI overhead off the
// 6.94 ms frame budget (Quality Pillar 1). The Activity additionally exposes pollContentInsets
// (safe-area pixels) so the render loop can keep the UI clear of the status bar / nav bar / cutout.

#include <jni.h>

struct ImGuiIO;

namespace smatchet {
namespace mobile {

class SmatchetAndroidImeBridge {
public:
    // Attach the render thread to the JVM, cache a global ref to the activity, and resolve the
    // showSoftInput / hideSoftInput / pollUnicodeChar method ids. `vm` = activity->vm,
    // `activity` = activity->clazz (the NativeActivity instance). Returns false + logs if any
    // lookup fails (bridge then degrades to no-op: no keyboard, no characters — keys still work).
    bool Init(JavaVM* vm, jobject activity);

    // Detach + drop the global ref. Idempotent.
    void Shutdown();

    // Show / hide the soft keyboard when io.WantTextInput changes (rising edge → show, falling
    // edge → hide). No JNI call on frames where the flag is unchanged.
    void ShowKeyboardIfNeeded(ImGuiIO& io);

    // Drain the activity's Unicode queue into io.AddInputCharacter. Cheap when empty (one JNI
    // call returning the empty sentinel).
    void PollUnicodeChars(ImGuiIO& io);

    // Read the current safe-area insets (status bar / nav bar / display cutout) in surface pixels
    // from the host Activity. All four default to 0 when unresolved (older Activity, or the lookup
    // failed) so the UI simply falls back to full-screen. One JNI call returning a packed long.
    void PollContentInsets(int& left, int& top, int& right, int& bottom);

private:
    JNIEnv* AcquireEnv();

    JavaVM* vm_ = nullptr;
    jobject activity_ = nullptr; // global ref
    jmethodID showSoftInput_ = nullptr;
    jmethodID hideSoftInput_ = nullptr;
    jmethodID pollUnicodeChar_ = nullptr;
    jmethodID pollContentInsets_ = nullptr;
    bool lastWantTextInput_ = false;
    bool ready_ = false;
};

} // namespace mobile
} // namespace smatchet
