# geoprobe: can this machine tell us where it is?
#
# C++/WinRT, and the three settings below are copied from src/CMakeLists.txt (and
# from gsmtcprobe) because getting any of them wrong produces an error that does not
# name the cause:
#
#   * the projection headers live under the SDK's cppwinrt\winrt and are not on the
#     default include path;
#   * the Windows Runtime is reached through WindowsApp.lib, not through a classic
#     import library -- without it the link fails on RoGetActivationFactory with no
#     hint that a library is missing;
#   * /await is required with /permissive- (set at the top level), because the WinRT
#     headers use awaitable types the standard does not allow otherwise.
add_executable(geo_probe main.cpp)

target_compile_features(geo_probe PRIVATE cxx_std_20)

if(MSVC)
    set(ISLANDBAR_WINSDK_ROOT
        "$ENV{WindowsSdkDir}Include\\${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

    if(EXISTS "${ISLANDBAR_WINSDK_ROOT}/cppwinrt/winrt")
        target_include_directories(geo_probe PRIVATE "${ISLANDBAR_WINSDK_ROOT}/cppwinrt")
    else()
        message(WARNING
            "geo_probe: cppwinrt projection headers not found under "
            "${ISLANDBAR_WINSDK_ROOT}; the probe will not compile. Configure from an MSVC "
            "environment (scripts\\env.ps1) so WindowsSdkDir is set.")
    endif()

    target_compile_options(geo_probe PRIVATE /await)
    target_link_libraries(geo_probe PRIVATE windowsapp.lib)
endif()

# A console program on purpose: its whole output is text for a human to read.
target_compile_definitions(geo_probe PRIVATE _CONSOLE)
