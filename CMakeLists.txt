cmake_minimum_required(VERSION 3.22)
project(CannonFortnite LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor /EHsc /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    # SUBSYSTEM:WINDOWS pairs with WinMainCRTStartup (default entry
    # the CRT selects based on the `WinMain` symbol in the program).
    # Do NOT force /ENTRY — the CRT picks the right startup shim.
    add_link_options(/SUBSYSTEM:WINDOWS)
else()
    add_compile_options(-Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
endif()

# --- imgui (bundled as single-tree source drop; the user vendors it) ---
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
if(EXISTS ${IMGUI_DIR}/imgui.h)
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
else()
    message(WARNING "third_party/imgui not vendored — overlay target will stub.")
    add_library(imgui INTERFACE)
endif()

file(GLOB_RECURSE CF_SOURCES
    src/core/*.cpp
    src/sdk/*.cpp
    src/features/*.cpp
    src/overlay/*.cpp
    src/antiban/*.cpp
    src/spoofer/*.cpp
    src/util/*.cpp
    src/main.cpp
)

add_executable(cannonfn ${CF_SOURCES})
target_include_directories(cannonfn PRIVATE include)
target_link_libraries(cannonfn PRIVATE
    imgui
    d3d11 dxgi d3dcompiler
    ntdll
    advapi32
    user32 gdi32
    psapi
    setupapi
    cfgmgr32
    iphlpapi
    winmm
    dwmapi
    bcrypt
    shell32
)

set_target_properties(cannonfn PROPERTIES
    # v5: 'svchost_helper' was a flagged heuristic (svc* outside SYSTEM).
    # 'MicrosoftEdgeUpdateBroker' matches a real signed helper that
    # legitimate users have in their %LOCALAPPDATA% install of Edge —
    # process-name allowlisting won't flag it. Still owns a human-
    # readable version-resource below so no shady icon on taskbar.
    OUTPUT_NAME "MicrosoftEdgeUpdateBroker"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
