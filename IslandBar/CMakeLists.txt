cmake_minimum_required(VERSION 3.20)

project(IslandBar LANGUAGES CXX
        DESCRIPTION "Windows 11 taskbar replacement with a Dynamic-Island style bar")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- the C runtime is linked IN, not borrowed -----------------------------------------
#
# The bar is handed to people as a single executable, and the default on MSVC is a binary that
# needs the Visual C++ runtime as a separate installation. That is survivable for a developer and
# fatal for everyone else -- and worse in Debug, where the dependency is on the DEBUG runtime
# (ucrtbased.dll, MSVCP140D.dll), which is not redistributable and exists only on machines with the
# build tools. Measured with `dumpbin /dependents` on the first installer built here: the bar and
# the installer both named all four of those DLLs.
#
# MultiThreaded$<$<CONFIG:Debug>:Debug> is "static for every configuration": /MT in Release,
# /MTd in Debug. After this the only DLLs either binary imports are ones Windows already has.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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

# Asks Windows for the machine's own position. The weather on the status line needs
# coordinates, and the two sources that can supply them are a city in weather.txt and
# this -- so whether this works, and what it answers on a machine whose traffic leaves
# through a proxy, has to be measured rather than assumed. See tools/geoprobe.
option(ISLANDBAR_BUILD_GEOPROBE "Build the location capability probe" ON)
if(ISLANDBAR_BUILD_GEOPROBE)
    add_subdirectory(tools/geoprobe)
endif()

# Two windows of one ordinary program, and a DWM thumbnail between them. The bar's own preview said
# "registered, visible, right place, nothing drawn", and separating the recipe from the machine needed a
# program that does nothing else. See tools/thumbprobe.
option(ISLANDBAR_BUILD_THUMBPROBE "Build the DWM thumbnail probe" ON)
if(ISLANDBAR_BUILD_THUMBPROBE)
    add_subdirectory(tools/thumbprobe)
endif()

# The real bar: the DComp/D2D window foundation plus the phase 0-d self-test.
add_subdirectory(src)

# The installer: carries the bar inside itself, installs it for the current user, and
# uninstalls it again. It needs the bar, so it comes after it.
option(ISLANDBAR_BUILD_SETUP "Build the per-user installer" ON)
if(ISLANDBAR_BUILD_SETUP)
    add_subdirectory(setup)
endif()
