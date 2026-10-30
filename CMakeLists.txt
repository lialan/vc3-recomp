cmake_minimum_required(VERSION 3.20)

# Virtua Cop 3 (Sega Chihiro) - static recompilation, macOS/Linux host.

project(vc3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Chihiro has 128 MB; D3D's contiguous pools need the window to match.
add_compile_definitions(XBOX_CONTIG_SIZE=0x08000000u)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# Lifted code from tools.recomp (gitignored; regenerate with scripts/lift.sh).
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR "src/recomp/gen is empty: run scripts/lift.sh first")
endif()

add_executable(vc3
    src/main.c
    src/recomp_manual.c
    src/chihiro/chihiro.c
    src/chihiro/storage.c
    src/chihiro/jvs.c
    src/chihiro/baseboard.c
    src/d3d_fence.c
    src/host_input.c
    src/display_settings.c
    src/gpu/gl_backend.c
    ${RECOMP_GEN_SOURCES}
)

target_link_libraries(vc3 PRIVATE xboxrecomp "-framework OpenGL")

# The Display menu (frame rate) in the menu bar SDL builds; Cocoa, so macOS
# only. Elsewhere there is no menu and VC3_FPS / F10 / the settings file
# still set the rate.
if(APPLE)
    enable_language(OBJC)
    target_sources(vc3 PRIVATE src/macos_menu.m)
    target_compile_definitions(vc3 PRIVATE VC3_MACOS_MENU=1)
    target_link_libraries(vc3 PRIVATE "-framework Cocoa")
endif()
target_include_directories(vc3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src/kernel
    ${XBOXRECOMP_DIR}/src/platform
)

# The generated chunks are huge straight-line C, and the game's own logic runs
# in them. -O2 is the default: in combat it lifts the worst second from ~28 to
# ~40 fps. VC3_GEN_OPT=-O1 builds much faster for bring-up work.
set(VC3_GEN_OPT "-O2" CACHE STRING "Optimisation level for the lifted C")
set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "${VC3_GEN_OPT};-Wno-everything")

# Let the crash handler name the lifted function at a host PC (dladdr).
target_link_options(vc3 PRIVATE -Wl,-export_dynamic)
