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
)

target_include_directories(SmatchetPlugin_Whisper PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(SmatchetPlugin_Whisper PUBLIC
    SmatchetCoreInterface
)

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
if(WIN32)
    target_link_libraries(SmatchetPlugin_Whisper PRIVATE
        ole32
        oleaut32
        uuid
        avrt
    )
endif()

smatchet_apply_strict_warnings(SmatchetPlugin_Whisper)
