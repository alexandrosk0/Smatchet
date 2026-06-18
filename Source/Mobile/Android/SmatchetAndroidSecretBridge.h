#pragma once

// JNI bridge to the host Activity's AndroidKeyStore-backed secret store (audit H2). The desktop
// build seals config secrets (API keys, MCP auth token) at rest with DPAPI; on Android the Core has
// no equivalent and historically persisted them as PLAINTEXT in smatchet_config.json. SmatchetActivity
// exposes protectSecret(String)->String / unprotectSecret(String)->String, backed by an AES-GCM key
// held in the AndroidKeyStore (hardware-bound where StrongBox is present), and this bridge drives
// them. It is installed as the process-wide ConfigManager secret provider at boot (BEFORE the first
// ConfigManager::Load) so every config read/write transparently routes its secret fields through the
// Keystore — the Android analogue of the Windows DPAPI seam.
//
// FAIL-SAFE CONTRACT (load-bearing): Protect / Unprotect return an EMPTY string on ANY failure — null
// vm/activity, unresolved method, a pending JNI exception, an OOM in NewStringUTF, or a null Java
// return (the Java store returns null/empty on any KeyStore/crypto error). Core treats that empty as
// "no secret": on protect the secret is simply NOT persisted (never written cleartext after a failed
// encrypt); on unprotect it is treated as absent (never a half-decrypted value). A secret is never
// surfaced or stored in cleartext on a Keystore failure.

#include <jni.h>

#include <mutex>
#include <string>

namespace smatchet {
namespace mobile {

class SmatchetAndroidSecretBridge {
public:
    // Attach the calling thread to the JVM, cache a global ref to the activity, and resolve the
    // protectSecret / unprotectSecret method ids. `vm` = activity->vm, `activity` = activity->clazz.
    // Returns false + logs if any lookup fails (bridge then stays un-ready: Protect / Unprotect return
    // empty, so Core FAILS CLOSED — the secret is dropped rather than persisted in cleartext).
    bool Init(JavaVM* vm, jobject activity);

    // Drop the global ref and clear readiness. Idempotent and thread-safe: serialized via mutex_ against
    // a concurrent Protect / Unprotect so an in-flight JNI call on the config-save worker finishes before
    // the activity global ref is deleted (no use-after-delete during teardown). (The JVM thread attach is
    // intentionally left in place — see the AcquireEnv note; the attached threads live for the process.)
    void Shutdown();

    // Seal a plaintext secret -> opaque base64 token (AES-GCM ciphertext, IV-prepended) for at-rest
    // storage. Returns empty on any failure (fail-safe: the caller must then NOT persist the secret).
    std::string Protect(const std::string& plain);

    // Reverse of Protect: opaque token -> plaintext secret. Returns empty on any failure OR if the
    // token was not produced by this device's key (fail-safe: the caller treats empty as "no secret").
    std::string Unprotect(const std::string& token);

private:
    JNIEnv* AcquireEnv();

    // Shared round-trip for both Activity methods (signature "(Ljava/lang/String;)Ljava/lang/String;").
    // Owns the full fail-safe: returns empty on a null env, a NewStringUTF failure, a pending JNI
    // exception, or a null Java return. Local refs are deleted before returning so a long-lived
    // attached thread (the config-save worker) cannot accumulate them across repeated saves.
    std::string CallStringMethod(jmethodID method, const std::string& in);

    // Serializes Protect / Unprotect against Shutdown so a teardown can't delete activity_ while a
    // config-save-worker JNI round-trip is in flight (CR #1357 Shutdown-vs-Protect race). Init runs once
    // at boot before the provider is installed, so it does not contend and is left unlocked.
    std::mutex mutex_;
    JavaVM* vm_ = nullptr;
    jobject activity_ = nullptr; // global ref
    jmethodID protectSecret_ = nullptr;
    jmethodID unprotectSecret_ = nullptr;
    bool ready_ = false;
};

} // namespace mobile
} // namespace smatchet
