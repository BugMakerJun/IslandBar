# =====================================================================
#  cmake/WinRt.cmake -- the C++/WinRT setup, in one place
#
#      islandbar_enable_cppwinrt(<target>)
#
#  Three things are needed to use the Windows Runtime projection, and each of them
#  fails in a way that does not name its cause:
#
#    * the projection headers live under the Windows SDK's cppwinrt\winrt and are
#      not on the default include path;
#    * the runtime is reached through WindowsApp.lib, not through a classic import
#      library -- without it the link fails on RoGetActivationFactory with no hint
#      that a library is missing;
#    * /await is required with /permissive- (set at the top level), because the
#      WinRT headers use awaitable types the standard does not otherwise allow.
#
#  This was written out three times -- src/CMakeLists.txt, tools/geoprobe,
#  tools/gsmtcprobe -- each copy repeating the same explanation, which is how a
#  fourth caller ends up with a subtly different version. One function now.
# =====================================================================

function(islandbar_enable_cppwinrt target)
    if(NOT MSVC)
        return()
    endif()

    # $ENV{WindowsSdkDir} is set only inside an MSVC environment (scripts\env.ps1).
    # Configured from a bare shell it is empty, the include directory below is then
    # relative and does not exist, and the existence test says so instead of letting
    # the compiler report a missing <winrt/base.h> three files deep.
    set(sdk_root "$ENV{WindowsSdkDir}Include\\${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

    if(EXISTS "${sdk_root}/cppwinrt/winrt")
        target_include_directories(${target} PRIVATE "${sdk_root}/cppwinrt")
        target_compile_options(${target} PRIVATE /await)
    else()
        message(WARNING
            "${target}: cppwinrt projection headers not found under ${sdk_root}; "
            "anything using the projection will not compile. Configure from an MSVC "
            "environment (scripts\\env.ps1) so WindowsSdkDir is set.")
    endif()

    # Linked whether or not the headers were found, and that is not a slip: the library is where
    # RoGetActivationFactory comes from, and a target that talks to the ABI directly -- which is
    # what the probes do when the projection is missing -- still needs it. Leaving the link out in
    # that case produced two unresolved Ro* symbols and a message that named no library.
    target_link_libraries(${target} PRIVATE windowsapp.lib)
endfunction()
