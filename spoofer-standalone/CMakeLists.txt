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

# Single EXE — interactive menu when launched with no args, or
# `spoofer apply|restore|burn|status` for scripts.
add_executable(spoofer
    src/main.cpp
    src/fingerprint.cpp
    src/rotator.cpp
    src/marker.cpp
)
target_include_directories(spoofer PRIVATE include)
target_link_libraries(spoofer PRIVATE
    advapi32
    setupapi
    cfgmgr32
    iphlpapi
    bcrypt
    shell32
)
set_target_properties(spoofer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    OUTPUT_NAME "spoofer"
)
