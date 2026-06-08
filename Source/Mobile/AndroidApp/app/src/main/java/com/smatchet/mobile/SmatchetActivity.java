package com.smatchet.mobile;

import android.app.NativeActivity;
import android.content.Context;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.View;
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
 */
public class SmatchetActivity extends NativeActivity {

    // Unicode code points produced by IME key events, drained by the native
    // frame loop one per call. Bounded but generous; offer() drops on overflow
    // rather than blocking the UI thread.
    private final LinkedBlockingQueue<Integer> unicodeQueue = new LinkedBlockingQueue<>(256);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }

    /** Called from native code (JNI) to raise the soft keyboard. */
    @SuppressWarnings("unused")
    public void showSoftInput() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                View view = getWindow().getDecorView();
                view.requestFocus();
                InputMethodManager imm =
                        (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.showSoftInput(view, InputMethodManager.SHOW_FORCED);
                }
            }
        });
    }

    /** Called from native code (JNI) to dismiss the soft keyboard. */
    @SuppressWarnings("unused")
    public void hideSoftInput() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                View view = getWindow().getDecorView();
                InputMethodManager imm =
                        (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.hideSoftInputFromWindow(view.getWindowToken(), 0);
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
}
