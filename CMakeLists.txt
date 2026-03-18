cmake_minimum_required(VERSION 3.24)

# Tell modern CMake to tolerate older third-party library policies
set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "Minimum CMake policy version" FORCE)

# FIX: Silence the FetchContent_Populate deprecation warning for header-only libs
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# Use Ninja from PATH when using Ninja generator (avoids stale WinGet path from another user)
find_program(_NINJA_FROM_PATH ninja PATHS ENV PATH)
if(_NINJA_FROM_PATH)
    set(CMAKE_MAKE_PROGRAM "${_NINJA_FROM_PATH}" CACHE FILEPATH "Ninja executable" FORCE)
endif()
unset(_NINJA_FROM_PATH CACHE)

project(SmatchetApp VERSION 1.0 LANGUAGES C CXX)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static libraries" FORCE)
set(CPR_FORCE_USE_SYSTEM_CURL OFF CACHE BOOL "Use internal cURL" FORCE)

# SET TO C++14 FOR MAXIMUM ENGINE COMPATIBILITY
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==============================================================================
# 1. FETCH ALL DEPENDENCIES (Zero manual downloads required)
# ==============================================================================
include(FetchContent)

message(STATUS "Fetching nlohmann/json...")
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

message(STATUS "Fetching cpr (C++ Requests)...")
FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git GIT_TAG 1.9.2)
FetchContent_MakeAvailable(cpr)

message(STATUS "Fetching SQLiteCpp...")
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "Disable linting" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "Disable cppcheck" FORCE)
FetchContent_Declare(sqlitecpp GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git GIT_TAG 3.3.1)
FetchContent_MakeAvailable(sqlitecpp)

message(STATUS "Fetching cpp-httplib...")
FetchContent_Declare(httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git GIT_TAG v0.14.1)
FetchContent_MakeAvailable(httplib)


message(STATUS "Fetching ghc::filesystem (C++14 Polyfill)...")
FetchContent_Declare(ghc_filesystem GIT_REPOSITORY https://github.com/gulrak/filesystem.git GIT_TAG v1.5.14)
FetchContent_MakeAvailable(ghc_filesystem)

message(STATUS "Fetching whisper.cpp...")
FetchContent_Declare(whisper GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git GIT_TAG v1.5.4)
FetchContent_MakeAvailable(whisper)

message(STATUS "Fetching stb_image...")
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG master)
FetchContent_Populate(stb)

message(STATUS "Fetching miniaudio...")
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG master)
FetchContent_Populate(miniaudio)

message(STATUS "Fetching imgui_markdown...")
FetchContent_Declare(imgui_markdown GIT_REPOSITORY https://github.com/juliettef/imgui_markdown.git GIT_TAG main)
FetchContent_Populate(imgui_markdown)

message(STATUS "Fetching GLFW...")
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.3.8)
FetchContent_MakeAvailable(glfw)

# ==============================================================================
# LUA & SOL2 (C++14 COMPATIBLE)
# ==============================================================================
set(LUA_SRC_DIR "${CMAKE_BINARY_DIR}/_deps/lua-src")
if(NOT EXISTS "${LUA_SRC_DIR}")
    message(STATUS "Downloading Lua 5.3.6 source...")
    file(DOWNLOAD https://www.lua.org/ftp/lua-5.3.6.tar.gz ${CMAKE_BINARY_DIR}/lua-5.3.6.tar.gz)
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf ${CMAKE_BINARY_DIR}/lua-5.3.6.tar.gz WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/_deps)
    file(RENAME ${CMAKE_BINARY_DIR}/_deps/lua-5.3.6 ${LUA_SRC_DIR})
endif()

# 1. Build Lua under a UNIQUE name to avoid clashing with sol2's internal 'LuaLib'
file(GLOB LUA_SOURCES "${LUA_SRC_DIR}/src/*.c")
list(REMOVE_ITEM LUA_SOURCES "${LUA_SRC_DIR}/src/lua.c" "${LUA_SRC_DIR}/src/luac.c")

