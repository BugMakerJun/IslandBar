# gsmtc_probe: does WeChat report its call through the media session API?
#
# C++/WinRT. What that needs beyond a plain console target is in cmake/WinRt.cmake,
# because getting any of it wrong produces an error that does not name its cause.
add_executable(gsmtc_probe main.cpp)

target_compile_features(gsmtc_probe PRIVATE cxx_std_20)

islandbar_enable_cppwinrt(gsmtc_probe)

# A console program on purpose: its whole output is text for a human to read.
target_compile_definitions(gsmtc_probe PRIVATE _CONSOLE)
