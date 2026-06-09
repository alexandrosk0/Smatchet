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

    activity_ = env->NewGlobalRef(activity);
    jclass clazz = env->GetObjectClass(activity_);
    if (clazz == nullptr) {
        SLOGE("ImeBridge::Init: GetObjectClass failed");
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
    if (want == lastWantTextInput_) {
        return;
    }
    lastWantTextInput_ = want;

    JNIEnv* env = AcquireEnv();
    if (env == nullptr) {
        return;
    }
    env->CallVoidMethod(activity_, want ? showSoftInput_ : hideSoftInput_);
}

void SmatchetAndroidImeBridge::PollUnicodeChars(ImGuiIO& io) {
    if (!ready_) {
        return;
    }
    JNIEnv* env = AcquireEnv();
    if (env == nullptr) {
        return;
    }
    // pollUnicodeChar returns 0 when the queue is empty (the sentinel). Bound the drain so a
    // misbehaving producer can never stall the frame.
    for (int guard = 0; guard < 64; ++guard) {
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