add_library(Smatchet_Lua_Internal STATIC ${LUA_SOURCES})
target_include_directories(Smatchet_Lua_Internal PUBLIC "$<BUILD_INTERFACE:${LUA_SRC_DIR}/src>")

# 2. Tell sol2 where the headers are and that Lua is ALREADY FOUND
set(LUA_INCLUDE_DIR "${LUA_SRC_DIR}/src" CACHE PATH "" FORCE)
set(LUA_LIBRARIES Smatchet_Lua_Internal CACHE STRING "" FORCE)
set(SOL2_BUILD_LUA OFF CACHE BOOL "" FORCE) # Tell sol2 NOT to try building its own Lua

FetchContent_Declare(sol2 GIT_REPOSITORY https://github.com/ThePhD/sol2.git GIT_TAG v2.20.6)
FetchContent_MakeAvailable(sol2)



# Compile Dear ImGui (with GLFW and OpenGL3 backends)
message(STATUS "Fetching Dear ImGui...")
FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG docking
)

FetchContent_Populate(imgui)
file(GLOB IMGUI_SOURCES "${imgui_SOURCE_DIR}/*.cpp")
list(APPEND IMGUI_SOURCES "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
add_library(ImGuiLib STATIC ${IMGUI_SOURCES})
target_include_directories(ImGuiLib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${glfw_SOURCE_DIR}/include
)

# ==============================================================================
# 3. SMATCHET CORE LIBRARY (Engine-Agnostic)
# ==============================================================================
file(GLOB_RECURSE CORE_SOURCES "Source_Core/src/*.cpp")
add_library(SmatchetCore STATIC ${CORE_SOURCES})

# FIX: Explicitly define the sol2 and lua paths because FetchContent variables
# can sometimes be finicky with older sol2 tags.
set(SOL2_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/sol2-src")
set(LUA_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/lua-src/src")

target_include_directories(SmatchetCore PUBLIC
        Source_Core/include
        ${sqlitecpp_SOURCE_DIR}/include
        ${LUA_SRC_DIR}/src
        ${sol2_SOURCE_DIR}/single
)
target_include_directories(SmatchetCore PRIVATE
        Source_Core/src
        ${stb_SOURCE_DIR}
        ${imgui_markdown_SOURCE_DIR}
        ${sol2_SOURCE_DIR}/single
        ${LUA_SRC_DIR}/src
)

target_link_libraries(SmatchetCore PUBLIC
        nlohmann_json::nlohmann_json
        SQLiteCpp
        cpr::cpr
        httplib::httplib
)

target_link_libraries(SmatchetCore PRIVATE
        Smatchet_Lua_Internal  # This provides the actual Lua symbols
        ghc_filesystem
        whisper
        ImGuiLib
        glfw
)
# ==============================================================================
# 4. SMATCHET STANDALONE EXECUTABLE
# ==============================================================================
file(GLOB STANDALONE_SOURCES "Target_Standalone/*.cpp")
add_executable(SmatchetStandalone ${STANDALONE_SOURCES})

target_include_directories(SmatchetStandalone PRIVATE ${miniaudio_SOURCE_DIR})

# Fix MinGW/GCC "relocation truncated to fit" when linking sol2/CachedTicket in Debug
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(SmatchetStandalone PRIVATE -mcmodel=large)
    target_link_options(SmatchetStandalone PRIVATE -mcmodel=large)
    # MinGW ld can overflow COFF debug relocations in this translation unit.
    # Force minimal debug emission for standalone target to avoid linker overflow.
    target_compile_options(SmatchetStandalone PRIVATE -g0)
endif()

target_link_libraries(SmatchetStandalone PRIVATE
        SmatchetCore
        ImGuiLib
        glfw
)

if(WIN32)
    target_link_libraries(SmatchetStandalone PRIVATE opengl32 ws2_32 crypt32)
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(OPENGL_LIBRARY OpenGL)
    target_link_libraries(SmatchetStandalone PRIVATE ${COCOA_LIBRARY} ${OPENGL_LIBRARY})
endif()