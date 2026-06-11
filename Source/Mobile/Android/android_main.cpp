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
#include "Tracker/TrackerHttpPure.h"
#include "Ui/SmatchetImGuiTextureGuard.h"

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

#include <android_native_app_glue.h>

#include <android/native_activity.h>

#include <GLES3/gl3.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

// Base TTF pixel size at mdpi (scale 1.0); multiplied by display density so glyphs stay
// finger-readable on high-dpi panels. Widget metrics are scaled separately via
// SmatchetTheme::ApplyUiDensityScale.
const float kBaseFontPx = 16.0f;

// item 14: process-death save-state blob handed to native_app_glue on APP_CMD_SAVE_STATE and read
// back on the next cold start. Layout (imgui.ini) + config already write through to the private dir
// — and SAVE_STATE force-flushes the ini below — so this blob only carries a sanity sentinel + a
// resume counter (proves the save/restore round-trip and is visible in logcat). Future fields append
// behind a version bump; a magic/version mismatch is rejected as a fresh start.
const uint32_t kSavedStateMagic = 0x534D4348u; // 'SMCH'
const uint32_t kSavedStateVersion = 1u;
struct SmatchetSavedState {
    uint32_t magic;
    uint32_t version;
    uint32_t resumeCount;
};

// ── Raw-GLES boot/error panel (WS3 item 7 / Issue #1053 — Quality Pillar 2 visible cue) ──────────
// When Source/Core fails to boot, coreBooted stays false and ImGui is never initialised, so the
// normal RenderOneFrame path cannot draw anything — the screen would otherwise stay black with no
// hint of what happened. This panel renders WITHOUT ImGui (a solid fill plus a 5x7 bitmap message
// drawn as GLES3 quads), so a boot failure shows a readable cue instead. The failure detail lands
// in logcat (tag Smatchet) and the app's smatchet.log; the on-screen message points there.

// 5x7 uppercase glyph: 7 rows top-first, low 5 bits per row (bit4 = leftmost column). Only the
// letters used by the two fixed panel strings are defined; any other char renders as a blank cell.
struct PanelGlyph {
    char ch;
    unsigned char rows[7];
};

const PanelGlyph kPanelFont[] = {
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
};

// Glyph rows for `ch`, or nullptr if undefined (e.g. space) — undefined cells draw nothing.
const unsigned char* PanelGlyphRows(char ch) {
    for (const PanelGlyph& g : kPanelFont) {
        if (g.ch == ch) {
            return g.rows;
        }
    }
    return nullptr;
}

// Append two NDC triangles (6 verts, x/y interleaved) for an axis-aligned quad whose top-left is
// (x, y) and which extends +w right / -h down.
void AppendPanelQuad(std::vector<float>& v, float x, float y, float w, float h) {
    const float x1 = x + w;
    const float y1 = y - h;
    const float q[12] = {x, y, x1, y, x1, y1, x, y, x1, y1, x, y1};
    v.insert(v.end(), q, q + 12);
}

// Append the lit-pixel quads for one centred text line. `topY` = NDC y of the line's top edge;
// `pxX`/`pxY` = NDC size of one glyph pixel; glyph cells are 6 px wide (5 + 1 spacing).
void AppendPanelLine(std::vector<float>& v, const char* s, float topY, float pxX, float pxY) {
    if (s == nullptr || s[0] == '\0') {
        return;
    }
    const size_t len = std::strlen(s);
    const float lineW = static_cast<float>(len * 6 - 1) * pxX;
    const float startX = -lineW * 0.5f;
    for (size_t k = 0; k < len; ++k) {
        const unsigned char* rows = PanelGlyphRows(s[k]);
        if (rows == nullptr) {
            continue;
        }
        const float gx = startX + static_cast<float>(k * 6) * pxX;
        for (int r = 0; r < 7; ++r) {
            for (int c = 0; c < 5; ++c) {
                if ((rows[r] >> (4 - c)) & 1) {
                    AppendPanelQuad(v, gx + static_cast<float>(c) * pxX, topY - static_cast<float>(r) * pxY, pxX, pxY);
                }
            }
        }
    }
}

