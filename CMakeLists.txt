cmake_minimum_required(VERSION 3.22)
project(CannonFortnite LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor /EHsc /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
endif()

# --------------------------------------------------------------------
# imgui — vendored under third_party/imgui. The UI module uses the
# win32 + dx11 backends, but swaps to the game's own device at runtime
# (the Present hook does the plumbing).
# --------------------------------------------------------------------
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
    message(WARNING "third_party/imgui not vendored — UI target will stub.")
    add_library(imgui INTERFACE)
endif()

# --------------------------------------------------------------------
# cheat.dll — the main module.
#
# Lives inside Fortnite. Boots the overlay + feature pipelines from
# DllMain → Boot thread (see src/dllmain.cpp). Named `cheat.dll` on
# disk; rename by changing the OUTPUT_NAME below if you want it to
# look like something else.
# --------------------------------------------------------------------
file(GLOB_RECURSE CF_SOURCES
    src/core/*.cpp
    src/sdk/*.cpp
    src/features/*.cpp
    src/overlay/*.cpp
    src/antiban/*.cpp
    src/util/*.cpp
    src/dllmain.cpp
    src/main.cpp
)

add_library(cheat SHARED ${CF_SOURCES})
target_include_directories(cheat PRIVATE include)
target_link_libraries(cheat PRIVATE
    imgui
    d3d11 dxgi d3dcompiler
    user32 gdi32
    advapi32
    psapi
    winmm
    dwmapi
    bcrypt
    shell32
)
set_target_properties(cheat PROPERTIES
    OUTPUT_NAME "cheat"
    PREFIX ""                       # no "lib" prefix
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# --------------------------------------------------------------------
# loader.exe — the standalone injector. Waits for Fortnite, writes
# cheat.dll path into the target, calls LoadLibraryW remotely.
# --------------------------------------------------------------------
add_executable(loader loader/main.cpp)
target_link_libraries(loader PRIVATE advapi32)
set_target_properties(loader PROPERTIES
    OUTPUT_NAME "loader"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# --------------------------------------------------------------------
# Auxiliary DLL slots — placeholders for when we split the monolith.
# Each currently empty; drop sources in and uncomment. They will all
# land in build/bin alongside cheat.dll and loader.exe.
#
# add_library(cheat_core      SHARED src/core/dummy.cpp)
# add_library(cheat_antiban   SHARED src/antiban/dummy.cpp)
# add_library(cheat_ui        SHARED src/overlay/dummy.cpp)
# add_library(cheat_features  SHARED src/features/dummy.cpp)
#
# foreach(aux cheat_core cheat_antiban cheat_ui cheat_features)
#     target_include_directories(${aux} PRIVATE include)
#     set_target_properties(${aux} PROPERTIES
#         OUTPUT_NAME "${aux}"
#         PREFIX ""
#         RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
#         LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
#     )
# endforeach()
# --------------------------------------------------------------------
