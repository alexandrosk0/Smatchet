# Source/Mobile — Android SHARED lib (SmatchetMobile) over CORE_SOURCES.
#
# INCLUDED from the root CMakeLists.txt via include() — deliberately NOT
# add_subdirectory(). All targets, variables, and source-file properties defined
# here live in the ROOT directory scope, exactly as they did when this content
# was part of the monolithic root CMakeLists.txt (pure mechanical split — zero
# behavior change: artifact/object paths, target names, options, and flags are
# identical). Consequences to keep in mind when editing:
#   * relative paths resolve against the repo root (CMAKE_CURRENT_SOURCE_DIR ==
#     the repo root here), NOT against this file's directory;
#   * variables set here are plain root-scope variables — visible to every
#     component file included after this one (no PARENT_SCOPE needed);
#   * do NOT add cmake_minimum_required() / project() here.
#
# Include order (root CMakeLists.txt § component includes):
#   ThirdParty/App deps -> Source/Core -> Source/Mobile -> Source/Plugins/Mcp
#   -> Source/Plugins/LuaConsole -> Source/Plugins/Whisper (add_subdirectory)
#   -> Source/UnrealPlugins -> Source/Standalone
#
# Defines (ANDROID only): SmatchetMobile SHARED lib + the NDK native_app_glue
# STATIC lib. No-op on every desktop configure.

# ==============================================================================
# 3a. SMATCHET MOBILE SHARED LIBRARY (Android, Phase 0)
# ==============================================================================
# Third sibling of SmatchetStandalone (GLFW/GL exe) and SmatchetCore_DX12 (Unreal
# STATIC lib): a SHARED .so over the SAME engine-agnostic CORE_SOURCES, compiled by
# the NDK toolchain through the non-_WIN32 (#else) side of the platform #ifdefs.
#
# Slice 3b deliverable is the working host shell: Source/Mobile/Android/*.cpp (android_main,
# the EGL/GLES context, the JNI IME bridge, the APK asset/density helpers) compiled into the
# .so alongside CORE_SOURCES, plus the NDK android_native_app_glue static lib that provides
# the ANativeActivity_onCreate entry point and the android_app event pump. With the host in
# place every symbol CORE_SOURCES needs is now defined, so the Slice-2 -Wl,-z,undefs escape
# is dropped in favour of -u ANativeActivity_onCreate (force-keep the glue entry point so the
# linker does not garbage-collect it out of the static archive).
if(ANDROID)
    file(GLOB MOBILE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/Mobile/Android/*.cpp")
    add_library(SmatchetMobile SHARED ${CORE_SOURCES} ${MOBILE_SOURCES})
    smatchet_configure_mobile_core_impl_target(SmatchetMobile)
    smatchet_apply_strict_warnings(SmatchetMobile)

    # android_native_app_glue: NDK-sources static lib that turns the NativeActivity callbacks
    # into the android_app event loop android_main() consumes. Built from the active NDK's
    # source tree; its include dir is propagated PUBLIC so android_main.cpp resolves the header.
    if(DEFINED ANDROID_NDK)
        set(_smatchet_ndk_root "${ANDROID_NDK}")
    else()
        set(_smatchet_ndk_root "${CMAKE_ANDROID_NDK}")
    endif()
    set(_smatchet_glue_dir "${_smatchet_ndk_root}/sources/android/native_app_glue")
    add_library(native_app_glue STATIC "${_smatchet_glue_dir}/android_native_app_glue.c")
    target_include_directories(native_app_glue PUBLIC "${_smatchet_glue_dir}")

    # Android system libs for the GLES3 + EGL render path and platform logging, plus the glue.
    target_link_libraries(SmatchetMobile PRIVATE native_app_glue log android EGL GLESv3)
    # Force-keep the glue's ANativeActivity_onCreate (the .so entry point dlsym'd by the
    # framework) — without -u the linker prunes it as unreferenced from the static archive.
    target_link_options(SmatchetMobile PRIVATE -u ANativeActivity_onCreate)
    # 16 KB page-size compatibility (Android 15+: Pixel/devices boot with 16 KB pages and
    # reject .so files whose LOAD segments are 4 KB-aligned). NDK r27+ defaults to 16 KB;
    # NDK 26 still emits 0x1000 alignment, so force the ELF max-page-size to 16384. AGP
    # 8.5.1+ separately zip-aligns the stored uncompressed .so to a 16 KB boundary.
    target_link_options(SmatchetMobile PRIVATE
        "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
endif()