// Owns the GLES3 program + dynamic VBO used to paint the boot/error panel. Init is lazy (first
// Draw) so nothing GL happens unless Core actually fails. GL objects live on the EGLContext, so a
// surface recreate (TERM/INIT_WINDOW) keeps them; Destroy() runs at teardown for hygiene.
class BootPanelGl {
public:
    void Draw(int w, int h, const char* line1, const char* line2);
    void Destroy();

    // item 12: drop the GL handles WITHOUT glDelete — used after EGL_CONTEXT_LOST, where the old
    // context (and every object it owned) is already gone, so the names are stale and must not be
    // deleted against the freshly created context. Re-arms lazy program creation on the next Draw.
    void Invalidate() {
        program_ = 0;
        vbo_ = 0;
        colorLoc_ = -1;
        programFailed_ = false;
    }

private:
    bool EnsureProgram();

    GLuint program_ = 0;
    GLuint vbo_ = 0;
    GLint colorLoc_ = -1;
    bool programFailed_ = false;
};

GLuint CompilePanelShader(GLenum type, const char* src) {
    const GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[256] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        SLOGE("BootPanel shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

bool BootPanelGl::EnsureProgram() {
    if (program_ != 0) {
        return true;
    }
    if (programFailed_) {
        return false;
    }
    const char* kVtx = "#version 300 es\n"
                       "layout(location=0) in vec2 aPos;\n"
                       "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    const char* kFrag = "#version 300 es\n"
                        "precision mediump float;\n"
                        "uniform vec4 uColor;\n"
                        "out vec4 fragColor;\n"
                        "void main(){ fragColor = uColor; }\n";
    const GLuint vs = CompilePanelShader(GL_VERTEX_SHADER, kVtx);
    const GLuint fs = CompilePanelShader(GL_FRAGMENT_SHADER, kFrag);
    if (vs == 0 || fs == 0) {
        programFailed_ = true; // solid fill alone is still a cue; never retry-spin on a bad driver
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        SLOGE("BootPanel program link failed");
        glDeleteProgram(program_);
        program_ = 0;
        programFailed_ = true;
        return false;
    }
    colorLoc_ = glGetUniformLocation(program_, "uColor");
    return true;
}

void BootPanelGl::Draw(int w, int h, const char* line1, const char* line2) {
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.18f, 0.02f, 0.02f, 1.0f); // dark maroon = "something went wrong"
    glClear(GL_COLOR_BUFFER_BIT);
    if (!EnsureProgram() || w <= 0 || h <= 0) {
        return; // solid maroon fill is itself a visible cue when text can't render
    }
    const size_t len1 = (line1 != nullptr) ? std::strlen(line1) : 0;
    const size_t len2 = (line2 != nullptr) ? std::strlen(line2) : 0;
    const size_t longest = len1 > len2 ? len1 : len2;
    if (longest == 0) {
        return;
    }
    // Size one glyph pixel so the longest line spans ~75% of the surface width; square the pixel in
    // device space by scaling the y extent by the aspect ratio.
    const float pxX = 1.5f / static_cast<float>(longest * 6 - 1);
    const float pxY = pxX * (static_cast<float>(w) / static_cast<float>(h));
    const float blockH = 17.0f * pxY; // line1 (7) + gap (3) + line2 (7)
    const float topY = blockH * 0.5f;
    std::vector<float> verts;
    AppendPanelLine(verts, line1, topY, pxX, pxY);
    AppendPanelLine(verts, line2, topY - 10.0f * pxY, pxX, pxY);
    if (verts.empty()) {
        return;
    }
    glUseProgram(program_);
    if (colorLoc_ >= 0) {
        glUniform4f(colorLoc_, 0.95f, 0.95f, 0.95f, 1.0f);
    }
    if (vbo_ == 0) {
        glGenBuffers(1, &vbo_);
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), reinterpret_cast<const void*>(0));
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 2));
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

