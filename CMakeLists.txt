cmake_minimum_required(VERSION 3.20)
project(mtk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    # /W4 = "all reasonable warnings"; /permissive- enforces standard C++
    # (closer to GCC/Clang strictness). /Zc:__cplusplus makes __cplusplus
    # report the actual standard rather than 199711L.
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
    # NOMINMAX prevents <windows.h> from defining min/max macros that
    # collide with std::min/std::max; WIN32_LEAN_AND_MEAN trims the
    # windows.h surface.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
endif()

# ---------- version stamp (git SHA + build date → src/version.hpp) ----------
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} rev-parse --short=8 HEAD
        OUTPUT_VARIABLE MTK_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_rev_result
    )
    if(NOT _git_rev_result EQUAL 0)
        set(MTK_GIT_SHA "unknown")
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} status --porcelain
        OUTPUT_VARIABLE _git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_git_status)
        set(MTK_GIT_SHA "${MTK_GIT_SHA}+dirty")
    endif()
else()
    set(MTK_GIT_SHA "unknown")
endif()

string(TIMESTAMP MTK_BUILD_DATE "%Y-%m-%d" UTC)
set(MTK_VERSION_FULL "${PROJECT_VERSION} (${MTK_GIT_SHA}, built ${MTK_BUILD_DATE})")
message(STATUS "mtk version: ${MTK_VERSION_FULL}")

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/mtk/version.hpp
    @ONLY
)

# ---------- submodule sanity check + auto-init ----------
# Detect missing submodules; if we're in a git checkout, run
# `git submodule update --init --recursive --depth 1` automatically so
# `git clone` (without --recurse-submodules) + `cmake -B build` works
# first try. Falls back to a clear error otherwise (downloaded tarball,
# no git, offline, etc. — INSTALL.md covers the manual path).
set(_mtk_third_party_deps CLI11 tomlplusplus reproc fmt doctest json)
set(_mtk_missing_deps "")
foreach(dep ${_mtk_third_party_deps})
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/${dep}/CMakeLists.txt")
        list(APPEND _mtk_missing_deps ${dep})
    endif()
endforeach()

if(_mtk_missing_deps)
    if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        message(STATUS "Submodules missing (${_mtk_missing_deps}); running "
                       "'git submodule update --init --recursive --depth 1' ...")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive --depth 1
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE _mtk_submod_rc
        )
        if(NOT _mtk_submod_rc EQUAL 0)
            message(FATAL_ERROR
                "Automatic submodule init failed (likely offline / no GitHub "
                "access). See INSTALL.md for the manual tarball-drop fallback.")
        endif()
        foreach(dep ${_mtk_missing_deps})
            if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/${dep}/CMakeLists.txt")
                message(FATAL_ERROR
                    "third_party/${dep}/ still missing after auto-init. "
                    "See INSTALL.md for manual setup.")
            endif()
        endforeach()
    else()
        message(FATAL_ERROR
            "Submodules missing: ${_mtk_missing_deps}. "
            "Not in a git checkout (or git not found) — auto-init unavailable. "
            "Run 'git submodule update --init --recursive' from a clone, "
            "or see INSTALL.md for the manual fallback.")
    endif()
endif()

# ---------- third-party deps ----------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# All third_party deps use EXCLUDE_FROM_ALL so `cmake --install` only
# installs our own binary + filters + docs, not the static-linked-in deps.

# CLI11 (header-only target: CLI11::CLI11). Currently unused — main.cpp
# dropped CLI11 during Phase 1.5 (2/2). Kept loaded for the moment so the
# submodule init doesn't churn; can be removed in a cleanup pass.
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/CLI11 EXCLUDE_FROM_ALL)

# tomlplusplus (header-only target: tomlplusplus::tomlplusplus)
add_subdirectory(third_party/tomlplusplus EXCLUDE_FROM_ALL)

# fmt (target: fmt::fmt)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fmt EXCLUDE_FROM_ALL)

# reproc — C++ wrapper target: reproc++
set(REPROC++ ON CACHE BOOL "" FORCE)
set(REPROC_TEST OFF CACHE BOOL "" FORCE)
set(REPROC_EXAMPLES OFF CACHE BOOL "" FORCE)
set(REPROC_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/reproc EXCLUDE_FROM_ALL)

# doctest (header-only target: doctest::doctest)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/doctest EXCLUDE_FROM_ALL)

# nlohmann/json (header-only target: nlohmann_json::nlohmann_json).
# Used only by cmds/hook.cpp for Copilot integration; the heavy compile
# cost is contained to that one TU.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
set(JSON_MultipleHeaders OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/json EXCLUDE_FROM_ALL)

# ---------- mtk core ----------
set(MTK_CORE_SOURCES
    src/core/exec.cpp
    src/core/utils.cpp
    src/core/tee.cpp
    src/core/config.cpp
    src/core/toml_filter.cpp
    src/core/diagnostic.cpp
    src/core/exit_codes.cpp
    src/core/exe_path.cpp
    src/core/run_context.cpp
    src/core/signals.cpp
    src/core/registry.cpp
    src/core/passthrough_filter.cpp
    src/core/toml_filter_adapter.cpp
    src/core/default_registry.cpp
    src/core/trust.cpp
    src/core/audit.cpp
    src/core/filter_cache.cpp
    src/core/platform/paths.cpp
    src/core/platform/file_lock.cpp
    src/core/color.cpp
    src/core/platform/tty.cpp
)

set(MTK_CMD_SOURCES
    src/cmds/git.cpp
    src/cmds/git_log.cpp
    src/cmds/git_status.cpp
    src/cmds/git_diff.cpp
    src/cmds/git_show.cpp
    src/cmds/ls.cpp
    src/cmds/grep.cpp
    src/cmds/hook.cpp
    src/cmds/init.cpp
    src/cmds/meta.cpp
)

add_library(mtk_lib STATIC ${MTK_CORE_SOURCES} ${MTK_CMD_SOURCES})
target_include_directories(mtk_lib
    PUBLIC
        src
        ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(mtk_lib
    PUBLIC
        reproc++
        tomlplusplus::tomlplusplus
        fmt::fmt
        nlohmann_json::nlohmann_json
)

add_executable(mtk src/main.cpp)
target_link_libraries(mtk PRIVATE mtk_lib)

enable_testing()
add_subdirectory(tests)

# ---------- install ----------
include(GNUInstallDirs)
install(TARGETS mtk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY filters/ DESTINATION ${CMAKE_INSTALL_DATADIR}/mtk/filters
        FILES_MATCHING PATTERN "*.toml")
install(FILES README.md INSTALL.md GUIDELINES.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
