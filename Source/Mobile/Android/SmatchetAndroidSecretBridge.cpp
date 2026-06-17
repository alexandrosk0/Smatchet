#include "SmatchetAndroidSecretBridge.h"

#include "SmatchetAndroidPlatform.h" // SLOG / SLOGE

namespace smatchet {
namespace mobile {

bool SmatchetAndroidSecretBridge::Init(JavaVM* vm, jobject activity) {
    if (vm == nullptr || activity == nullptr) {
        SLOGE("SecretBridge::Init: null vm or activity");
        return false;
    }
    vm_ = vm;

    JNIEnv* env = AcquireEnv();
    if (env == nullptr) {
        return false;
    }

    activity_ = env->NewGlobalRef(activity);
    if (activity_ == nullptr) {
        // OOM creating the global ref. Passing a null jobject to GetObjectClass below is undefined
        // (aborts under CheckJNI), so bail before that. activity_ stays null -> Shutdown is a no-op.
        SLOGE("SecretBridge::Init: NewGlobalRef failed");
        return false;
    }
    jclass clazz = env->GetObjectClass(activity_);
    if (clazz == nullptr) {
        SLOGE("SecretBridge::Init: GetObjectClass failed");
        env->DeleteGlobalRef(activity_);
        activity_ = nullptr;
        return false;
    }

    protectSecret_ = env->GetMethodID(clazz, "protectSecret", "(Ljava/lang/String;)Ljava/lang/String;");
    unprotectSecret_ = env->GetMethodID(clazz, "unprotectSecret", "(Ljava/lang/String;)Ljava/lang/String;");
    env->DeleteLocalRef(clazz);

    if (protectSecret_ == nullptr || unprotectSecret_ == nullptr) {
        // A pending JNI exception (NoSuchMethodError) must be cleared or the next JNI call aborts.
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        SLOGE("SecretBridge::Init: missing SmatchetActivity method(s) — Keystore secret store disabled");
        // Release the global ref on this failure path so a later Shutdown can't double-free / leak it.
        env->DeleteGlobalRef(activity_);
        activity_ = nullptr;
        return false;
    }

    ready_ = true;
    SLOG("SecretBridge ready (AndroidKeyStore secret-at-rest)");
    return true;
}

void SmatchetAndroidSecretBridge::Shutdown() {
    if (vm_ != nullptr && activity_ != nullptr) {
        JNIEnv* env = AcquireEnv();
        if (env != nullptr) {
            env->DeleteGlobalRef(activity_);
        }
    }
    activity_ = nullptr;
    ready_ = false;
}

JNIEnv* SmatchetAndroidSecretBridge::AcquireEnv() {
    if (vm_ == nullptr) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    const jint getResult = vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getResult == JNI_OK) {
        return env;
    }
    if (getResult == JNI_EDETACHED) {
        // The config secret provider is also invoked from the coalescing config-save WORKER thread
        // (ProtectSecretForConfig runs inside ConfigManager::Save), not just the boot thread. Attach it
        // on first use. The attach is intentionally never undone: both the boot thread and the save
        // worker live for the whole process, and detaching mid-life would just re-attach on the next
        // save. This matches the IME bridge's render-thread attach.
        if (vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            return env;
        }
        SLOGE("SecretBridge: AttachCurrentThread failed");
        return nullptr;
    }
    SLOGE("SecretBridge: GetEnv failed (%d)", static_cast<int>(getResult));
    return nullptr;
}

std::string SmatchetAndroidSecretBridge::CallStringMethod(jmethodID method, const std::string& in) {
    JNIEnv* env = AcquireEnv();
    if (env == nullptr) {
        return std::string();
    }

    // Secrets are API keys / tokens / base64 ciphertext — ASCII, no embedded NUL (the config persist
    // path strips CR/LF/NUL), so NewStringUTF's modified-UTF8 encoding is identical to the bytes here.
    jstring jin = env->NewStringUTF(in.c_str());
    if (jin == nullptr) {
        // OOM building the argument. Clear any pending exception and fail safe.
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return std::string();
    }

    jobject jresult = env->CallObjectMethod(activity_, method, jin);
    // A Java-side exception (KeyStore unavailable, AEAD tag mismatch, etc.) must be cleared before the
    // next JNI call, and is treated as a hard failure -> empty.
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(jin);
        return std::string();
    }

    std::string out;
    if (jresult != nullptr) {
        const jsize len = env->GetStringUTFLength(static_cast<jstring>(jresult));
        const char* chars = env->GetStringUTFChars(static_cast<jstring>(jresult), nullptr);
        if (chars != nullptr) {
            out.assign(chars, static_cast<size_t>(len));
            env->ReleaseStringUTFChars(static_cast<jstring>(jresult), chars);
        }
        env->DeleteLocalRef(jresult);
    }
    // jresult == null is the Java store's fail-safe sentinel -> out stays empty.

    env->DeleteLocalRef(jin);
    return out;
}

std::string SmatchetAndroidSecretBridge::Protect(const std::string& plain) {
    if (!ready_) {
        return std::string();
    }
    return CallStringMethod(protectSecret_, plain);
}

std::string SmatchetAndroidSecretBridge::Unprotect(const std::string& token) {
    if (!ready_) {
        return std::string();
    }
    return CallStringMethod(unprotectSecret_, token);
}

} // namespace mobile
} // namespace smatchet
