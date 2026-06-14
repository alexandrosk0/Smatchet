# Source/Plugins/Whisper — push-to-talk dictation plugin. See
# docs/design/whisper-dictation.md.
#
# Entire subtree is CMake-conditional (gating layer 6 in the plan): the root
# CMakeLists.txt only calls add_subdirectory(Source/Plugins/Whisper) when
# SMATCHET_WITH_WHISPER=ON, so no source-level ifdefs are needed inside this
# directory. The DX12 / Unreal build keeps SMATCHET_WITH_WHISPER=OFF and
# therefore never visits this CMakeLists.

add_library(SmatchetPlugin_Whisper STATIC
    WhisperPlugin.cpp
    WhisperApiClient.cpp
    WhisperApiKeyResolve.cpp
    WavWriter.cpp
    WindowsAudioCapture.cpp
    # Phase C — local backend wrapper, model catalog, downloader, consent gate.
    WhisperLocal.cpp
    ModelCatalog.cpp
    ModelDownloader.cpp
    ModelDownloadPolicy.cpp
    WhisperConsentGate.cpp
    # Phase E — push-to-talk hotkey parsing + Win32 global hotkey hook.
    HotkeyParse.cpp
    GlobalHotkey_Win32.cpp
    # Phase F — pure-helper silence trim + clip-length cap (worker-thread).
    SilenceTrim.cpp
)

target_include_directories(SmatchetPlugin_Whisper PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(SmatchetPlugin_Whisper PUBLIC
    SmatchetCoreInterface
)

# See docs/adr/0002-plugin-shim-link-discipline.md — every gated `#if SMATCHET_WITH_*` block
# inside `SmatchetUiSession.h` (and any other Source/Core header that grows them) shifts
# member offsets. Plugins must match the Source/Core define state to avoid silent ABI drift.
# Configure-time guard `smatchet_assert_plugin_shim_links` fires below if either shim is missing.
if(SMATCHET_WITH_AI)
    target_link_libraries(SmatchetPlugin_Whisper PUBLIC SmatchetCoreAiShim)
endif()
if(SMATCHET_WITH_MCP)
    target_link_libraries(SmatchetPlugin_Whisper PUBLIC SmatchetCoreMcpShim)
endif()

# Link ghc::filesystem for the model-presence probe in WhisperPlugin.cpp.
# Standalone targets already link it transitively, but PRIVATE here keeps the
# dependency explicit and survives a future plugin-host refactor.
target_link_libraries(SmatchetPlugin_Whisper PRIVATE
    ghc_filesystem
)

# ImGui include path for WhisperPlugin.cpp — the post-transcription splice
# now snapshots `ImGui::GetActiveID()` so the router can pick the focused
# entry instead of the front of the list. Header-only consume; ImGuiLib's
# PUBLIC includes propagate via this link.
target_link_libraries(SmatchetPlugin_Whisper PRIVATE
    ImGuiLib
)

# Win32 WASAPI symbols (CoInitializeEx, MMDeviceEnumerator, IAudioClient, ...).
# Phase B introduces WindowsAudioCapture.cpp, which compiles cross-platform
# (non-Windows builds get a stub) but requires the COM / WASAPI libraries on
# Windows. The plugin subtree itself is entered whenever SMATCHET_WITH_WHISPER
# is ON regardless of platform; the if(WIN32) guard scopes ONLY the Windows-only
# link list.
#
# Phase C adds bcrypt for ModelDownloader's streaming SHA-256
# (BCryptCreateHash + BCryptHashData + BCryptFinishHash chain with
# BCRYPT_SHA256_ALGORITHM) — no extra third-party hash dep needed since the
# Windows link path covers it.
#
# avrt (MMCSS) intentionally omitted — WindowsAudioCapture.cpp does not call
# AvSetMmThreadCharacteristics / AvRevertMmThreadCharacteristics and does not
# include <avrt.h>. Re-add only when MMCSS thread-priority promotion is wired in.
if(WIN32)
    # avrt (MMCSS) intentionally omitted — WindowsAudioCapture.cpp does not call
    # AvSetMmThreadCharacteristics / AvRevertMmThreadCharacteristics and does not
    # include <avrt.h>. Re-add only when MMCSS thread-priority promotion is wired in.
    target_link_libraries(SmatchetPlugin_Whisper PRIVATE
        ole32
        oleaut32
        uuid
        bcrypt
    )
endif()

# Optional whisper.cpp local-backend link. Default OFF — Phase C ships the
# WhisperLocal API surface unconditionally, but the actual whisper.cpp link
# costs ~50 MB of static lib and carries MinGW UCRT build risk. Flip the
# sub-option ON to fetch whisper.cpp v1.7.4 and link it into the plugin; the
# `SMATCHET_WHISPER_LOCAL_BACKEND=1` compile define then activates the live
# inference branch in WhisperLocal.cpp. See docs/design/whisper-dictation.md
# § Deviations for the rationale.
if(SMATCHET_WHISPER_LOCAL_BACKEND)
    target_compile_definitions(SmatchetPlugin_Whisper PUBLIC SMATCHET_WHISPER_LOCAL_BACKEND=1)
    target_link_libraries(SmatchetPlugin_Whisper PRIVATE whisper)
endif()

smatchet_apply_strict_warnings(SmatchetPlugin_Whisper)
smatchet_assert_plugin_shim_links(SmatchetPlugin_Whisper)
