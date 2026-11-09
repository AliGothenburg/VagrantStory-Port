cmake_minimum_required(VERSION 3.20)
project(VagrantStoryPort VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8 /EHsc /permissive- /bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(VSPORT_LINK_AOT "Compile and link generated C++20 AOT modules into vs_host if present" ON)

add_library(vsport_core STATIC
    src/cdrom_iso.cpp
    src/psx_cdrom.cpp
    src/psx_exe.cpp
    src/mips_decoder.cpp
    src/gte.cpp
    src/psx_gpu.cpp
    src/psx_mdec.cpp
    src/psx_spu.cpp
    src/runtime.cpp
    src/cfg_lifter.cpp
)

target_include_directories(vsport_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(vs_inspect tools/vs_inspect.cpp)
target_link_libraries(vs_inspect PRIVATE vsport_core)

add_executable(vs_recomp tools/vs_recomp.cpp)
target_link_libraries(vs_recomp PRIVATE vsport_core)

add_executable(vs_host
    tools/vs_host.cpp
    src/d3d11_renderer.cpp
    src/win32_audio.cpp
    src/win32_input.cpp
)
target_link_libraries(vs_host PRIVATE vsport_core)

if(VSPORT_LINK_AOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated_recomp/vs_aot_registry.cpp")
    file(GLOB VSPORT_AOT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated_recomp/*.cpp")
    add_library(vsport_aot STATIC ${VSPORT_AOT_SOURCES})
    target_link_libraries(vsport_aot PUBLIC vsport_core)
    target_include_directories(vsport_aot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/generated_recomp")
    target_link_libraries(vs_host PRIVATE vsport_aot)
    target_compile_definitions(vs_host PRIVATE VSPORT_HAS_AOT_MODULES=1)
    message(STATUS "[vsport] Native C++20 AOT modulleri vs_host icine baglaniyor (${CMAKE_CURRENT_SOURCE_DIR}/generated_recomp)")
endif()

if(WIN32)
    target_link_libraries(vs_host PRIVATE user32 gdi32 d3d11 dxgi d3dcompiler winmm xinput dinput8 dxguid)
endif()

enable_testing()
add_executable(vsport_tests tests/test_core.cpp)
target_link_libraries(vsport_tests PRIVATE vsport_core)
add_test(NAME vsport_core_tests COMMAND vsport_tests)
