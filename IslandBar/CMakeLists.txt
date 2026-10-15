# =====================================================================
#  IslandBar -- the top-level build
#
#  A Windows 11 taskbar replacement: a Dynamic-Island style bar at the top of the
#  screen, carrying the Start button, the open windows and a quick-settings panel,
#  and hiding the shell's own taskbar. C++20, Direct2D, DirectComposition.
#
#  The tree:
#      src/       the bar itself, one directory per module (src/CMakeLists.txt)
#      tools/     development probes; none of them is shipped (tools/CMakeLists.txt)
#      setup/     the per-user installer, which carries the bar inside itself
#      cmake/     shared CMake modules
#      scripts/   the PowerShell that builds, runs, drives and photographs the bar
#      docs/      the handoff, the feature list, and the verified screenshots
#
#  Build it with scripts\build.ps1. That script finds the toolchain -- the CMake
#  bundled with Visual Studio, the Windows SDK -- in a way a bare `cmake` does not.
# =====================================================================

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

# ON, not OFF, and that is a decision rather than a default:
#
# It was OFF, which meant "the tree builds warning-free" was a thing somebody had to notice, and it was not
# true: two C4456 shadowing warnings sat in main.cpp and Diagnostics.cpp for as long as those files were not
# rebuilt, and only appeared when the geometry header changed and both were recompiled. A build that is green
# by luck is not a build anybody else can inherit. With this ON, a new warning stops the build at the moment it
# is introduced, with the file and line in the message -- which is the cheapest possible review.
#
# If a third-party header (C++/WinRT is the only candidate) ever emits a warning that cannot be fixed here, the
# answer is to silence THAT warning for THAT include with a comment and a reason, not to turn this back off.
option(ISLANDBAR_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

if(MSVC AND ISLANDBAR_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
endif()

# --- helper modules --------------------------------------------------
# The C++/WinRT setup. The bar and two of the probes need it, so it is written once.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(WinRt)

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
# The bar first: the installer embeds its executable, and two of the probes are
# compiled from its sources.
add_subdirectory(src)
add_subdirectory(tools)

# The installer: carries the bar inside itself, installs it for the current user, and
# uninstalls it again. It needs the bar, so it comes after it.
option(ISLANDBAR_BUILD_SETUP "Build the per-user installer" ON)
if(ISLANDBAR_BUILD_SETUP)
    add_subdirectory(setup)
endif()
