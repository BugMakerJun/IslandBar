# =====================================================================
#  src/CMakeLists.txt
#  The bar itself: one executable, built from the modules below.
#
#  The modules are directories, and an include is written from THIS directory --
#  `#include "core/Log.h"`, not `#include "Log.h"`. The include root is therefore
#  src/, and a file that moves between modules breaks the build loudly instead of
#  silently resolving through a stale entry on the search path.
#
#  NOTE: this file was lost once -- something in the tooling deleted it while the
#  project was being edited, and there was no version control to recover it from.
#  The target name, the manifest embedding and the C++/WinRT setup are load-bearing,
#  and the reasons are written down here because they were written down here once.
# =====================================================================

add_executable(island_bar
    main.cpp

    # --- app: the window, and what hangs off it ----------------------
    app/AppBar.cpp
    app/IslandWindow.cpp
    app/PlayerWindow.cpp

    # --- core: pure logic and the plumbing everything shares ---------
    core/Log.cpp
    core/MiniJson.cpp
    # Not a module: a compile-time rule. It fails the build if <windows.h> creeps back into the pure-logic
    # headers (see the file). Cheap, and it is the only thing here that keeps the layering honest.
    core/PurityCheck.cpp
    core/Motion.cpp
    core/ProcessMetrics.cpp

    # --- media: what is playing, and what it looks like --------------
    media/AudioVolume.cpp
    media/Lyrics.cpp
    media/MusicSession.cpp
    media/SoundOutput.cpp
    media/WicImage.cpp

    # --- platform: the Win32 and shell APIs the bar stands on --------
    platform/DisplayBrightness.cpp
    platform/GlassMaterial.cpp
    platform/Monitor.cpp
    platform/NativeTaskbar.cpp
    platform/ThumbnailPreview.cpp
    platform/WindowList.cpp

    # --- render: everything that puts pixels on the composition ------
    render/Renderer.cpp
    render/TextRenderer.cpp
    render/Theme.cpp

    # --- services: the channels that bring things in -----------------
    services/Notification.cpp
    services/NotificationIcon.cpp
    services/NotificationPipe.cpp
    services/ToastStore.cpp
    services/WeChatWatch.cpp

    # --- shell: the shell's own controls, mirrored -------------------
    shell/StartApps.cpp
    shell/StartMenu.cpp
    shell/SystemStatus.cpp
    shell/SystemToggles.cpp
    shell/TrayIcons.cpp
    shell/TrayLearn.cpp
    shell/TrayPress.cpp
    shell/Weather.cpp

    # --- selftest: the runtime assertions (-self-test) ---------------
    selftest/Diagnostics.cpp
)

# src/ is the include root: the modules are directories under it, and every
# `#include "module/Name.h"` is resolved from here.
target_include_directories(island_bar PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# --- C++/WinRT, for the system media controls (Windows.Media.Control) ---------
# island_bar, gsmtcprobe and geoprobe all need it, so it lives in one place.
islandbar_enable_cppwinrt(island_bar)

# --- the application manifest ------------------------------------------------
#
# PerMonitorV2 DPI awareness. Declared in the manifest rather than at runtime so
# Windows applies it before the first window exists; a window created without it
# is scaled by the system and every layout metric is silently wrong.
#
# Embedded by the linker with /MANIFEST:EMBED and /MANIFESTINPUT rather than with
# a .rc file, because a resource script would need its own compiler pass and an
# .rc that references one file is not worth the extra target.
target_sources(island_bar PRIVATE island_bar.manifest)
target_sources(island_bar PRIVATE island_bar.rc)

if(MSVC)
    target_link_options(island_bar PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/island_bar.manifest"
    )
endif()

# --- a GUI subsystem binary with a console when there is one -----------------
#
# WIN32_EXECUTABLE, so double-clicking it does not open a console window. The
# program still borrows the parent console when started from a shell (see
# PrepareConsole in main.cpp), and it must never write to stdout unguarded: with
# no console attached the CRT's stdout has descriptor -1, and writing there trips
# an assertion dialog instead of printing nothing. Every console write goes
# through Log.cpp.
#
# /ENTRY:mainCRTStartup because WIN32_EXECUTABLE makes the linker look for
# WinMain, and this program has an ordinary main().
set_target_properties(island_bar PROPERTIES
    WIN32_EXECUTABLE TRUE
)

if(MSVC)
    target_link_options(island_bar PRIVATE /ENTRY:mainCRTStartup)
endif()

# --- third-party pieces ------------------------------------------------------
#
# SQLite is needed only by the notification database reader, which is optional and
# off by default. Linking it here only when it exists keeps the default build free
# of the dependency.
if(TARGET sqlite3)
    target_link_libraries(island_bar PRIVATE sqlite3)
    target_compile_definitions(island_bar PRIVATE ISLANDBAR_HAVE_SQLITE=1)
endif()

# --- the module that lives inside other applications -------------------------
#
# A separate binary on purpose: it is loaded into OTHER processes to watch them register their tray icons
# (see trayhook/TrayHook.cpp), and it is written beside the bar because that is where the bar loads it from.
add_subdirectory(trayhook)
