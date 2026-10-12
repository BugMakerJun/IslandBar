# gsmtc_probe: does WeChat report its call through the media session API?
#
# C++/WinRT. Two things are needed beyond a plain console target, and both are
# copied from src/CMakeLists.txt because getting either wrong produces an error
# that does not name the cause:
#
#   * the projection headers live under the SDK's cppwinrt\winrt and are not on
#     the default include path;
#   * the Windows Runtime is reached through WindowsApp.lib, not through a
#     classic import library -- without it the link fails on
#     RoGetActivationFactory with no hint that a library is missing.
#
# /await is required with /permissive- (set at the top level): the WinRT headers
# use awaitable types the standard does not allow otherwise.
add_executable(gsmtc_probe main.cpp)

target_compile_features(gsmtc_probe PRIVATE cxx_std_20)

if(MSVC)
    set(ISLANDBAR_WINSDK_ROOT
        "$ENV{WindowsSdkDir}Include\\${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

    if(EXISTS "${ISLANDBAR_WINSDK_ROOT}/cppwinrt/winrt")
        target_include_directories(gsmtc_probe PRIVATE "${ISLANDBAR_WINSDK_ROOT}/cppwinrt")
        target_compile_options(gsmtc_probe PRIVATE /await)
        target_link_libraries(gsmtc_probe PRIVATE windowsapp.lib)
    else()
        message(WARNING
            "gsmtc_probe: cppwinrt projection headers not found under "
            "${ISLANDBAR_WINSDK_ROOT}; the probe will not compile. Configure from an "
            "MSVC environment (scripts\\env.ps1) so WindowsSdkDir is set.")
    endif()

    # WindowsApp.lib whether or not the projection headers were found, and that is not a mistake.
    #
    # The library is what RoGetActivationFactory comes from, and the objects that need it are compiled from
    # this tool's own source -- which uses the ABI directly when the projection is missing. Leaving the link
    # out in that case produced exactly the failure this file's header warns about: two unresolved Ro* symbols
    # and a message that names no library. The build then fails for a tool nobody asked for, which is how a
    # missing link line turns into "the build is broken".
    if(NOT TARGET windowsapp_linked)
        target_link_libraries(gsmtc_probe PRIVATE windowsapp.lib)
    endif()
endif()

# A console program on purpose: its whole output is text for a human to read.
target_compile_definitions(gsmtc_probe PRIVATE _CONSOLE)