void BootPanelGl::Destroy() {
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

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
    bool imeReady = false; // item 8 (#1069): IME bridge init succeeded — false ⇒ degraded cue
    bool hasSurface = false;
    bool bootPanelDrawn = false; // item 7: raw-GL boot/error panel painted once on the live surface
    bool hasFocus = true;        // item 13: APP_CMD_GAINED_FOCUS/LOST_FOCUS — false ⇒ pause render
    bool paused = false;         // item 13: APP_CMD_PAUSE/RESUME — true ⇒ backgrounded, pause render
    uint32_t resumeCount = 0;    // item 14: bumped on each restore from a process-death save-state
    BootPanelGl bootPanel;       // item 7: raw-GLES boot/error renderer (used only when Core fails)
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

    // TLS trust anchor (WS2 / Issue #1068). libcurl (built against the NDK OpenSSL via cpr) has
    // no CA bundle on a stock Android device, so a default CURLOPT_SSL_VERIFYPEER handshake to
    // Jira dies with "unable to get local issuer certificate" (HTTP 0). We materialise the
    // APK-shipped Mozilla cacert.pem into the app's private dir, then feed that path into Core's
    // runtime trust seam via TrackerHttpPure::SetCaBundlePath — every tracker verb then applies it
    // as an explicit CURLOPT_CAINFO (TrackerHttpUtils::MakeTrackerSslOptions). This runs before
    // ConfigManager::Load / AppController::Initialize below, i.e. before the first HTTPS request.
    //
    // The runtime seam is now the load-bearing trust anchor (replaces the prior reliance on the
    // baked compile-time CURL_CA_BUNDLE define + the CURL_CA_BUNDLE *env* var, which this NDK-built
    // libcurl ignores). The compile-time define is demoted to a documented defense-in-depth
    // fallback (cmake/SmatchetThirdParty.cmake). SSL_CERT_FILE is OpenSSL's own env var, kept as a
    // secondary belt-and-suspenders path. The seam only ever ADDS a CAINFO; it never disables
    // verification (peer/host verify stay on — cpr defaults). Proven live earlier: read HTTP 200 +
    // write PUT HTTP 204 against real Jira.
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
                // Load-bearing: hand the cafile to Core's per-request CURLOPT_CAINFO seam. Set once
                // here, before Core boots; read by every tracker verb for the process lifetime.
                TrackerHttpPure::SetCaBundlePath(caPath);
                // Secondary: OpenSSL's own env var, honored via CURL_CA_FALLBACK should the explicit
                // CAINFO ever be unset.
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

// Bridge used by PollWorkAreaInsets, an ImGui C callback with no user-data slot. Set once in
// InitImGuiFirstTime; both the bridge (a member of the android_main-local state) and the ImGui
// context live for the whole process, so this raw pointer never dangles.
smatchet::mobile::SmatchetAndroidImeBridge* g_insetBridge = nullptr;

// ImGui safe-area hook. ImGui invokes this for the main viewport inside NewFrame
// (UpdateViewportsNewFrame) and folds the result into the viewport work-inset, so both
// DockSpaceOverViewport (WorkPos/WorkSize) and BeginMainMenuBar (build work rect) lay out clear of
// the system chrome — without it the menu bar draws at y=0 under the status bar / cutout and is
// unreachable. ImVec4 = (left, top, right, bottom) in surface pixels (1:1 with DisplaySize here);
// ImGui asserts each >= 0, which the unsigned-packed insets always satisfy.
ImVec4 PollWorkAreaInsets(ImGuiViewport*) {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    if (g_insetBridge != nullptr) {
        g_insetBridge->PollContentInsets(left, top, right, bottom);
    }
    return ImVec4(static_cast<float>(left), static_cast<float>(top), static_cast<float>(right),
                  static_cast<float>(bottom));
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
    // item 8 (#1069): capture IME-bridge init. On failure the UI must still come up (do NOT gate
    // imguiReady) — only the soft keyboard + clipboard paste are lost; hardware keys still work.
    // RenderOneFrame paints a degraded-mode cue while imeReady is false.
    s.imeReady = s.ime.Init(app->activity->vm, app->activity->clazz);
    if (!s.imeReady) {
        SLOGE("IME bridge init failed — soft keyboard + paste unavailable; hardware keys still work");
    }
    g_insetBridge = &s.ime;
    ImGui::GetPlatformIO().Platform_GetWindowWorkAreaInsets = &PollWorkAreaInsets;
    s.imguiReady = true;
    SLOG("ImGui ready (density=%.2f font=%.1fpx)", s.densityScale, fontPx);
}

// item 12 (#1071): full GL-layer rebuild after an EGL_CONTEXT_LOST present failure. A GPU reset /
// power-management event voids the EGLContext and every GL object it owned (ImGui's font texture +
// shaders, the boot-panel program/VBO), so we shut the GL backend (its glDelete calls are harmless
// no-ops on the lost context), drop the boot-panel's GL names WITHOUT glDelete (they're stale), tear
// down EGL, then recreate the context, bind a fresh surface to the live window, and re-init both
// ImGui backends. ImGui's CPU-side context (layout + font atlas bytes) survives — ImplOpenGL3_Init
// rebuilds the shader/VBO device objects immediately, while the font texture re-uploads lazily on the
// next frame through ImGui 1.92's ImTextureData WantCreate pass in RenderDrawData (and the #1122
// texture guard in RenderOneFrame re-arms any texture that still reaches render Invalid). Returns true
// once a usable surface is back. If no live window / surface can be obtained (a TERM_WINDOW raced in,
// or a driver-side create failure), ImGui is fully torn down + imguiReady cleared so the next
// APP_CMD_INIT_WINDOW does
// a clean InitImGuiFirstTime rather than a half re-init.
bool RecreateAfterContextLoss(android_app* app, AndroidHostState& s) {
    SLOGE("EGL_CONTEXT_LOST on present — rebuilding GL context + surface (item 12)");
    const bool hadImgui = s.imguiReady;
    if (hadImgui) {
        ImGui_ImplOpenGL3_Shutdown();
    }
    s.bootPanel.Invalidate();
    s.hasSurface = false;
    s.egl.Destroy();

    if (app->window != nullptr && s.egl.CreateContext() && s.egl.CreateSurface(app->window)) {
        ImGui_ImplOpenGL3_Init("#version 300 es");
        ImGui_ImplAndroid_Init(app->window);
        s.hasSurface = true;
        SLOG("EGL context + surface rebuilt after loss (%dx%d)", s.egl.Width(), s.egl.Height());
        return true;
    }

    SLOGE("EGL_CONTEXT_LOST recovery deferred — no usable window/surface yet; full ImGui teardown");
    if (hadImgui) {
        s.ime.Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
    }
    s.imguiReady = false;
    s.imeReady = false;
    return false;
}

// Pin the pure guard's status constants to ImGui's enum so the predicate (compiled into the Windows
// doctest rig, which has no imgui.h) can never silently drift from the values this TU reasons about.
static_assert(static_cast<int>(smatchet::ui::kTextureGuardStatus_OK) == static_cast<int>(ImTextureStatus_OK) &&
                  static_cast<int>(smatchet::ui::kTextureGuardStatus_Destroyed) ==
                      static_cast<int>(ImTextureStatus_Destroyed) &&
                  static_cast<int>(smatchet::ui::kTextureGuardStatus_WantCreate) ==
                      static_cast<int>(ImTextureStatus_WantCreate) &&
                  static_cast<int>(smatchet::ui::kTextureGuardStatus_WantUpdates) ==
                      static_cast<int>(ImTextureStatus_WantUpdates) &&
                  static_cast<int>(smatchet::ui::kTextureGuardStatus_WantDestroy) ==
                      static_cast<int>(ImTextureStatus_WantDestroy),
              "SmatchetImGuiTextureGuard status constants drifted from ImGui's ImTextureStatus enum");

// [P0 #1122] ImGui 1.92 dynamic-texture safety pass. Runs after ImGui::Render() and before the GL3
// backend reads any draw command's texture id. ImGui 1.92 owns texture lifetime via ImTextureData;
// two paths can leave a committed ImDrawCmd pointing at a texture whose backend TexID is Invalid when
// the command loop reads it, which ImDrawCmd::GetTexID() asserts on (SIGABRT, "Backend must call
// ImTextureData::SetTexID()..."):
//
//   (1) Context-loss re-arm (list-walk). After an EGL/activity recreate (RecreateAfterContextLoss,
//       INIT_WINDOW re-init, background->foreground) a font texture can reach render Destroyed with
//       TexID=Invalid: the core's Destroyed->WantCreate self-heal is skipped when WantDestroyNextFrame
//       is set, and the GL3 backend's UpdateTexture has no Destroyed branch. If its CPU pixels survive,
//       force WantCreate so the backend re-uploads it during its Textures[] pass — which runs in the
//       same RenderDrawData call, before the draw commands sample the texture. (Validated on device via
//       the forced EGL_CONTEXT_LOST path.)
//
//   (2) Orphaned-command repoint (command-walk). A mid-frame dynamic-atlas grow / font re-apply
//       (on-demand glyph baking when a new tab/label first renders) retires the OLD atlas texture while
//       a command was already committed on it this frame; by the command loop that texture's id is
//       Invalid (destroyed in the Textures[] pass, or removed from the texture list entirely). Path (1)
//       only walks drawData->Textures and never the per-command TexRef, so it structurally misses an
//       orphan that is no longer in the list. Walk the committed commands and repoint any doomed one
//       (SmatchetDrawCmdTextureNeedsRebind: an Invalid id the upload pass will NOT validate this frame)
//       at the live atlas texture, which that pass makes valid. The walk is over draw COMMANDS (tens to
//       hundreds per frame, not vertices), so the per-frame cost is negligible (Quality Pillar 1).
//
// A repoint changes only the command's TexRef; its vertices keep their old UVs, so the worst case is
// one frame of cosmetically-wrong glyphs on a rare transition — never a crash. Confirmed on a physical
// Pixel for the rotate->dock-tab->tap-Tickets repro (#1122).
void GuardImGuiDynamicTextures(ImDrawData* drawData) {
    if (drawData == nullptr) {
        return;
    }
    // (1) context-loss re-arm: resurrect any in-list texture left Invalid with its CPU pixels intact.
    if (drawData->Textures != nullptr) {
        for (ImTextureData* tex : *drawData->Textures) {
            if (tex != nullptr && tex->GetTexID() == ImTextureID_Invalid &&
                tex->Status != ImTextureStatus_WantCreate && tex->Pixels != nullptr) {
                tex->SetStatus(ImTextureStatus_WantCreate); // backend re-uploads it this same frame
            }
        }
    }
    // (2) orphaned-command repoint: repoint any committed command whose texture the upload pass will
    // leave Invalid onto the live atlas (which that same pass makes valid this frame).
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImTextureData* liveAtlas = atlas != nullptr ? atlas->TexData : nullptr;
    if (liveAtlas == nullptr) {
        return;
    }
    int repointed = 0;
    for (int li = 0; li < drawData->CmdListsCount; ++li) {
        ImDrawList* dl = drawData->CmdLists[li];
        if (dl == nullptr) {
            continue;
        }
        for (ImDrawCmd& cmd : dl->CmdBuffer) {
            ImTextureData* td = cmd.TexRef._TexData;
            if (td == nullptr || td == liveAtlas) {
                continue; // no per-texture id to resolve, or already the live atlas the pass validates
            }
            if (!smatchet::ui::SmatchetDrawCmdTextureNeedsRebind(
                    static_cast<int>(td->Status), td->Pixels != nullptr,
                    td->GetTexID() == ImTextureID_Invalid)) {
                continue;
            }
            cmd.TexRef = liveAtlas->GetTexRef(); // GetTexRef() (not ImTextureRef(td)) wires _TexData
            ++repointed;
        }
    }
    if (repointed > 0) {
        static bool warnedOnce = false;
        if (!warnedOnce) {
            warnedOnce = true;
            SLOG("[P0 #1122] repointed %d orphaned draw command(s) to the live font atlas after a "
                 "mid-frame atlas teardown (further occurrences silenced)", repointed);
        }
    }
}

void RenderOneFrame(android_app* app, AndroidHostState& s) {
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

    // The device safe area (status bar / nav bar / display cutout) is applied as the main-viewport
    // work-area inset by PollWorkAreaInsets (registered in InitImGuiFirstTime), which ImGui folds in
    // during the NewFrame above. So DockSpaceOverViewport and the main menu bar both lay out clear of
    // the system chrome with no manual WorkPos fix-up here.
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
    s.mainWindow->Draw(*s.app);
    s.pluginHost->OnDraw(*s.app);

    // item 8 (#1069): degraded-mode cue when the IME bridge failed to init — a small, non-interactive
    // overlay so the user knows the soft keyboard is unavailable rather than silently dead-typing.
    if (!s.imeReady) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8.0f, vp->WorkPos.y + 8.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        const ImGuiWindowFlags cueFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##ime_degraded", nullptr, cueFlags)) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Soft keyboard unavailable - hardware keys only");
        }
        ImGui::End();
    }
    ImGui::Render();

    glViewport(0, 0, s.egl.Width(), s.egl.Height());
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    glClearColor(bg.x * bg.w, bg.y * bg.w, bg.z * bg.w, bg.w);
    glClear(GL_COLOR_BUFFER_BIT);

    // [P0 #1122] Make every committed draw command's texture safe before the GL3 backend reads its
    // id. Covers both the EGL/activity-recreate re-arm and the mid-frame atlas-teardown orphan that
    // crash the rotate->dock-tab->tap-Tickets repro; see GuardImGuiDynamicTextures above.
    ImDrawData* drawData = ImGui::GetDrawData();
    GuardImGuiDynamicTextures(drawData);
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
    // item 12: a present failure caused by EGL_CONTEXT_LOST needs a full GL-layer rebuild; a benign
    // surface loss is recovered by the ordinary TERM_WINDOW / INIT_WINDOW cycle and is ignored here.
    if (!s.egl.SwapBuffers() && s.egl.LastSwapLostContext()) {
        RecreateAfterContextLoss(app, s);
    }
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
    // Free the boot-panel GL objects only while the context is still current (a surface is bound);
    // otherwise egl.Destroy() below tears down the whole context and frees them anyway.
    if (s.egl.HasSurface()) {
        s.bootPanel.Destroy();
    }
    s.egl.Destroy();
    s.imguiReady = false;
    s.imeReady = false;
    s.coreBooted = false;
    s.hasSurface = false;
    s.bootPanelDrawn = false;
    SLOG("Teardown complete");
}

