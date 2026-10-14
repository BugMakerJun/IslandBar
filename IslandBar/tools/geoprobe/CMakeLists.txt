# geoprobe: can this machine tell us where it is?
#
# C++/WinRT. The three settings that needs are in cmake/WinRt.cmake, because getting
# any of them wrong produces an error that does not name its cause.
add_executable(geo_probe main.cpp)

target_compile_features(geo_probe PRIVATE cxx_std_20)

islandbar_enable_cppwinrt(geo_probe)

# A console program on purpose: its whole output is text for a human to read.
target_compile_definitions(geo_probe PRIVATE _CONSOLE)
