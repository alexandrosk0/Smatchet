package com.smatchet.mobile;

import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.nio.charset.StandardCharsets;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * AndroidKeyStore-backed secret-at-rest store (audit H2). The Android analogue of the desktop's
 * DPAPI seam: the native Core has no per-user OS keychain on Android and historically wrote config
 * secrets (API keys, MCP auth token) as PLAINTEXT in smatchet_config.json. This seals each secret
 * with an AES-256-GCM key held in the AndroidKeyStore — non-exportable, hardware-bound where a
 * StrongBox / TEE is present — so the on-disk config carries only opaque ciphertext.
 *
 * <p>Token format: {@code base64( IV[12] || ciphertext||GCM-tag )}, where the 12-byte GCM nonce is
 * provider-generated per encryption (AndroidKeyStore forbids a caller-supplied IV) and prepended.
 *
 * <p>FAIL-SAFE: every entry point catches {@link Throwable} and returns the empty string on ANY
 * failure (KeyStore unavailable, AEAD tag mismatch from a tampered/foreign token, decode error). The
 * native bridge (SmatchetAndroidSecretBridge) and Core treat empty as "no secret": on protect the
 * secret is simply NOT persisted (never cleartext after a failed encrypt); on unprotect it is treated
 * as absent. A secret is never surfaced or stored in cleartext on a Keystore failure.
 *
 * <p>Called from native code via SmatchetActivity.protectSecret / unprotectSecret (resolved by JNI).
 */
final class SmatchetSecretStore {

    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String KEY_ALIAS = "SmatchetConfigSecret";
    private static final String TRANSFORM = "AES/GCM/NoPadding";
    private static final int IV_LENGTH = 12;  // GCM standard nonce length (bytes)
    private static final int TAG_BITS = 128;  // GCM authentication tag length (bits)
    private static final int KEY_BITS = 256;

    private SmatchetSecretStore() {}

    /**
     * Seal a plaintext secret. Returns {@code base64(IV || ciphertext+tag)}, or "" on empty input or
     * any failure (fail-safe: the caller must then NOT persist the secret).
     */
    static String protect(String plain) {
        if (plain == null || plain.isEmpty()) {
            return "";
        }
        try {
            return seal(getOrCreateKey(), plain);
        } catch (Throwable t) {
            return "";
        }
    }

    /**
     * Recover a secret sealed by {@link #protect}. Returns "" on empty input, a missing key, a decode
     * error, or an AEAD tag mismatch (tampered or foreign-device token) — all fail-safe to "no secret".
     */
    static String unprotect(String token) {
        if (token == null || token.isEmpty()) {
            return "";
        }
        try {
            SecretKey key = getKey();
            if (key == null) {
                return "";
            }
            return open(key, token);
        } catch (Throwable t) {
            return "";
        }
    }

    // ---- crypto core (no AndroidKeyStore dependency, so the JVM/Robolectric harness can round-trip
    //      it with an injected software AES key — the AndroidKeyStore provider is not available there) ----

    static String seal(SecretKey key, String plain) throws Exception {
        Cipher cipher = Cipher.getInstance(TRANSFORM);
        cipher.init(Cipher.ENCRYPT_MODE, key);
        // AndroidKeyStore generates the nonce itself; read it back rather than supplying one.
        byte[] iv = cipher.getIV();
        byte[] ct = cipher.doFinal(plain.getBytes(StandardCharsets.UTF_8));
        byte[] out = new byte[iv.length + ct.length];
        System.arraycopy(iv, 0, out, 0, iv.length);
        System.arraycopy(ct, 0, out, iv.length, ct.length);
        return Base64.encodeToString(out, Base64.NO_WRAP);
    }

    static String open(SecretKey key, String token) throws Exception {
        byte[] all = Base64.decode(token, Base64.NO_WRAP);
        if (all.length <= IV_LENGTH) {
            return ""; // too short to carry an IV + a non-empty GCM tag — not our token
        }
        Cipher cipher = Cipher.getInstance(TRANSFORM);
        cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(TAG_BITS, all, 0, IV_LENGTH));
        byte[] pt = cipher.doFinal(all, IV_LENGTH, all.length - IV_LENGTH);
        return new String(pt, StandardCharsets.UTF_8);
    }

    // ---- AndroidKeyStore key management ----

    // synchronized: the secret provider is called from both the boot thread (first Load) and the
    // native config-save worker. Without serialization, two threads could each observe a missing key
    // and both generateKey() — the second call replaces the alias, so ciphertext sealed under the
    // first key becomes permanently undecryptable. Locking makes the check-then-create atomic.
    private static synchronized SecretKey getOrCreateKey() throws Exception {
        SecretKey existing = getKey();
        if (existing != null) {
            return existing;
        }
        KeyGenerator kg = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE);
        KeyGenParameterSpec.Builder b = new KeyGenParameterSpec.Builder(
                        KEY_ALIAS,
                        KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(KEY_BITS)
                // The secret must be usable headlessly (the native config-save worker has no UI to
                // prompt for biometric/device-credential auth), so it is NOT gated on user auth.
                .setUserAuthenticationRequired(false);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            // Prefer a hardware-isolated StrongBox key where the device has one. StrongBox absence
            // surfaces as a StrongBoxUnavailableException at generateKey() time, not init — so try the
            // StrongBox build first and fall back to a TEE-backed key on failure.
            try {
                b.setIsStrongBoxBacked(true);
                kg.init(b.build());
                return kg.generateKey();
            } catch (Throwable strongBoxUnavailable) {
                b.setIsStrongBoxBacked(false);
            }
        }
        kg.init(b.build());
        return kg.generateKey();
    }

    private static SecretKey getKey() throws Exception {
        KeyStore ks = KeyStore.getInstance(KEYSTORE);
        ks.load(null);
        KeyStore.Entry entry = ks.getEntry(KEY_ALIAS, null);
        if (entry instanceof KeyStore.SecretKeyEntry) {
            return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
        }
        return null;
    }
}
