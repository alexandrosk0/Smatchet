#include "SmatchetAndroidImeBridge.h"

#include "SmatchetAndroidPlatform.h" // SLOG / SLOGE

#include "imgui.h"

namespace smatchet {
namespace mobile {

bool SmatchetAndroidImeBridge::Init(JavaVM* vm, jobject activity) {
    if (vm == nullptr || activity == nullptr) {
        SLOGE("ImeBridge::Init: null vm or activity");
        return false;
    }
    vm_ = vm;

    JNIEnv* env = AcquireEnv();
    if (env == nullptr) {
        return false;
    }

    // CPP_CODE_AUDIT.md #32: mirror SecretBridge::Init's three-case handling —
    // NewGlobalRef can return null (OOM, pending OutOfMemoryError) and passing
    // that null jobject into GetObjectClass is UB; a failed GetObjectClass can
    // also leave a pending exception that must be cleared (and the global ref
    // released) before returning, or the next JNI call on this thread aborts
    // under CheckJNI.
    activity_ = env->NewGlobalRef(activity);
    if (activity_ == nullptr) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        SLOGE("ImeBridge::Init: NewGlobalRef failed");
        return false;
    }
    jclass clazz = env->GetObjectClass(activity_);
    if (clazz == nullptr) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        SLOGE("ImeBridge::Init: GetObjectClass failed");
        env->DeleteGlobalRef(activity_);
        activity_ = nullptr;
        return false;
    }

    showSoftInput_ = env->GetMethodID(clazz, "showSoftInput", "()V");
    hideSoftInput_ = env->GetMethodID(clazz, "hideSoftInput", "()V");
    pollUnicodeChar_ = env->GetMethodID(clazz, "pollUnicodeChar", "()I");
    // Window-inset getter is optional: resolved independently of the IME methods so a build/Activity
    // without it still gets a working keyboard — insets just stay 0 (full-screen, pre-fix behavior).
    pollContentInsets_ = env->GetMethodID(clazz, "pollContentInsets", "()J");
    if (pollContentInsets_ == nullptr && env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(clazz);

    if (showSoftInput_ == nullptr || hideSoftInput_ == nullptr || pollUnicodeChar_ == nullptr) {
        // A pending JNI exception (NoSuchMethodError) must be cleared or the next JNI call aborts.
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        SLOGE("ImeBridge::Init: missing SmatchetActivity method(s) — IME disabled");
        // Release the global ref on this failure path too, matching SecretBridge::Init —
        // otherwise a later Shutdown() call sees a non-null activity_ from a bridge that
        // never became ready and double-frees / leaks it.
        env->DeleteGlobalRef(activity_);
        activity_ = nullptr;
        return false;
    }

    ready_ = true;
    SLOG("ImeBridge ready");
    return true;
}

void SmatchetAndroidImeBridge::Shutdown() {
    if (vm_ != nullptr && activity_ != nullptr) {
        JNIEnv* env = AcquireEnv();
        if (env != nullptr) {
            env->DeleteGlobalRef(activity_);
        }
    }
    activity_ = nullptr;
    ready_ = false;
}

JNIEnv* SmatchetAndroidImeBridge::AcquireEnv() {
    if (vm_ == nullptr) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    const jint getResult = vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getResult == JNI_OK) {
        return env;
    }
    if (getResult == JNI_EDETACHED) {
        if (vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            return env;
        }
        SLOGE("ImeBridge: AttachCurrentThread failed");
        return nullptr;
    }
    SLOGE("ImeBridge: GetEnv failed (%d)", static_cast<int>(getResult));
    return nullptr;
}

void SmatchetAndroidImeBridge::ShowKeyboardIfNeeded(ImGuiIO& io) {
    if (!ready_) {
        return;
    }
    const bool want = io.WantTextInput;
    // A fresh pointer press this frame. imgui_impl_android feeds touches into MouseDown[0], and ImGui
    // guarantees a press is visible for at least one frame even when press+release land together — so
    // a quick tap is never missed. Read before NewFrame, so this is the prior frame's state (matches
    // the WantTextInput read above); the one-frame lag is imperceptible.
    const bool pointerDown = io.MouseDown[0];
    const bool tapEdge = pointerDown && !lastPointerDown_;
    lastPointerDown_ = pointerDown;

    JNIEnv* env = AcquireEnv();
    if (env == nullptr) {
        return;
    }

    if (want) {
        // Raise on the rising edge of WantTextInput (a widget just took text focus) OR on a fresh tap
        // while a text widget wants input. ImGui keeps WantTextInput true after a Back/swipe dismiss,
        // so it never falls — only a new tap signals "bring the keyboard back" (item 11 / #1055).
        // There is deliberately NO steady-state re-raise: that is item 10 (#1054) — a dismiss leaves
        // the field focused but the keyboard stays down until the user taps the field again. Because
        // the tap that deactivates a field also drops WantTextInput in the same frame, a tap on a
        // non-text widget hits `want == false` here and cannot spuriously re-raise.
        if (!lastWantTextInput_ || tapEdge) {
            env->CallVoidMethod(activity_, showSoftInput_);
        }
    } else if (lastWantTextInput_) {
        // Falling edge: focus left a text widget → hide.
        env->CallVoidMethod(activity_, hideSoftInput_);
    }
    lastWantTextInput_ = want;
}

void SmatchetAndroidImeBridge::PollUnicodeChars(ImGuiIO& io) {
    if (!ready_) {
        return;
    }
    JNIEnv* env = AcquireEnv();
    if (env == nullptr) {
        return;
    }
    // pollUnicodeChar returns 0 when the queue is empty (the sentinel). item 9 (#1070): drain the
    // WHOLE queue each frame so a long clipboard paste arrives intact in one go — the old 64/frame
    // cap silently truncated big pastes. The guard is now just a runaway backstop (a far larger
    // paste than any real one) so a misbehaving producer still can't loop forever and stall the
    // frame (Pillar 2). The queue is bounded UI-side, so this normally exits on the empty sentinel.
    const int kDrainBackstop = 65536;
    for (int guard = 0; guard < kDrainBackstop; ++guard) {
        const jint codepoint = env->CallIntMethod(activity_, pollUnicodeChar_);
        if (codepoint <= 0) {
            break;
        }
        io.AddInputCharacter(static_cast<unsigned int>(codepoint));
    }
}

void SmatchetAndroidImeBridge::PollContentInsets(int& left, int& top, int& right, int& bottom) {
    left = 0;
    top = 0;
    right = 0;
    bottom = 0;
    if (!ready_ || pollContentInsets_ == nullptr) {
        return;
    }
    JNIEnv* env = AcquireEnv();
    if (env == nullptr) {
        return;
    }
    // Packed by SmatchetActivity.pollContentInsets(): [left:16][top:16][right:16][bottom:16], each
    // an unsigned pixel count (< 65536).
    const jlong packed = env->CallLongMethod(activity_, pollContentInsets_);
    left = static_cast<int>((packed >> 48) & 0xFFFF);
    top = static_cast<int>((packed >> 32) & 0xFFFF);
    right = static_cast<int>((packed >> 16) & 0xFFFF);
    bottom = static_cast<int>(packed & 0xFFFF);
}

} // namespace mobile
} // namespace smatchet
