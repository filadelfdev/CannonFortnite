cmake_minimum_required(VERSION 3.22)
project(CannonFortnite LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All DLLs link the shared CRT (/MD) so STL containers (string, vector)
# can safely cross module boundaries without the "different heap" trap.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor /EHsc /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
endif()

# --------------------------------------------------------------------
# imgui — vendored under third_party/imgui. Only cheat_ui pulls it in;
# the backends install themselves on the game's own D3D11 device at
# runtime (Present hook).
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
# Shared output helper — every produced binary lands in build/bin so
# the loader can walk the folder without knowing per-target subdirs.
# --------------------------------------------------------------------
function(cf_place_binary target)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${target}"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endfunction()

# --------------------------------------------------------------------
# cheat_core.dll — process memory, engine SDK, log, report.
#
# Grafo: everybody depends on this. No downstream dependencies.
# Exports via CF_BUILDING_CORE → CF_CORE_API becomes __declspec(dllexport).
# --------------------------------------------------------------------
file(GLOB CORE_SOURCES
    src/core/*.cpp
    src/sdk/*.cpp
    src/util/*.cpp
)
add_library(cheat_core SHARED ${CORE_SOURCES})
target_include_directories(cheat_core PUBLIC include)
target_compile_definitions(cheat_core PRIVATE CF_BUILDING_CORE)
target_link_libraries(cheat_core PRIVATE
    psapi advapi32 bcrypt shell32 winmm
)
cf_place_binary(cheat_core)

# --------------------------------------------------------------------
# cheat_antiban.dll — jitter, clamp, cadence humanisation.
#
# No downstream dependencies (features + UI reach into it, not the
# other way round). Keeps the RNG state local so a cheat_core reload
# doesn't reseed the jitter stream.
# --------------------------------------------------------------------
file(GLOB AB_SOURCES src/antiban/*.cpp)
add_library(cheat_antiban SHARED ${AB_SOURCES})
target_include_directories(cheat_antiban PUBLIC include)
target_compile_definitions(cheat_antiban PRIVATE CF_BUILDING_ANTIBAN)
target_link_libraries(cheat_antiban PRIVATE bcrypt)
cf_place_binary(cheat_antiban)

# --------------------------------------------------------------------
# cheat_ui.dll — ImGui overlay + D3D11 Present hook on the game's own
# swapchain. Only this target pulls in the graphics libraries.
# --------------------------------------------------------------------
file(GLOB UI_SOURCES src/overlay/*.cpp)
add_library(cheat_ui SHARED ${UI_SOURCES})
target_include_directories(cheat_ui PUBLIC include)
target_compile_definitions(cheat_ui PRIVATE CF_BUILDING_UI)
target_link_libraries(cheat_ui PRIVATE
    cheat_core
    imgui
    d3d11 dxgi d3dcompiler
    user32 gdi32 dwmapi
)
cf_place_binary(cheat_ui)

# --------------------------------------------------------------------
# cheat_features.dll — ticks for cannon_chest, aimbot, loot, mats,
# esp, route, speed. Depends on both core (for proc + sdk) and
# antiban (for jitter + clamp).
# --------------------------------------------------------------------
file(GLOB FEAT_SOURCES src/features/*.cpp)
add_library(cheat_features SHARED ${FEAT_SOURCES})
target_include_directories(cheat_features PUBLIC include)
target_compile_definitions(cheat_features PRIVATE CF_BUILDING_FEATURES)
target_link_libraries(cheat_features PRIVATE
    cheat_core
    cheat_antiban
)
cf_place_binary(cheat_features)

# --------------------------------------------------------------------
# cheat.dll — the orchestrator the loader injects first.
#
# Boot thread LoadLibraryW's the 4 auxiliary DLLs in dependency order,
# then runs cf::Run (worker loop + overlay). This target defines NO
# CF_BUILDING_* macro → every CF_x_API in its translation units becomes
# __declspec(dllimport) and resolves against the aux DLLs' export tables.
# --------------------------------------------------------------------
add_library(cheat SHARED
    src/dllmain.cpp
    src/main.cpp
)
target_include_directories(cheat PRIVATE include)
target_link_libraries(cheat PRIVATE
    cheat_core
    cheat_antiban
    cheat_ui
    cheat_features
    user32
)
cf_place_binary(cheat)

# --------------------------------------------------------------------
# loader.exe — standalone injector. Walks build/bin and injects the
# five DLLs in dependency order: core → antiban → ui → features → cheat.
# --------------------------------------------------------------------
add_executable(loader loader/main.cpp)
target_link_libraries(loader PRIVATE advapi32)
set_target_properties(loader PROPERTIES
    OUTPUT_NAME "loader"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
