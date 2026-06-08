// Android NativeActivity host shell for Smatchet (Phase-0 mobile MVP). This is the mobile
// sibling of Source/Standalone/StandaloneAppBootstrap.cpp: it boots the engine-agnostic
// Source/Core stack (AppController + PluginHost + SmatchetUI) and drives the same per-frame
// order (Drain -> Scenarios().Tick -> NewFrame -> Draw -> Render), but over EGL/GLES3 +
// the imgui_impl_android + imgui_impl_opengl3 backends instead of GLFW/desktop-GL.
//
// Lifecycle: Core boots once at android_main() start (window-independent). The EGL surface
// and ImGui are created on the first APP_CMD_INIT_WINDOW and the surface is torn down /
// rebuilt across the TERM_WINDOW / INIT_WINDOW cycle while the EGLContext (and therefore
// all GL objects + the ImGui context) survives. Text input is owned by the host JNI bridge
// (imgui_impl_android cannot raise the IME or emit characters — see SmatchetAndroidImeBridge).

#include "SmatchetAndroidEgl.h"
#include "SmatchetAndroidImeBridge.h"
#include "SmatchetAndroidPlatform.h"

#include "AppController.h"
#include "Commands/Scenarios/IScenario.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "PluginHost.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetTheme.h"
#include "SmatchetUI.h"

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

#include <android_native_app_glue.h>

#include <android/native_activity.h>

#include <GLES3/gl3.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

// Base TTF pixel size at mdpi (scale 1.0); multiplied by display density so glyphs stay
// finger-readable on high-dpi panels. Widget metrics are scaled separately via
// SmatchetTheme::ApplyUiDensityScale.
const float kBaseFontPx = 16.0f;

struct AndroidHostState {
    SmatchetAndroidEgl egl;
    smatchet::mobile::SmatchetAndroidImeBridge ime;
    std::unique_ptr<AppController> app;
    std::unique_ptr<PluginHost> pluginHost;
    std::unique_ptr<SmatchetUI> mainWindow;
    std::vector<unsigned char> fontBlob; // retained for process lifetime (injected by ref)
    std::string iniPath;                 // backs io.IniFilename for the context lifetime
    float densityScale = 1.0f;
    bool coreBooted = false;
    bool imguiReady = false;
    bool hasSurface = false;
};

std::string NormalizeDir(const char* raw) {
    std::string dir = (raw != nullptr && raw[0] != '\0') ? raw : ".";
    if (dir.back() != '/') {
        dir.push_back('/');
    }
    return dir;
}

