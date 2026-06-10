package com.smatchet.mobile;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * JVM/Robolectric harness for SmatchetActivity's IME / Unicode-queue bridge
 * (WS5 item 17, docs/plans/active/mobile-mvp-completion.md) — the Java half of the
 * paste path the native frame loop drains via {@code pollUnicodeChar()}.
 *
 * <p>Scope: the {@code unicodeQueue} + code-point logic only. The tests drive the
 * private {@code enqueueText(CharSequence)} — the single source of truth that BOTH
 * the IME {@code commitText} (clipboard paste) path and the key-event path feed —
 * and the public {@code pollUnicodeChar()} / {@code pollContentInsets()}. They do
 * NOT run the Activity lifecycle: {@link android.app.NativeActivity#onCreate} loads
 * the native shell {@code .so} and takes the window surface, neither of which the
 * pure queue logic needs, and skipping it keeps the harness independent of NDK
 * artifacts and of a booted window.
 *
 * <p>Out of JVM scope (covered by the WS5 emulator/bucket-E harness, item 18):
 * {@code dispatchKeyEvent} (its {@code super} call needs a real window) and the
 * native 64-char/frame drain in SmatchetAndroidImeBridge.cpp.
 *
 * <p>#1070 DONE (landed on develop via #1105): the unicodeQueue is now an UNBOUNDED
 * {@code LinkedBlockingQueue}, so a large paste is delivered in full and {@code offer()}
 * still never blocks the UI thread (unbounded offer returns immediately — the ANR guard).
 * {@code enqueueLargePasteIsNonBlockingAndLossless} is the full-delivery proof the plan's
 * §Verification calls for.
 */
@RunWith(RobolectricTestRunner.class)
public class SmatchetActivityImeTest {

    // buildActivity(...).get() allocates the instance inside the Robolectric sandbox
    // WITHOUT driving onCreate (a bare `new SmatchetActivity()` throws "Stub!" against
    // android.jar). The unicodeQueue field initializer runs at construction, so the
    // queue is live and empty; the inset fields default to 0.
    private SmatchetActivity newActivity() {
        return Robolectric.buildActivity(SmatchetActivity.class).get();
    }

    private void enqueueText(SmatchetActivity a, CharSequence text) throws Exception {
        Method m = SmatchetActivity.class.getDeclaredMethod("enqueueText", CharSequence.class);
        m.setAccessible(true);
        m.invoke(a, text);
    }

    private void setInset(SmatchetActivity a, String field, int value) throws Exception {
        Field f = SmatchetActivity.class.getDeclaredField(field);
        f.setAccessible(true);
        f.setInt(a, value);
    }

    @Test
    public void pollReturnsZeroWhenQueueEmpty() {
        assertEquals(0, newActivity().pollUnicodeChar());
    }

    @Test
    public void enqueueTextDrainsFifoThenZero() throws Exception {
        SmatchetActivity a = newActivity();
        enqueueText(a, "Hi");
        assertEquals('H', a.pollUnicodeChar());
        assertEquals('i', a.pollUnicodeChar());
        assertEquals(0, a.pollUnicodeChar());
    }

    @Test
    public void enqueueTextDeliversAstralCodePointAsOne() throws Exception {
        // U+1F600 GRINNING FACE is a UTF-16 surrogate pair ("😀"); the
        // codePointAt/charCount loop must deliver it as ONE 0x1F600, not two halves.
        SmatchetActivity a = newActivity();
        enqueueText(a, "😀X");
        assertEquals(0x1F600, a.pollUnicodeChar());
        assertEquals('X', a.pollUnicodeChar());
        assertEquals(0, a.pollUnicodeChar());
    }

    @Test
    public void enqueueTextSkipsNulCodePoint() throws Exception {
        // Code point 0 is the native "no character available" sentinel and must never
        // be queued (else pollUnicodeChar can't distinguish it from an empty queue).
        SmatchetActivity a = newActivity();
        enqueueText(a, "a\u0000b");
        assertEquals('a', a.pollUnicodeChar());
        assertEquals('b', a.pollUnicodeChar());
        assertEquals(0, a.pollUnicodeChar());
    }

    @Test
    public void enqueueWithinCapacityDeliversAllInOrder() throws Exception {
        // A 200-char paste: every char survives, in order. Pins the lossless,
        // order-preserving FIFO contract (#1070) for a mid-size paste.
        SmatchetActivity a = newActivity();
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 200; i++) {
            sb.append((char) ('A' + (i % 26)));
        }
        enqueueText(a, sb.toString());
        for (int i = 0; i < 200; i++) {
            assertEquals('A' + (i % 26), a.pollUnicodeChar());
        }
        assertEquals(0, a.pollUnicodeChar());
    }

    @Test
    public void enqueueLargePasteIsNonBlockingAndLossless() throws Exception {
        // #1070 (landed via #1105): the unicodeQueue is an UNBOUNDED LinkedBlockingQueue
        // fed via offer(), so a large paste is delivered in FULL while offer() still never
        // blocks the UI thread (unbounded offer() returns immediately — the ANR guard the
        // pre-#1070 bounded queue provided by dropping). This test completing at all proves
        // non-blocking; draining all 300 proves lossless.
        SmatchetActivity a = newActivity();
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 300; i++) {
            sb.append('x');
        }
        enqueueText(a, sb.toString());
        int drained = 0;
        while (a.pollUnicodeChar() != 0) {
            drained++;
        }
        assertEquals(300, drained);
    }

    @Test
    public void pollContentInsetsPacksFourUnsigned16BitFields() throws Exception {
        // pollContentInsets() packs [left:16][top:16][right:16][bottom:16] for the JNI
        // render loop; verify each lane round-trips at its bit offset.
        SmatchetActivity a = newActivity();
        setInset(a, "insetLeft", 0x0011);
        setInset(a, "insetTop", 0x0022);
        setInset(a, "insetRight", 0x0033);
        setInset(a, "insetBottom", 0x0044);
        long packed = a.pollContentInsets();
        assertEquals(0x0011L, (packed >>> 48) & 0xFFFF);
        assertEquals(0x0022L, (packed >>> 32) & 0xFFFF);
        assertEquals(0x0033L, (packed >>> 16) & 0xFFFF);
        assertEquals(0x0044L, packed & 0xFFFF);
    }
}
