cmake_minimum_required(VERSION 3.20)
project(mtk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)

# ---------- submodule sanity check ----------
foreach(dep CLI11 tomlplusplus reproc fmt doctest)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/${dep}/CMakeLists.txt")
        message(FATAL_ERROR
            "third_party/${dep}/ is empty or missing CMakeLists.txt — "
            "run 'git submodule update --init --recursive' "
            "or see INSTALL.md for manual setup.")
    endif()
endforeach()

# ---------- third-party deps ----------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# CLI11 (header-only target: CLI11::CLI11)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/CLI11)

# tomlplusplus (header-only target: tomlplusplus::tomlplusplus)
add_subdirectory(third_party/tomlplusplus)

# fmt (target: fmt::fmt)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fmt)

# reproc — C++ wrapper target: reproc++
set(REPROC++ ON CACHE BOOL "" FORCE)
set(REPROC_TEST OFF CACHE BOOL "" FORCE)
set(REPROC_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/reproc)

# doctest (header-only target: doctest::doctest)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/doctest)

# ---------- mtk core ----------
set(MTK_CORE_SOURCES
    src/core/exec.cpp
    src/core/utils.cpp
    src/core/tee.cpp
    src/core/config.cpp
    src/core/toml_filter.cpp
    src/core/diagnostic.cpp
    src/core/exit_codes.cpp
    src/core/run_context.cpp
)

set(MTK_CMD_SOURCES
    src/cmds/git.cpp
    src/cmds/exec.cpp
    src/cmds/ls.cpp
    src/cmds/grep.cpp
)

add_library(mtk_lib STATIC ${MTK_CORE_SOURCES} ${MTK_CMD_SOURCES})
target_include_directories(mtk_lib PUBLIC src)
target_link_libraries(mtk_lib
    PUBLIC
        reproc++
        tomlplusplus::tomlplusplus
        fmt::fmt
)

add_executable(mtk src/main.cpp)
target_link_libraries(mtk PRIVATE mtk_lib CLI11::CLI11)

enable_testing()
add_subdirectory(tests)
