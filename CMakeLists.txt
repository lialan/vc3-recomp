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
# The generated code includes its own copy of the runtime header; keep it the
# toolkit's (the register layout must match the runtime's), as lift.sh would.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen")
    configure_file("${XBOXRECOMP_DIR}/templates/runtime/recomp_types.h"
                   "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/recomp_types.h" COPYONLY)
endif()
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR "src/recomp/gen is empty: run scripts/lift.sh first")
endif()

set(VC3_SOURCES
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

if(ANDROID)
    # Meta Quest (quest/build.sh): SDL's Java side loads libmain.so and calls
    # SDL_main (src/android/android_main.c), which runs main.c's main; the
    # headset, its controllers and the window are src/android/quest_host.c.
    # The GL back end is the same file built for GLES 3.2 on EGL.
    set(OPENXR_DIR "" CACHE PATH "OpenXR headers (the loader .aar's prefab headers)")
    set(OPENXR_LIB "" CACHE FILEPATH "libopenxr_loader.so for arm64-v8a")
    find_package(SDL2 REQUIRED)
    add_library(vc3 SHARED ${VC3_SOURCES}
        src/android/android_main.c
        src/android/quest_host.c
        src/android/quest_overlay.c
        src/android/quest_menu.c)
    set_target_properties(vc3 PROPERTIES OUTPUT_NAME main)
    set_source_files_properties(src/main.c PROPERTIES COMPILE_DEFINITIONS main=vc3_main)
    target_compile_definitions(vc3 PRIVATE VC3_GLES=1)
    target_include_directories(vc3 PRIVATE ${OPENXR_DIR})
    # Calls between the generated functions bind inside libmain.so, not
    # through the PLT: 5600 jump slots, one indirect branch per guest call.
    # Only SDL_main and the JNI entry points are called from outside.
    target_link_options(vc3 PRIVATE "-Wl,-Bsymbolic-functions")
    target_link_libraries(vc3 PRIVATE xboxrecomp SDL2::SDL2 ${OPENXR_LIB}
        EGL GLESv3 android log m)
else()
    add_executable(vc3 ${VC3_SOURCES} src/vr_view_desktop.c)
    target_link_libraries(vc3 PRIVATE xboxrecomp "-framework OpenGL")
endif()

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

# The ring of recent indirect-call targets (and the call count) that the crash
# reports print: three volatile stores per indirect call, so off unless asked.
option(VC3_ICALL_TRACE "Record recent indirect calls for crash reports" OFF)
if(VC3_ICALL_TRACE)
    target_compile_definitions(vc3 PRIVATE RECOMP_ICALL_TRACE=1)
endif()

# Let the crash handler name the lifted function at a host PC (dladdr).
if(APPLE)
    target_link_options(vc3 PRIVATE -Wl,-export_dynamic)
endif()
