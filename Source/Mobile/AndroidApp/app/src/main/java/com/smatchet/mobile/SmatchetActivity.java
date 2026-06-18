package com.smatchet.mobile;

import android.app.NativeActivity;
import android.content.Context;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.DisplayCutout;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.util.concurrent.LinkedBlockingQueue;

/**
 * Host Activity for the Smatchet mobile native shell.
 *
 * Extends {@link NativeActivity} so the NDK native_app_glue entry point
 * ({@code android_main}) drives the app. This subclass adds the soft-keyboard +
 * Unicode-character bridge that the native side resolves by name via JNI
 * (see SmatchetAndroidImeBridge): {@code showSoftInput()V},
 * {@code hideSoftInput()V}, {@code pollUnicodeChar()I}. Keep those three
 * signatures exactly in sync with the native bridge.
 *
 * <p>It also exposes the AndroidKeyStore secret-at-rest bridge (audit H2) that
 * SmatchetAndroidSecretBridge resolves by name:
 * {@code protectSecret(Ljava/lang/String;)Ljava/lang/String;} and
 * {@code unprotectSecret(Ljava/lang/String;)Ljava/lang/String;}, delegating to
 * {@link SmatchetSecretStore}. Keep those two signatures in sync with that bridge.
 */
public class SmatchetActivity extends NativeActivity {

    // Unicode code points produced by IME key events + committed text, drained by the native frame
    // loop one per call. Unbounded (item 9 / #1070): a clipboard paste is enqueued whole on the UI
    // thread, and a 256-cap silently dropped the tail of a long paste. offer() on an unbounded queue
    // never blocks the UI thread (Pillar 2), and the producers (key events + commitText) are
    // human-bounded — there is no runaway source that could grow it without limit.
    private final LinkedBlockingQueue<Integer> unicodeQueue = new LinkedBlockingQueue<>();

    // Cached safe-area insets in surface pixels (status bar / nav bar / display cutout). The native
    // render loop reads these via pollContentInsets() so the menu bar + dockspace stay out from
    // under the system chrome. volatile: written on the UI thread (inset dispatch), read on the
    // render thread (JNI).
    private volatile int insetLeft = 0;
    private volatile int insetTop = 0;
    private volatile int insetRight = 0;
    private volatile int insetBottom = 0;

