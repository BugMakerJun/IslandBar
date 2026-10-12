# thumbprobe: does DWM draw a live thumbnail into a window of ours?
#
# No C++/WinRT here -- this is plain Win32 plus dwmapi, because the question is about the compositor and
# the fewer moving parts between the question and the answer the better.
add_executable(thumb_probe main.cpp)

target_compile_features(thumb_probe PRIVATE cxx_std_20)

if(MSVC)
    target_link_libraries(thumb_probe PRIVATE dwmapi.lib user32.lib gdi32.lib)
endif()

# A console program on purpose: its whole output is text for a human to read.
target_compile_definitions(thumb_probe PRIVATE _CONSOLE)
