# =====================================================================
#  src/CMakeLists.txt
#  The bar itself.
#
#  NOTE: this file was lost -- something in the tooling deleted it while the
#  project was being edited, and there was no version control to recover it from.
#  It has been reconstructed from the source list, the README's build notes and
#  what the build needs; the target name, the manifest embedding and the C++/WinRT
#  setup are all load-bearing and are explained below because the reasons were
#  written down here once and are worth keeping.
# =====================================================================

add_executable(island_bar
    main.cpp
    AppBar.cpp
    StartMenu.cpp
    SystemStatus.cpp
    AudioVolume.cpp
    Diagnostics.cpp
    IslandWindow.cpp
    Log.cpp
    Lyrics.cpp
    MiniJson.cpp
    Monitor.cpp
    Motion.cpp
    MusicSession.cpp
    NativeTaskbar.cpp
    Notification.cpp
    NotificationIcon.cpp
    NotificationPipe.cpp
    Renderer.cpp
    SoundOutput.cpp
    TextRenderer.cpp
    ToastStore.cpp
    WeChatWatch.cpp
    WicImage.cpp
    WindowList.cpp
)

target_include_directories(island_bar PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# --- C++/WinRT, for the system media controls (Windows.Media.Control) ---------
#
# The projection headers ship with the Windows SDK under cppwinrt\winrt, but they
# are not on the default include path, and the Windows Runtime APIs they wrap are
# reached through WindowsApp.lib rather than a classic import library.
#
# /await is required with /permissive- (set at the top level): the WinRT headers
# use awaitable types that the standard does not allow otherwise.
if(MSVC)
    set(ISLANDBAR_WINSDK_ROOT
        "$ENV{WindowsSdkDir}Include\\${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

    if(EXISTS "${ISLANDBAR_WINSDK_ROOT}/cppwinrt/winrt")
        target_include_directories(island_bar PRIVATE
            "${ISLANDBAR_WINSDK_ROOT}/cppwinrt")
    else()
        message(WARNING
            "cppwinrt projection headers not found under ${ISLANDBAR_WINSDK_ROOT}; "
            "the media controls will not compile")
    endif()

    target_compile_options(island_bar PRIVATE /await)
    target_link_libraries(island_bar PRIVATE windowsapp.lib)
endif()

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