    // Hidden focusable view that owns the IME InputConnection. NativeActivity's content is a native
    // surface view that can't be a text editor, so the soft keyboard is targeted at this instead.
    private View imeView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        installWindowInsetsListener();
        installImeCaptureView();
    }

    // Add a hidden, focusable 0-px view that returns our InputConnection (see SmatchetInputConnection)
    // so committed text — crucially clipboard paste, which the IME delivers via commitText rather
    // than key events — reaches the native queue. The native surface view cannot own an
    // InputConnection, so without this paste is silently dropped. Touch + native key input are
    // dispatched at window level and are unaffected by this view holding IME focus.
    private void installImeCaptureView() {
        imeView = new View(this) {
            @Override
            public boolean onCheckIsTextEditor() {
                return true;
            }

            @Override
            public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
                if (outAttrs != null) {
                    outAttrs.inputType =
                            InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
                    outAttrs.imeOptions =
                            EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
                }
                return new SmatchetInputConnection(this);
            }
        };
        imeView.setFocusable(true);
        imeView.setFocusableInTouchMode(true);
        // 1x1, not 0x0: apps targeting Android P+ require View.hasSize() (right>left && bottom>top) for
        // requestFocus() to succeed, so a zero-area view can never become the IME-served view and
        // showSoftInput() is silently ignored ("view ... is not served"). A single pixel in the top-left
        // corner is laid out and focusable while intercepting effectively no touches from the native surface.
        addContentView(imeView, new ViewGroup.LayoutParams(1, 1));
    }

    // Subscribe to window-inset changes on the decor view and cache the system-bar + display-cutout
    // insets. The native EGL surface fills the whole window (system-chrome area included), so the
    // render loop applies these as the ImGui main-viewport work area. Forwards to the default
    // handler so normal inset behavior is preserved.
    private void installWindowInsetsListener() {
        final View decor = getWindow().getDecorView();
        decor.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                int left;
                int top;
                int right;
                int bottom;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    Insets bars = insets.getInsets(
                            WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                    left = bars.left;
                    top = bars.top;
                    right = bars.right;
                    bottom = bars.bottom;
                } else {
                    left = insets.getSystemWindowInsetLeft();
                    top = insets.getSystemWindowInsetTop();
                    right = insets.getSystemWindowInsetRight();
                    bottom = insets.getSystemWindowInsetBottom();
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                        DisplayCutout cutout = insets.getDisplayCutout();
                        if (cutout != null) {
                            left = Math.max(left, cutout.getSafeInsetLeft());
                            top = Math.max(top, cutout.getSafeInsetTop());
                            right = Math.max(right, cutout.getSafeInsetRight());
                            bottom = Math.max(bottom, cutout.getSafeInsetBottom());
                        }
                    }
                }
                insetLeft = left;
                insetTop = top;
                insetRight = right;
                insetBottom = bottom;
                return v.onApplyWindowInsets(insets);
            }
        });
        decor.requestApplyInsets();
    }

    /**
     * Called from native code (JNI) each frame to read the cached safe-area insets, packed as four
     * unsigned 16-bit pixel counts: [left:16][top:16][right:16][bottom:16]. Recomputed only on
     * inset changes, so this is a cheap field read.
     */
    @SuppressWarnings("unused")
    public long pollContentInsets() {
        return ((long) (insetLeft & 0xFFFF) << 48)
                | ((long) (insetTop & 0xFFFF) << 32)
                | ((long) (insetRight & 0xFFFF) << 16)
                | (long) (insetBottom & 0xFFFF);
    }

    /**
     * Called from native code (JNI) to raise the soft keyboard. The native bridge calls this on the
     * WantTextInput rising edge AND on a re-tap of a focused field (item 11 / #1055), so it must
     * reliably re-raise after a Back/swipe dismiss. InputMethodManager.showSoftInput with flag 0 is
     * used on all releases: flag 0 is un-forced (SHOW_FORCED is retired — it forced the IME up even
     * when the system meant it hidden and left it stuck open), and unlike WindowInsetsController.show
     * it re-establishes the served-view connection, so the re-tap re-raise works even when the view
     * already holds focus (a no-op requestFocus that WindowInsetsController.show would not recover).
     */
    @SuppressWarnings("unused")
    public void showSoftInput() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                View view = imeView != null ? imeView : getWindow().getDecorView();
                view.requestFocus();
                InputMethodManager imm =
                        (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.showSoftInput(view, 0);
                }
            }
        });
    }

    /**
     * Called from native code (JNI) to dismiss the soft keyboard on the WantTextInput falling edge
     * (focus left a text widget). A user-initiated dismiss (Back / swipe-down) is handled by the IME
     * itself and never routes here; the keyboard then simply stays down because the native bridge has
     * no steady-state re-raise (item 10 / #1054) — it waits for a fresh re-tap (item 11).
     */
    @SuppressWarnings("unused")
    public void hideSoftInput() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                View view = imeView != null ? imeView : getWindow().getDecorView();
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    WindowInsetsController controller = view.getWindowInsetsController();
                    if (controller != null) {
                        controller.hide(WindowInsets.Type.ime());
                    }
                } else {
                    InputMethodManager imm =
                            (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                    if (imm != null) {
                        imm.hideSoftInputFromWindow(view.getWindowToken(), 0);
                    }
                }
            }
        });
    }

    /**
     * Called from native code (JNI) once per frame to drain a pending Unicode
     * code point. Returns 0 when the queue is empty (the native side treats 0
     * as "no character available").
     */
    @SuppressWarnings("unused")
    public int pollUnicodeChar() {
        Integer codePoint = unicodeQueue.poll();
        return codePoint != null ? codePoint : 0;
    }

    /**
     * Called from native code (JNI) to seal a config secret at rest with the AndroidKeyStore AES-GCM
     * key (see {@link SmatchetSecretStore}). Runs synchronously on the calling (native) thread — the
     * caller needs the return value and KeyStore ops do not require the UI thread. Returns the empty
     * string on any failure, which the native bridge treats as "do not persist" (fail-safe), so a
     * secret is never written in cleartext.
     */
    @SuppressWarnings("unused")
    public String protectSecret(String plain) {
        return SmatchetSecretStore.protect(plain);
    }

    /**
     * Called from native code (JNI) to recover a secret sealed by {@link #protectSecret}. Returns the
     * empty string on any failure or if the token was not produced by this device's key (fail-safe:
     * the native bridge / Core then treat the secret as absent rather than surfacing a partial value).
     */
    @SuppressWarnings("unused")
    public String unprotectSecret(String token) {
        return SmatchetSecretStore.unprotect(token);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int action = event.getAction();
        if (action == KeyEvent.ACTION_DOWN || action == KeyEvent.ACTION_MULTIPLE) {
            int codePoint = event.getUnicodeChar(event.getMetaState());
            if (codePoint != 0) {
                unicodeQueue.offer(codePoint);
            }
        }
        return super.dispatchKeyEvent(event);
    }

    private void enqueueText(CharSequence text) {
        if (text == null) {
            return;
        }
        int length = text.length();
        for (int i = 0; i < length; ) {
            int codePoint = Character.codePointAt(text, i);
            if (codePoint != 0) {
                unicodeQueue.offer(codePoint);
            }
            i += Character.charCount(codePoint);
        }
    }

    // BaseInputConnection in dummy-editor mode whose commitText enqueues each pasted/typed code
    // point into unicodeQueue (drained by the native pollUnicodeChar loop). Backspace / enter /
    // arrows still arrive as key events and are handled by the ImGui Android backend.
    private final class SmatchetInputConnection extends BaseInputConnection {
        SmatchetInputConnection(View targetView) {
            super(targetView, false);
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            // Enqueue directly and DON'T call super. In dummy-editor mode BaseInputConnection.commitText
            // synthesizes KeyEvents for committed text (sendCurrentText -> sendKeyEvent), which loop back
            // through SmatchetActivity.dispatchKeyEvent and enqueue every character a SECOND time —
            // doubling each keystroke. (Paste escaped this because multi-char text is sent as one
            // ACTION_MULTIPLE event whose getUnicodeChar() is 0, so it was never re-enqueued.) Enqueuing
            // here is the single source of truth for IME-committed characters; backspace / enter / arrows
            // still arrive as real key events handled by the ImGui Android backend.
            enqueueText(text);
            return true;
        }
    }
}
