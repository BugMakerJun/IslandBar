# =====================================================================
#  tools/jsonprobe/CMakeLists.txt
#  A standalone harness for the JSON reader and the notification layer.
#
#  It links only those two modules, so a hang in the parser cannot be confused
#  with a hang in the DirectComposition setup. Not part of the shipped bar.
# =====================================================================

add_executable(jsonprobe
    main.cpp
    "${CMAKE_SOURCE_DIR}/src/services/Notification.cpp"
)

target_include_directories(jsonprobe PRIVATE "${CMAKE_SOURCE_DIR}/src")

# The JSON reader lives in the pure-logic library now (src/domain), so this probe links that library instead
# of compiling the reader itself -- the same boundary the bar uses.
target_link_libraries(jsonprobe PRIVATE island_domain)
