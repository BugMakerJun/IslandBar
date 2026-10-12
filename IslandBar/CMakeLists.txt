cmake_minimum_required(VERSION 3.20)

project(IslandBar LANGUAGES CXX
        DESCRIPTION "Windows 11 taskbar replacement with a Dynamic-Island style bar")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Global compile settings -----------------------------------------
if(MSVC)
    add_compile_options(
        /W4                 # high warning level
        /permissive-        # conforming mode
        /utf-8              # source and execution charset = UTF-8
        /EHsc
        /Zc:__cplusplus
        /Zc:preprocessor
        /MP                 # parallel compilation
    )
    add_compile_definitions(
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    add_link_options(/DEBUG:FULL)
endif()

option(ISLANDBAR_BUILD_SMOKE "Build the toolchain smoke test" ON)
option(ISLANDBAR_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(MSVC AND ISLANDBAR_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
endif()

# --- Third-party dependencies ----------------------------------------
# sqlite3 is vendored as the amalgamation, fetched at configure time.
# It is needed only by the notification subsystem, so it stays optional
# until that subsystem exists.
include(FetchContent)
option(ISLANDBAR_FETCH_SQLITE "Download the SQLite amalgamation" OFF)

if(ISLANDBAR_FETCH_SQLITE)
    FetchContent_Declare(
        sqlite_amalgamation
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sqlite_amalgamation)
    add_library(sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
    target_include_directories(sqlite3 PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
endif()

# --- Targets ---------------------------------------------------------
if(ISLANDBAR_BUILD_SMOKE)
    add_subdirectory(tools/smoke)
endif()

# A standalone harness for the JSON reader and the notification layer. It links
# only those two modules, so a hang in the parser cannot be mistaken for a hang in
# the DirectComposition setup that surrounds it in the main self-test.
option(ISLANDBAR_BUILD_JSONPROBE "Build the JSON/notification probe" ON)
if(ISLANDBAR_BUILD_JSONPROBE)
    add_subdirectory(tools/jsonprobe)
endif()

# Asks whether WeChat reports its calls through the media session API. That
# decides whether the call pill can be driven from the C++/WinRT media path the
# bar already uses, or has to be driven from WeChat's call window instead. The
# question cannot be asked from PowerShell on this machine, whose WindowsRuntime
# interop is not loadable, so it needs a C++/WinRT program -- this one.
option(ISLANDBAR_BUILD_GSMTCPROBE "Build the media-session capability probe" ON)
if(ISLANDBAR_BUILD_GSMTCPROBE)
    add_subdirectory(tools/gsmtcprobe)
endif()

# The real bar: the DComp/D2D window foundation plus the phase 0-d self-test.
add_subdirectory(src)
