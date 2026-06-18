package com.smatchet.mobile;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.util.Base64;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;

import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

/**
 * JVM/Robolectric harness for {@link SmatchetSecretStore} — the Java half of the AndroidKeyStore
 * secret-at-rest seam (audit H2), the Android analogue of the desktop DPAPI seal.
 *
 * <p>The AndroidKeyStore provider is not available on the JVM, so the AES-GCM CORE ({@code seal} /
 * {@code open}) is exercised with an injected SOFTWARE AES-256 key — that pins the IV-prepend framing,
 * the round-trip, the fresh-nonce-per-seal property, and the AEAD tamper rejection independent of any
 * device key. The public {@code protect} / {@code unprotect} entry points (which DO touch the
 * AndroidKeyStore) are covered only for their environment-independent FAIL-SAFE contract: they must
 * never throw and must yield "" on bad input. The live on-device KeyStore round-trip belongs to the
 * instrumented/emulator harness.
 */
@RunWith(RobolectricTestRunner.class)
public class SmatchetSecretStoreTest {

    private static SecretKey softwareKey() throws Exception {
        KeyGenerator kg = KeyGenerator.getInstance("AES");
        kg.init(256);
        return kg.generateKey();
    }

    @Test
    public void sealOpenRoundTripsExactly() throws Exception {
        SecretKey key = softwareKey();
        String plain = "sk-ant-secret-éñ-12345"; // includes non-ASCII to pin UTF-8 handling
        String token = SmatchetSecretStore.seal(key, plain);
        assertEquals(plain, SmatchetSecretStore.open(key, token));
    }

    @Test
    public void tokenIsOpaqueBase64NotThePlaintext() throws Exception {
        SecretKey key = softwareKey();
        String plain = "anthropic-api-key-xyz";
        String token = SmatchetSecretStore.seal(key, plain);
        assertNotNull(token);
        assertFalse("token must not contain the plaintext", token.contains(plain));
        // IV(12) + ciphertext(=plaintext len) + GCM tag(16), base64'd.
        byte[] raw = Base64.decode(token, Base64.NO_WRAP);
        assertEquals(12 + plain.getBytes("UTF-8").length + 16, raw.length);
    }

    @Test
    public void eachSealUsesAFreshNonceSoCiphertextDiffers() throws Exception {
        SecretKey key = softwareKey();
        String plain = "same-input-every-time";
        String a = SmatchetSecretStore.seal(key, plain);
        String b = SmatchetSecretStore.seal(key, plain);
        // Fresh 12-byte GCM nonce per encryption => identical plaintext yields distinct tokens.
        assertNotEquals(a, b);
        // Both still decrypt back to the same plaintext.
        assertEquals(plain, SmatchetSecretStore.open(key, a));
        assertEquals(plain, SmatchetSecretStore.open(key, b));
    }

    @Test
    public void tamperedCiphertextIsRejectedByTheAeadTag() throws Exception {
        SecretKey key = softwareKey();
        String token = SmatchetSecretStore.seal(key, "do-not-tamper");
        byte[] raw = Base64.decode(token, Base64.NO_WRAP);
        raw[raw.length - 1] ^= 0x01; // flip a bit in the GCM tag
        String tampered = Base64.encodeToString(raw, Base64.NO_WRAP);
        try {
            SmatchetSecretStore.open(key, tampered);
            fail("expected an AEAD tag-mismatch exception on tampered ciphertext");
        } catch (Exception expected) {
            // javax.crypto.AEADBadTagException (a BadPaddingException) — the core surfaces the failure;
            // the public unprotect() wrapper is what converts this into the fail-safe "" below.
        }
    }

    @Test
    public void openOfTooShortTokenReturnsEmpty() throws Exception {
        SecretKey key = softwareKey();
        // Fewer bytes than a 12-byte IV: cannot be our token -> empty, not an exception.
        String stub = Base64.encodeToString(new byte[] {1, 2, 3}, Base64.NO_WRAP);
        assertEquals("", SmatchetSecretStore.open(key, stub));
    }

    @Test
    public void publicEntryPointsAreFailSafeOnEmptyAndGarbage() {
        // Environment-independent fail-safe contract: never throws; empty in -> empty out; a foreign /
        // undecodable token -> "" (AndroidKeyStore may be absent on the JVM; either way no throw).
        assertEquals("", SmatchetSecretStore.protect(null));
        assertEquals("", SmatchetSecretStore.protect(""));
        assertEquals("", SmatchetSecretStore.unprotect(null));
        assertEquals("", SmatchetSecretStore.unprotect(""));
        assertEquals("", SmatchetSecretStore.unprotect("!!!not-base64!!!"));
        assertNotNull(SmatchetSecretStore.protect("some-secret")); // "" or a real token, never null/throw
    }

    @Test
    public void protectThenUnprotectRoundTripsWhenKeystoreIsAvailable() {
        // Best-effort end-to-end through the AndroidKeyStore path. Robolectric MAY back the
        // "AndroidKeyStore" provider; if it does, protect yields a non-empty token that unprotect
        // recovers. If it does not, protect fails safe to "" and we assert only that no value leaks.
        String plain = "mcp-auth-token-9f3a";
        String token = SmatchetSecretStore.protect(plain);
        if (token.isEmpty()) {
            return; // KeyStore unavailable in this JVM environment — fail-safe path, nothing to round-trip
        }
        assertFalse("token must not embed the plaintext", token.contains(plain));
        assertEquals(plain, SmatchetSecretStore.unprotect(token));
        assertTrue(true);
    }
}
