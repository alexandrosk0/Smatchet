# Plugins/Whisper — push-to-talk dictation plugin. See
# docs/design/whisper-dictation.md.
#
# Entire subtree is CMake-conditional (gating layer 6 in the plan): the root
# CMakeLists.txt only calls add_subdirectory(Plugins/Whisper) when
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

# See AI-shim note on SmatchetPlugin_LuaConsole in the root CMakeLists.txt — UiDrawSession
# layout depends on SMATCHET_WITH_AI. Plugins including Source_Core headers must match the
# Source_Core define state to avoid silent member-offset drift.
if(SMATCHET_WITH_AI)
    target_link_libraries(SmatchetPlugin_Whisper PUBLIC SmatchetCoreAiShim)
endif()

# Link ghc::filesystem for the model-presence probe in WhisperPlugin.cpp.
# Standalone targets already link it transitively, but PRIVATE here keeps the
# dependency explicit and survives a future plugin-host refactor.
target_link_libraries(SmatchetPlugin_Whisper PRIVATE
    ghc_filesystem
)

# Win32 WASAPI symbols (CoInitializeEx, MMDeviceEnumerator, IAudioClient, ...).
# Phase B introduces WindowsAudioCapture.cpp; the libs below resolve the COM
# imports it uses. Plugin subtree is only entered on Windows builds with
# SMATCHET_WITH_WHISPER=ON, so the link list is unconditional.
#
# Phase C adds bcrypt for ModelDownloader's streaming SHA-256 (BCryptHash with
# BCRYPT_SHA256_ALGORITHM) — no extra third-party hash dep needed since the
# plugin is already Windows-gated.
if(WIN32)
    target_link_libraries(SmatchetPlugin_Whisper PRIVATE
        ole32
        oleaut32
        uuid
        avrt
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