// item 14: APP_CMD_SAVE_STATE — the system may kill a backgrounded process at any time. Layout
// (imgui.ini) + config already write through to the private dir, but ImGui only flushes the ini on
// its ~5 s timer, so force a flush now or recent docking/layout edits are lost on a background-kill.
// The native_app_glue savedState blob then carries a versioned sentinel + resume counter (round-trip
// proof, visible in logcat); we malloc it and glue frees it (free_saved_state), matching the
// native-activity sample contract — do NOT free a previous blob here or it double-frees.
void PersistSavedState(android_app* app, AndroidHostState& s) {
    if (s.imguiReady) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.IniFilename != nullptr) {
            ImGui::SaveIniSettingsToDisk(io.IniFilename);
        }
    }
    SmatchetSavedState* blob = static_cast<SmatchetSavedState*>(std::malloc(sizeof(SmatchetSavedState)));
    if (blob == nullptr) {
        SLOGE("SAVE_STATE: malloc failed; process-death restore will start fresh");
        return;
    }
    blob->magic = kSavedStateMagic;
    blob->version = kSavedStateVersion;
    blob->resumeCount = s.resumeCount;
    app->savedState = blob;
    app->savedStateSize = sizeof(SmatchetSavedState);
    SLOG("SAVE_STATE persisted (resumeCount=%u, ini flushed)", s.resumeCount);
}

