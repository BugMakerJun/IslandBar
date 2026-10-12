# Smoke test: decodes UTF-8 source correctly and links the island's API surface.
add_executable(island_smoke main.cpp)

target_link_libraries(island_smoke PRIVATE
    d3d11
    dxgi
    d2d1
    dwrite
    dcomp
    dwmapi
    shcore
    shell32
    ole32
    oleaut32
    uuid
    uiautomationcore
    windowscodecs
    propsys
    shlwapi
    user32
    gdi32
)

# The smoke test prints Chinese, so the console must be UTF-8 aware.
target_compile_definitions(island_smoke PRIVATE _CONSOLE)
