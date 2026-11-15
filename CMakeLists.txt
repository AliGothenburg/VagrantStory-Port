cmake_minimum_required(VERSION 3.20)
project(VagrantStoryPort VERSION 0.1.0 LANGUAGES C CXX)

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
    src/savestate.cpp
    src/cheats.cpp
    src/texture_replace.cpp
    src/psx_exe.cpp
    src/mips_decoder.cpp
    src/gte.cpp
    src/psx_gpu.cpp
    src/hr_software.cpp
    src/psx_mdec.cpp
    src/psx_spu.cpp
    src/runtime.cpp
    src/memcard.cpp
    src/cfg_lifter.cpp
)

target_include_directories(vsport_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(vs_inspect tools/vs_inspect.cpp)
target_link_libraries(vs_inspect PRIVATE vsport_core)

add_executable(vs_recomp tools/vs_recomp.cpp)
target_link_libraries(vs_recomp PRIVATE vsport_core)

# RetroAchievements: official rcheevos library (MIT, third_party/rcheevos, v12.5.0)
file(GLOB RCHEEVOS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rcheevos/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rcheevos/src/rapi/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rcheevos/src/rcheevos/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rcheevos/src/rhash/*.c)
add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
target_include_directories(rcheevos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rcheevos/include)
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(rcheevos PRIVATE /W0)
endif()

set(VSPORT_HOST_SOURCES
    tools/vs_host.cpp
    tools/achievements.cpp
    tools/launcher_win32.cpp
    src/d3d11_renderer.cpp
    src/d3d11_hr_backend.cpp
    src/win32_audio.cpp
    src/win32_input.cpp
)

# vs_host: console build for development (logs, --headless, debugging options)
add_executable(vs_host ${VSPORT_HOST_SOURCES})
set(VSPORT_HOST_TARGETS vs_host)

# VagrantStory: the same host as a windowed (GUI subsystem) app to launch by double-click.
# It asks for the disc image on first start and remembers it in VagrantStory.ini.
if(WIN32)
    add_executable(VagrantStory WIN32 ${VSPORT_HOST_SOURCES})
    target_compile_definitions(VagrantStory PRIVATE VSPORT_GUI_SUBSYSTEM=1)
    if(MSVC)
        target_link_options(VagrantStory PRIVATE /ENTRY:mainCRTStartup) # keep int main()
    endif()
    list(APPEND VSPORT_HOST_TARGETS VagrantStory)
endif()

if(VSPORT_LINK_AOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated_recomp/vs_aot_registry.cpp")
    file(GLOB VSPORT_AOT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated_recomp/*.cpp")
    add_library(vsport_aot STATIC ${VSPORT_AOT_SOURCES})
    target_link_libraries(vsport_aot PUBLIC vsport_core)
    target_include_directories(vsport_aot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/generated_recomp")
    message(STATUS "[vsport] Native C++20 AOT modulleri host uygulamalarina baglaniyor (${CMAKE_CURRENT_SOURCE_DIR}/generated_recomp)")
endif()

foreach(host IN LISTS VSPORT_HOST_TARGETS)
    target_link_libraries(${host} PRIVATE vsport_core rcheevos)
    if(TARGET vsport_aot)
        target_link_libraries(${host} PRIVATE vsport_aot)
        target_compile_definitions(${host} PRIVATE VSPORT_HAS_AOT_MODULES=1)
    endif()
    if(WIN32)
        target_link_libraries(${host} PRIVATE user32 gdi32 comdlg32 comctl32 ole32 shell32 d3d11 dxgi d3dcompiler winmm xinput dinput8 dxguid)
    endif()
endforeach()

enable_testing()
add_executable(vsport_tests tests/test_core.cpp)
target_link_libraries(vsport_tests PRIVATE vsport_core)
add_test(NAME vsport_core_tests COMMAND vsport_tests)

# Graphics card upscaled picture vs software, on the WARP software device (no graphics card needed)
if(WIN32)
    add_executable(vsport_gpu_tests tests/test_gpu_hr.cpp src/d3d11_hr_backend.cpp)
    target_link_libraries(vsport_gpu_tests PRIVATE vsport_core d3d11 dxgi d3dcompiler)
    add_test(NAME vsport_gpu_tests COMMAND vsport_gpu_tests)
endif()
