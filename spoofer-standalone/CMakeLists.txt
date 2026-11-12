cmake_minimum_required(VERSION 3.22)
project(CannonSpoofer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor /EHsc /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)
endif()

# Shared library — fingerprint + rotator + marker
add_library(cfspoof_core STATIC
    src/fingerprint.cpp
    src/rotator.cpp
    src/marker.cpp
)
target_include_directories(cfspoof_core PUBLIC include)
target_link_libraries(cfspoof_core PUBLIC
    advapi32
    setupapi
    cfgmgr32
    iphlpapi
    bcrypt
    shell32
)

# One EXE per verb — admin needed for registry writes + driver IOCTL.
foreach(exe IN ITEMS apply restore burn status)
    add_executable(spoof-${exe} src/main_${exe}.cpp)
    target_link_libraries(spoof-${exe} PRIVATE cfspoof_core)
    set_target_properties(spoof-${exe} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        OUTPUT_NAME "spoof-${exe}"
    )
endforeach()