// item 14: cold-start restore. native_app_glue hands back the bytes PersistSavedState wrote in
// app->savedState (null on a clean launch). Layout + config restore from disk automatically; this
// only validates the sentinel and bumps the resume counter so the round-trip is observable in logcat.
void RestoreSavedState(android_app* app, AndroidHostState& s) {
    if (app->savedState == nullptr || app->savedStateSize < sizeof(SmatchetSavedState)) {
        return;
    }
    const SmatchetSavedState* blob = static_cast<const SmatchetSavedState*>(app->savedState);
    if (blob->magic != kSavedStateMagic || blob->version != kSavedStateVersion) {
        SLOGE("savedState magic/version mismatch (0x%x/%u) — starting fresh", blob->magic, blob->version);
        return;
    }
    s.resumeCount = blob->resumeCount + 1u;
    SLOG("Restored from process-death save-state (resumeCount now %u)", s.resumeCount);
}

// item 15: APP_CMD_CONFIG_CHANGED — display density can move at runtime (fold/unfold, external
// display, accessibility font scale). Re-resolve it; if it changed, rebuild the font atlas at the new
// pixel size and re-assert the host density scale WITHOUT compounding (ReassertHostDensityScale, not
// a second ApplyUiDensityScale which would stack scales). The atlas rebuild must stay under the
// Pillar-2 100 ms UI-block budget — timed and logged so a regression is visible.
void OnConfigChanged(android_app* app, AndroidHostState& s) {
    if (!s.imguiReady) {
        return;
    }
    const float newScale = smatchet::mobile::ResolveDensityScale(app->config);
    if (!(newScale > 0.0f) || newScale == s.densityScale) {
        return;
    }
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ImGuiIO& io = ImGui::GetIO();
    const float fontPx = kBaseFontPx * newScale;
    if (!s.fontBlob.empty()) {
        SmatchetSetInjectedFontBytes(s.fontBlob.data(), static_cast<int>(s.fontBlob.size()), fontPx);
    }
    SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(io); // rebuild the CPU atlas at the new size
    ImGui_ImplOpenGL3_DestroyDeviceObjects();            // drop the stale font texture + shaders
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {      // re-upload at the new atlas size
        // GL re-upload failed: keep s.densityScale and the theme scale on the OLD value so a later
        // CONFIG_CHANGED (or the backend's lazy per-frame device-object creation, which retries when
        // FontTexture==0) can recover, rather than asserting a scale whose font texture never landed.
        SLOGE("CONFIG_CHANGED CreateDeviceObjects failed (density %.2f) — keeping old scale %.2f for retry",
              newScale, s.densityScale);
        return;
    }
    SmatchetTheme::ReassertHostDensityScale(newScale);   // relative re-scale — no compounding
    s.densityScale = newScale;

    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    SLOG("CONFIG_CHANGED density %.2f font=%.1fpx atlas-rebuild %.1fms", newScale, fontPx, ms);
    if (ms > 100.0) {
        SLOGE("CONFIG_CHANGED atlas rebuild %.1fms exceeded the 100ms UI-block budget (Pillar 2)", ms);
    }
}

