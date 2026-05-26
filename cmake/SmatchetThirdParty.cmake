function(smatchet_prepare_cpr)
    # Bundled libcurl CMake probes ioctlsocket(FIONBIO) with int*; GCC 14+
    # rejects int* vs u_long*. Winsock uses u_long, so force the successful
    # result for MinGW-family toolchains.
    if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        set(HAVE_IOCTLSOCKET_FIONBIO 1 CACHE INTERNAL "curl: ioctlsocket FIONBIO (MinGW GCC14+ probe fix)" FORCE)
    endif()
    # Smatchet uses libcurl for HTTPS transport but does not need zlib-backed
    # transfer decoding in the standalone runtime.
    set(CURL_ZLIB OFF CACHE BOOL "Disable optional curl zlib support" FORCE)
endfunction()

function(smatchet_patch_cpr_after_fetch cpr_source_dir)
    # MSVC C4244: Body(File) used int for tellg() (streamoff). Patch vendored
    # header after fetch rather than carrying a fork.
    set(_cpr_body_h "${cpr_source_dir}/include/cpr/body.h")
    if(EXISTS "${_cpr_body_h}")
        file(READ "${_cpr_body_h}" _cpr_body_h_contents)
        set(_cpr_body_h_patched "${_cpr_body_h_contents}")
        string(REPLACE "int length = is.tellg();"
                       "const std::streamoff length = is.tellg();"
                       _cpr_body_h_patched
                       "${_cpr_body_h_patched}")
        string(REPLACE "buffer.resize(length);"
                       "buffer.resize(static_cast<size_t>(length));"
                       _cpr_body_h_patched
                       "${_cpr_body_h_patched}")
        string(REPLACE "is.read(&buffer[0], length);"
                       "is.read(buffer.data(), static_cast<std::streamsize>(length));"
                       _cpr_body_h_patched
                       "${_cpr_body_h_patched}")
        if(NOT _cpr_body_h_patched STREQUAL _cpr_body_h_contents)
            file(WRITE "${_cpr_body_h}" "${_cpr_body_h_patched}")
            message(STATUS "Patched cpr include/cpr/body.h (streamoff / resize / read).")
        endif()
    endif()
endfunction()

function(smatchet_prepare_sqlitecpp)
    set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "Disable linting" FORCE)
    set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "Disable cppcheck" FORCE)
endfunction()

function(smatchet_patch_sqlitecpp_after_populate sqlitecpp_source_dir)
    # Avoid warnings about pre-3.10 compatibility in the vendored project.
    set(_sqlitecpp_cmakelists "${sqlitecpp_source_dir}/CMakeLists.txt")
    if(EXISTS "${_sqlitecpp_cmakelists}")
        file(READ "${_sqlitecpp_cmakelists}" _sqlitecpp_cmakelists_contents)
        string(REPLACE "cmake_minimum_required(VERSION 3.5)"
                       "cmake_minimum_required(VERSION 3.10)"
                       _sqlitecpp_patched_contents
                       "${_sqlitecpp_cmakelists_contents}")
        if(NOT _sqlitecpp_patched_contents STREQUAL _sqlitecpp_cmakelists_contents)
            file(WRITE "${_sqlitecpp_cmakelists}" "${_sqlitecpp_patched_contents}")
        endif()
    endif()

    # MinGW/GCC: fixed-width integer typedefs no longer leak from libstdc++
    # transitive includes; SQLiteCpp 3.3.1 uses int32_t/int64_t/... without
    # <cstdint> in Statement.h.
    set(_sqlitecpp_statement_h "${sqlitecpp_source_dir}/include/SQLiteCpp/Statement.h")
    if(EXISTS "${_sqlitecpp_statement_h}")
        file(READ "${_sqlitecpp_statement_h}" _sqlitecpp_statement_h_contents)
        if(NOT _sqlitecpp_statement_h_contents MATCHES "#include <cstdint>")
            string(REPLACE "#include <memory>"
                           "#include <memory>\n#include <cstdint>"
                           _sqlitecpp_statement_h_patched
                           "${_sqlitecpp_statement_h_contents}")
            if(NOT _sqlitecpp_statement_h_patched STREQUAL _sqlitecpp_statement_h_contents)
                file(WRITE "${_sqlitecpp_statement_h}" "${_sqlitecpp_statement_h_patched}")
                message(STATUS "Patched SQLiteCpp include/SQLiteCpp/Statement.h (<cstdint> for MinGW/GCC).")
            endif()
        endif()
    endif()
endfunction()

function(smatchet_prepare_httplib)
    # We only use the plain HTTP server surface today. Letting cpp-httplib
    # auto-detect compression/TLS support pulls unnecessary runtime
    # dependencies into the standalone publish artifact.
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "Disable optional cpp-httplib OpenSSL auto-linking" FORCE)
    set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "Disable optional cpp-httplib zlib auto-linking" FORCE)
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "Disable optional cpp-httplib brotli auto-linking" FORCE)
endfunction()