// Boot the Source/Core stack once. Window-independent: runs at android_main() start so the
// app, sqlite cache and tracker backend are live before the first frame. Mirrors
// StandaloneAppBootstrap::InitAppAndPlugins minus the GLFW window + desktop plugins.
void BootCoreOnce(android_app* app, AndroidHostState& s) {
    if (s.coreBooted) {
        return;
    }
    const std::string dataDir = NormalizeDir(app->activity != nullptr ? app->activity->internalDataPath : nullptr);
    // Android cwd is "/" (read-only), so all writable paths must be absolute under the app's
    // private dir. Seam #14 points Core's shared-dir resolver at it; the explicit setters keep
    // the imgui.ini + config + sqlite db inside it.
    ConfigManager::SetPlatformSharedUserDataDirectoryOverride(dataDir);
    ConfigManager::SetUserDataDirectory(dataDir);
    ConfigManager::SetRuntimeAssetDirectory(dataDir);

    // Persistent log file in the app's private dir (host-parity with Standalone main.cpp). The
    // NativeActivity discards stdout/stderr and Core's LOG_* has no logcat sink, so without this
    // the only diagnostics are the in-memory ring buffer. Readable off-device via `run-as cat`.
    Logger::Instance().SetFileSinkPath(dataDir + "smatchet.log");

    // TLS trust anchor. libcurl (built against the NDK OpenSSL via cpr) has no CA bundle on a
    // stock Android device, so a default CURLOPT_SSL_VERIFYPEER handshake to Jira dies with
    // "unable to get local issuer certificate" (HTTP 0). The build bakes a compile-time
    // CURL_CA_BUNDLE define (cmake/SmatchetThirdParty.cmake) = libcurl's default CAINFO = this
    // exact private-dir path; all this code does is materialise the APK-shipped Mozilla cacert.pem
    // at that path so the baked default resolves. No per-request SslOptions edit in Core/Tracker
    // needed (proven live: read HTTP 200 + write PUT HTTP 204 against real Jira).
    if (app->activity != nullptr && app->activity->assetManager != nullptr) {
        const std::vector<unsigned char> caBytes =
            smatchet::mobile::ReadApkAsset(app->activity->assetManager, "certs/cacert.pem");
        if (!caBytes.empty()) {
            const std::string caPath = dataDir + "cacert.pem";
            std::ofstream caOut(caPath.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
            if (caOut.is_open()) {
                caOut.write(reinterpret_cast<const char*>(caBytes.data()),
                            static_cast<std::streamsize>(caBytes.size()));
                caOut.close();
                // Belt-and-suspenders env vars. The load-bearing trust anchor is the baked
                // compile-time CURL_CA_BUNDLE define (set in CMake) — this NDK-built libcurl reads
                // it as the default CAINFO and IGNORES the CURL_CA_BUNDLE *env* var, so the setenv
                // below is a no-op net for that name. SSL_CERT_FILE is OpenSSL's own, honored via
                // CURL_CA_FALLBACK as a secondary path should the baked CAINFO ever be unset. Both
                // are set so any future libcurl/cpr ordering change still resolves.
                setenv("CURL_CA_BUNDLE", caPath.c_str(), 1);
                setenv("SSL_CERT_FILE", caPath.c_str(), 1);
                SLOG("TLS CA bundle ready (%zu bytes) -> %s", caBytes.size(), caPath.c_str());
            } else {
                SLOGE("TLS CA bundle: failed to write %s (HTTPS will fail cert verify)", caPath.c_str());
            }
        } else {
            SLOGE("TLS CA bundle: certs/cacert.pem missing/empty in APK (HTTPS will fail cert verify)");
        }
    }

    TrackerConfig cfg = ConfigManager::Load();
    std::string dbPath = cfg.DbPath;
    if (dbPath.empty() || dbPath[0] != '/') {
        dbPath = dataDir + dbPath; // dataDir ends in '/'
    }

    try {
        s.app = std::unique_ptr<AppController>(new AppController());
        s.pluginHost = std::unique_ptr<PluginHost>(new PluginHost());
        s.app->SetRuntimePluginHost(s.pluginHost.get());
        s.app->SetRequestAppQuitHandler([app]() {
            if (app->activity != nullptr) {
                ANativeActivity_finish(app->activity);
            }
        });
        s.pluginHost->OnEarlyInit(*s.app);
        s.app->Initialize(dbPath, cfg.TrackerType);
        s.pluginHost->OnStart(*s.app);
        s.mainWindow = std::unique_ptr<SmatchetUI>(new SmatchetUI());
        s.coreBooted = true;
        SLOG("Core booted (db=%s tracker=%s)", dbPath.c_str(), cfg.TrackerType.c_str());
    } catch (const std::exception& ex) {
        SLOGE("Core boot failed: %s", ex.what());
    } catch (...) {
        SLOGE("Core boot failed: unknown exception");
    }
}

// One-time ImGui setup on the first INIT_WINDOW (context already current). Font bytes are
// injected from the APK BEFORE the first atlas build (seam #12); density scaling is applied
// AFTER the style so it is not wiped (seam #13 — persisted across later ApplyStyle by
// SmatchetTheme's ReapplyHostDensityScale).
void InitImGuiFirstTime(android_app* app, AndroidHostState& s) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    s.iniPath = ConfigManager::GetImGuiSettingsPath();
    {
        TrackerConfig bootCfg = ConfigManager::Load();
        if (bootCfg.LayoutSchemaVersion < ConfigManager::kCurrentLayoutSchemaVersion) {
            ConfigManager::WriteDefaultImGuiSettingsFile();
            bootCfg.LayoutSchemaVersion = ConfigManager::kCurrentLayoutSchemaVersion;
            ConfigManager::Save(bootCfg);
        } else {
            ConfigManager::EnsureDefaultImGuiSettingsFile();
        }
    }
    io.IniFilename = s.iniPath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // touch host: no hardware cursor

    ImGui::StyleColorsDark();

    s.densityScale = smatchet::mobile::ResolveDensityScale(app->config);
    const float fontPx = kBaseFontPx * (s.densityScale > 0.0f ? s.densityScale : 1.0f);
    s.fontBlob = smatchet::mobile::ReadApkAsset(app->activity->assetManager, "fonts/Roboto-Medium.ttf");
    if (!s.fontBlob.empty()) {
        SmatchetSetInjectedFontBytes(s.fontBlob.data(), static_cast<int>(s.fontBlob.size()), fontPx);
    }
    SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(io);
    SmatchetTheme::ApplyUiDensityScale(s.densityScale);

    ImGui_ImplAndroid_Init(app->window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    s.ime.Init(app->activity->vm, app->activity->clazz);
    s.imguiReady = true;
    SLOG("ImGui ready (density=%.2f font=%.1fpx)", s.densityScale, fontPx);
}

void RenderOneFrame(AndroidHostState& s) {
    if (!s.hasSurface || !s.imguiReady || !s.coreBooted) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    s.ime.ShowKeyboardIfNeeded(io);
    s.ime.PollUnicodeChars(io);

    s.app->mainThreadDispatcher.Drain();
    bool scenScrollActive = false;
    int scenScrollTarget = -1;
    s.app->Scenarios().Tick(*s.app, scenScrollActive, scenScrollTarget);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
    s.mainWindow->Draw(*s.app);
    s.pluginHost->OnDraw(*s.app);
    ImGui::Render();

    glViewport(0, 0, s.egl.Width(), s.egl.Height());
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    glClearColor(bg.x * bg.w, bg.y * bg.w, bg.z * bg.w, bg.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    s.egl.SwapBuffers();
}

void TeardownAll(AndroidHostState& s) {
    if (s.imguiReady) {
        s.ime.Shutdown();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
    }
    if (s.coreBooted && s.app && s.pluginHost) {
        s.app->ClearAutomationLogSinks();
        s.app->SetRuntimePluginHost(nullptr);
        s.pluginHost->OnStop();
    }
    s.mainWindow.reset();
    s.app.reset();
    s.pluginHost.reset();
    if (s.imguiReady) {
        ImGui::DestroyContext();
    }
    s.egl.Destroy();
    s.imguiReady = false;
    s.coreBooted = false;
    s.hasSurface = false;
    SLOG("Teardown complete");
}

void OnAppCmd(android_app* app, int32_t cmd) {
    AndroidHostState& s = *static_cast<AndroidHostState*>(app->userData);
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window == nullptr || !s.coreBooted) {
            break;
        }
        if (!s.imguiReady) {
            if (s.egl.CreateContext() && s.egl.CreateSurface(app->window)) {
                InitImGuiFirstTime(app, s);
                s.hasSurface = s.imguiReady;
            }
        } else if (s.egl.CreateSurface(app->window)) {
            // Context (+ GL objects + ImGui ctx) survived TERM_WINDOW; only re-point the
            // android backend at the fresh ANativeWindow so NewFrame reads the right size.
            ImGui_ImplAndroid_Init(app->window);
            s.hasSurface = true;
        }
        break;
    case APP_CMD_TERM_WINDOW:
        s.hasSurface = false;
        s.egl.DestroySurface(); // keep context + GL objects + ImGui ctx alive
        break;
    default:
        break;
    }
}

int32_t OnInputEvent(android_app* app, AInputEvent* event) {
    AndroidHostState& s = *static_cast<AndroidHostState*>(app->userData);
    if (!s.imguiReady) {
        return 0;
    }
    return ImGui_ImplAndroid_HandleInputEvent(event);
}

} // namespace

void android_main(android_app* app) {
    AndroidHostState state;
    app->userData = &state;
    app->onAppCmd = OnAppCmd;
    app->onInputEvent = OnInputEvent;

    BootCoreOnce(app, state);

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;
        // Spin non-blocking while a surface is live (render every loop); block when
        // backgrounded so an idle app consumes no CPU until the next lifecycle event.
        // The timeout MUST be re-evaluated on every poll: an INIT_WINDOW processed inside
        // this drain flips hasSurface true, and the very next poll has to switch to the
        // non-blocking (0) timeout so the loop exits and renders instead of blocking on -1.
        while (ALooper_pollOnce(state.hasSurface ? 0 : -1, nullptr, &events,
                               reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) {
                source->process(app, source);
            }
            if (app->destroyRequested != 0) {
                TeardownAll(state);
                return;
            }
        }
        if (state.hasSurface) {
            RenderOneFrame(state);
        }
    }
}
