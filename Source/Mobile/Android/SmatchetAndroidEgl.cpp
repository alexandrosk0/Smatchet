#include "SmatchetAndroidEgl.h"

#include "SmatchetAndroidPlatform.h" // SLOG / SLOGE (logcat host-logging facility)

#include <EGL/egl.h>
#include <android/native_window.h>

SmatchetAndroidEgl::SmatchetAndroidEgl()
    : display_(EGL_NO_DISPLAY), config_(nullptr), context_(EGL_NO_CONTEXT), surface_(EGL_NO_SURFACE), width_(0),
      height_(0) {}

SmatchetAndroidEgl::~SmatchetAndroidEgl() { Destroy(); }

bool SmatchetAndroidEgl::CreateContext() {
    if (context_ != EGL_NO_CONTEXT) {
        return true;
    }

    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        SLOGE("eglGetDisplay returned EGL_NO_DISPLAY");
        return false;
    }
    if (eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
        SLOGE("eglInitialize failed (0x%x)", eglGetError());
        display_ = EGL_NO_DISPLAY;
        return false;
    }

    // GLES3, 24-bit RGB + 24-bit depth, window-renderable. ES3 is guaranteed on API 24.
    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE,
                                    EGL_OPENGL_ES3_BIT,
                                    EGL_SURFACE_TYPE,
                                    EGL_WINDOW_BIT,
                                    EGL_BLUE_SIZE,
                                    8,
                                    EGL_GREEN_SIZE,
                                    8,
                                    EGL_RED_SIZE,
                                    8,
                                    EGL_DEPTH_SIZE,
                                    24,
                                    EGL_NONE};
    EGLint numConfigs = 0;
    if (eglChooseConfig(display_, configAttribs, &config_, 1, &numConfigs) != EGL_TRUE || numConfigs < 1) {
        SLOGE("eglChooseConfig found no matching config (0x%x)", eglGetError());
        Destroy();
        return false;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        SLOGE("eglCreateContext failed (0x%x)", eglGetError());
        Destroy();
        return false;
    }
    SLOG("EGL context created (GLES3)");
    return true;
}

bool SmatchetAndroidEgl::CreateSurface(ANativeWindow* window) {
    if (display_ == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT) {
        SLOGE("CreateSurface called before CreateContext");
        return false;
    }
    if (window == nullptr) {
        SLOGE("CreateSurface called with null ANativeWindow");
        return false;
    }
    DestroySurface();

    // Match the ANativeWindow buffer format to the EGL config's native visual id before
    // creating the surface — required by EGL on Android, else eglCreateWindowSurface fails.
    EGLint visualId = 0;
    eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &visualId);
    ANativeWindow_setBuffersGeometry(window, 0, 0, visualId);

    surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        SLOGE("eglCreateWindowSurface failed (0x%x)", eglGetError());
        return false;
    }
    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
        SLOGE("eglMakeCurrent failed (0x%x)", eglGetError());
        DestroySurface();
        return false;
    }

    eglQuerySurface(display_, surface_, EGL_WIDTH, &width_);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &height_);
    SLOG("EGL surface created (%dx%d)", width_, height_);
    return true;
}

void SmatchetAndroidEgl::DestroySurface() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    width_ = 0;
    height_ = 0;
}

void SmatchetAndroidEgl::Destroy() {
    DestroySurface();
    if (display_ != EGL_NO_DISPLAY) {
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
    config_ = nullptr;
}

bool SmatchetAndroidEgl::SwapBuffers() {
    if (display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE) {
        return false;
    }
    return eglSwapBuffers(display_, surface_) == EGL_TRUE;
}
