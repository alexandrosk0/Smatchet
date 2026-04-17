#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the Smatchet ImGui host (created/destroyed by the plugin native lib).
typedef void* SmatchetImGuiHostHandle;

typedef enum SmatchetRendererBackendC {
    SMATCHET_RENDERER_BACKEND_UNKNOWN = 0,
    SMATCHET_RENDERER_BACKEND_DX12 = 1,
    SMATCHET_RENDERER_BACKEND_PS5 = 2,
    SMATCHET_RENDERER_BACKEND_XBOX = 3
} SmatchetRendererBackendC;

// Callbacks provided by the Unreal host.
typedef void (*SmatchetOpenUrlFn)(const char* urlUtf8, void* userData);
typedef void (*SmatchetAttachmentViewerFn)(const char* localPathUtf8,
                                             const char* mimeTypeUtf8,
                                             const char* filenameUtf8,
                                             void* userData);

// Lifecycle
SmatchetImGuiHostHandle SmatchetHost_Create();
void SmatchetHost_Destroy(SmatchetImGuiHostHandle host);

// Cache init options (no network/load starts here; the real initialize is triggered on first show).
void SmatchetHost_SetInitOptions(
    SmatchetImGuiHostHandle host,
    const char* dbPathUtf8,
    const char* backendTypeUtf8,
    int mcpPort,
    int rendererBackend,
    int numFramesInFlight,
    int colorFormat,
    void* nativeDevice,
    void* rendererResource0,
    void* rendererResource1,
    void* rendererResource2,
    void* nativeCommandQueue,
    SmatchetOpenUrlFn openUrlFn,
    void* openUrlUserData,
    SmatchetAttachmentViewerFn attachmentViewerFn,
    void* attachmentViewerUserData);
bool SmatchetHost_UpdateRendererColorFormat(SmatchetImGuiHostHandle host, int colorFormat);
/** D3D12: total SRV descriptors in RendererResource0 heap. Slot 0 is reserved for the font atlas,
 *  slots 1..N-1 are used by the dynamic-texture SRV allocator (attachment thumbnails, etc.).
 *  Call after SmatchetHost_SetInitOptions and before the first BeginFrame. Default is 1. */
void SmatchetHost_SetNumSrvDescriptors(SmatchetImGuiHostHandle host, int numSrvDescriptors);

/** Static string: ImGui version + init path; use to verify packaged SmatchetImGuiHost.lib matches plugin. */
const char* SmatchetHost_GetBuildTag(void);
/** Empty string if no failure yet; UTF-8 message from last Initialize() failure. */
const char* SmatchetHost_GetLastInitError(SmatchetImGuiHostHandle host);
/** Writes ASCII summary of cached SetInitOptions renderer fields into buf (NUL-terminated, truncated). */
void SmatchetHost_FormatCachedRendererSummary(SmatchetImGuiHostHandle host, char* buf, int bufSize);

// UI visibility
void SmatchetHost_SetUiVisible(SmatchetImGuiHostHandle host, bool visible);
void SmatchetHost_ToggleUiVisible(SmatchetImGuiHostHandle host);
void SmatchetHost_SetSuppressSoftwareCursor(SmatchetImGuiHostHandle host, bool suppress);
bool SmatchetHost_GetSuppressSoftwareCursor(SmatchetImGuiHostHandle host);
bool SmatchetHost_IsUiVisible(SmatchetImGuiHostHandle host);
bool SmatchetHost_IsInitialized(SmatchetImGuiHostHandle host);
bool SmatchetHost_IsFrameActive(SmatchetImGuiHostHandle host);

// Frame
void SmatchetHost_BeginFrame(SmatchetImGuiHostHandle host,
                             float deltaTimeSeconds,
                             float viewportWidth,
                             float viewportHeight);
void SmatchetHost_DrawUI(SmatchetImGuiHostHandle host);
void SmatchetHost_RenderDrawData(SmatchetImGuiHostHandle host, int rendererBackend, void* nativeCommandList);

// Input forwarding
void SmatchetHost_SetMousePosition(SmatchetImGuiHostHandle host, float x, float y);
void SmatchetHost_SetMouseButton(SmatchetImGuiHostHandle host, int button, bool isDown);
void SmatchetHost_AddMouseWheel(SmatchetImGuiHostHandle host, float wheelX, float wheelY);
void SmatchetHost_SetKeyDown(SmatchetImGuiHostHandle host, int imguiKey, bool isDown);
void SmatchetHost_SetKeyModifiers(SmatchetImGuiHostHandle host,
                                   bool ctrl,
                                   bool shift,
                                   bool alt,
                                   bool superKey);
void SmatchetHost_AddInputCharacter(SmatchetImGuiHostHandle host, unsigned int character);

#ifdef __cplusplus
} // extern "C"
#endif

