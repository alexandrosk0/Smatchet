#pragma once

// EGL context + window-surface RAII for the Android host shell. Deliberately free of
// any ImGui / Core include so it can be unit-reasoned in isolation and never leaks GLES
// headers into Core translation units (the host-injection contract — Core stays platform
// header-free). Owns the EGLDisplay / EGLContext for the process lifetime and a single
// EGLSurface bound to the current ANativeWindow; the surface is torn down + rebuilt across
// the Android APP_CMD_TERM_WINDOW / APP_CMD_INIT_WINDOW cycle while the context survives.

#include <EGL/egl.h>

struct ANativeWindow;

class SmatchetAndroidEgl {
public:
    SmatchetAndroidEgl();
    ~SmatchetAndroidEgl();

    SmatchetAndroidEgl(const SmatchetAndroidEgl&) = delete;
    SmatchetAndroidEgl& operator=(const SmatchetAndroidEgl&) = delete;

    // Open the default display, choose a GLES3 RGB8+D24 config, and create the context.
    // No surface yet (the window may not exist at boot). Returns false + logs on failure.
    bool CreateContext();

    // Create the window surface for `window` and make the context current on it. Applies
    // the EGL-native visual to the ANativeWindow first. Safe to call again after
    // DestroySurface (window recreate). Returns false + logs on failure.
    bool CreateSurface(ANativeWindow* window);

    // Release the current surface (context stays alive). No-op when no surface is bound.
    void DestroySurface();

    // Tear down surface + context + display. Idempotent.
    void Destroy();

    // Present the back buffer. Returns false on EGL error (e.g. surface lost). After a false
    // return, LastSwapLostContext() reports whether the cause was EGL_CONTEXT_LOST.
    bool SwapBuffers();

    // True iff the most recent SwapBuffers() failed with EGL_CONTEXT_LOST — a GPU reset / power
    // event that voids the EGLContext and every GL object it owned. The host must rebuild the
    // context + surface and re-init the GL backend (a benign transient surface error does not set
    // this). Cleared at the start of each SwapBuffers(). Item 12 (#1071).
    bool LastSwapLostContext() const { return lastSwapContextLost_; }

    bool HasContext() const { return context_ != EGL_NO_CONTEXT; }
    bool HasSurface() const { return surface_ != EGL_NO_SURFACE; }
    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    EGLDisplay display_;
    EGLConfig config_;
    EGLContext context_;
    EGLSurface surface_;
    int width_;
    int height_;
    bool lastSwapContextLost_ = false; // set by SwapBuffers() on an EGL_CONTEXT_LOST present failure
};