void OnAppCmd(android_app* app, int32_t cmd) {
    AndroidHostState& s = *static_cast<AndroidHostState*>(app->userData);
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window == nullptr) {
            break;
        }
        // item 7 (#1053): bring up the EGL context + surface even when Core failed to boot, so the
        // raw-GL boot/error panel can render. ImGui is initialised only once Core is up; on a
        // boot failure hasSurface still flips true and the main loop paints the panel instead.
        if (!s.egl.HasContext()) {
            s.egl.CreateContext();
        }
        if (s.egl.HasContext() && s.egl.CreateSurface(app->window)) {
            s.hasSurface = true;
            s.bootPanelDrawn = false; // repaint the panel once on a freshly (re)created surface
            if (s.coreBooted) {
                if (!s.imguiReady) {
                    InitImGuiFirstTime(app, s);
                } else {
                    // Context (+ GL objects + ImGui ctx) survived TERM_WINDOW; only re-point the
                    // android backend at the fresh ANativeWindow so NewFrame reads the right size.
                    ImGui_ImplAndroid_Init(app->window);
                }
            }
        }
        break;
    case APP_CMD_TERM_WINDOW:
        s.hasSurface = false;
        s.egl.DestroySurface(); // keep context + GL objects + ImGui ctx alive
        break;
    case APP_CMD_GAINED_FOCUS:
        s.hasFocus = true; // item 13: resume the render/swap loop
        break;
    case APP_CMD_LOST_FOCUS:
        s.hasFocus = false; // item 13: pause render — don't draw into a non-focused surface
        break;
    case APP_CMD_RESUME:
        s.paused = false; // item 13: foregrounded again
        break;
    case APP_CMD_PAUSE:
        s.paused = true; // item 13: backgrounded — stop the render/swap loop (Pillar 1: no idle GPU)
        break;
    case APP_CMD_SAVE_STATE:
        PersistSavedState(app, s); // item 14: a process-death may follow — flush ini + persist sentinel
        break;
    case APP_CMD_CONFIG_CHANGED:
        OnConfigChanged(app, s); // item 15: runtime density change — rebuild atlas + re-assert scale
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

