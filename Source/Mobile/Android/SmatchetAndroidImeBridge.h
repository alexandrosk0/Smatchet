#pragma once

// JNI bridge to the soft keyboard + Unicode text input. imgui_impl_android only emits raw
// AddKeyEvent (keycodes) and cannot raise the IME or produce characters (upstream FIXMEs,
// imgui issue #3446), so the host owns text input: a Kotlin SmatchetActivity exposes
// showSoftInput / hideSoftInput / pollUnicodeChar, and this bridge drives them from the
// render thread. Calls are debounced on io.WantTextInput edges to keep JNI overhead off the
// 6.94 ms frame budget (Quality Pillar 1).

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

private:
    JNIEnv* AcquireEnv();

    JavaVM* vm_ = nullptr;
    jobject activity_ = nullptr; // global ref
    jmethodID showSoftInput_ = nullptr;
    jmethodID hideSoftInput_ = nullptr;
    jmethodID pollUnicodeChar_ = nullptr;
    bool lastWantTextInput_ = false;
    bool ready_ = false;
};

} // namespace mobile
} // namespace smatchet