// item 7: paint the raw-GL boot/error panel once and present it. Called from the main loop only
// when the surface is live but Core failed to boot (so ImGui is never available).
void DrawBootErrorPanel(AndroidHostState& s) {
    s.bootPanel.Draw(s.egl.Width(), s.egl.Height(), "STARTUP FAILED", "CHECK LOGCAT");
    s.egl.SwapBuffers();
}

// Whether the next ALooper_pollOnce should be non-blocking (timeout 0). Re-evaluated on EVERY poll
// (an INIT_WINDOW processed mid-drain flips hasSurface, and the next poll must switch to 0 so the
// loop exits and renders). A booted ImGui UI animates continuously → always spin; a boot-failed app
// needs exactly one panel draw, then blocks on -1 so it consumes no CPU.
bool WantImmediatePoll(const AndroidHostState& s) {
    // item 13: a paused / unfocused app must block on pollOnce(-1) so a backgrounded process consumes
    // no CPU (Pillar 1) — never spin (timeout 0) while we're not rendering.
    if (!s.hasSurface || s.paused || !s.hasFocus) {
        return false;
    }
    if (s.coreBooted && s.imguiReady) {
        return true;
    }
    return !s.bootPanelDrawn;
}

// item 13: a frame may render only when the surface is live, Core + ImGui are up, and the app is
// foregrounded + focused. Gating render on focus/foreground keeps a backgrounded app off the CPU/GPU
// (Pillar 1) and avoids drawing into a surface the system is tearing down.
bool IsRenderable(const AndroidHostState& s) {
    return s.hasSurface && s.coreBooted && s.imguiReady && s.hasFocus && !s.paused;
}

} // namespace

void android_main(android_app* app) {
    AndroidHostState state;
    app->userData = &state;
    app->onAppCmd = OnAppCmd;
    app->onInputEvent = OnInputEvent;

    BootCoreOnce(app, state);
    RestoreSavedState(app, state); // item 14: cold-start process-death restore (no-op on clean launch)

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;
        // Block when there's no per-frame work pending, spin (timeout 0) when there is. The predicate
        // is re-evaluated on every poll (it's in the loop condition): an INIT_WINDOW processed inside
        // this drain flips hasSurface true, and the next poll must switch to 0 so the loop exits and
        // renders. A boot-failed app draws its panel once then blocks — no CPU spin (Pillar 1).
        while (ALooper_pollOnce(WantImmediatePoll(state) ? 0 : -1, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) {
                source->process(app, source);
            }
            if (app->destroyRequested != 0) {
                TeardownAll(state);
                return;
            }
        }
        if (IsRenderable(state)) {
            RenderOneFrame(app, state);
        } else if (state.hasSurface && !state.coreBooted && !state.bootPanelDrawn &&
                   state.hasFocus && !state.paused) {
            // item 13: mirror IsRenderable's focus/pause gate so the boot panel never draws/swaps a
            // frame while backgrounded or unfocused (Pillar 1/2). On resume WantImmediatePoll returns
            // !bootPanelDrawn == true, spins the loop back here, and the panel paints then.
            DrawBootErrorPanel(state); // Core boot failed → raw-GL error panel (item 7)
            state.bootPanelDrawn = true;
        }
    }
}
