cmake_minimum_required(VERSION 3.20)

# Sega Chihiro games, statically recompiled: one folder per game under games/
# (vc3: Virtua Cop 3), all on the Chihiro runtime in runtime/ (chihiro_rt).
# Mirrors the Lindbergh ports' layout (gse-recomp: lindbergh_runtime(),
# games/<id>/src).
#
# A game's target only exists once its XBE has been lifted into
# games/<id>/generated (scripts/lift.sh <id>); its target is named after
# its folder (build/vc3).
#
# CHIHIRO_GAMES=<id;...> builds only those (the Quest build makes one game
# per build tree: each is the app's libmain.so).

project(chihiro_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CHIHIRO_GAMES "" CACHE STRING "games to build (empty: every one with lifted code)")
# The generated chunks are huge straight-line C, and the game's own logic runs
# in them. -O2 is the default: in VC3's combat it lifts the worst second from
# ~28 to ~40 fps. -O1 builds much faster for bring-up work. (VC3_GEN_OPT,
# this option's old name, still sets it.)
set(VC3_GEN_OPT "-O2" CACHE STRING "Optimisation level for the lifted C (old name)")
set(CHIHIRO_GEN_OPT "${VC3_GEN_OPT}" CACHE STRING "Optimisation level for the lifted C")
# The ring of recent indirect-call targets (and the call count) that the crash
# reports print: three volatile stores per indirect call, so off unless asked.
option(VC3_ICALL_TRACE "Record recent indirect calls for crash reports" OFF)

# Chihiro has 128 MB; D3D's contiguous pools need the window to match.
add_compile_definitions(XBOX_CONTIG_SIZE=0x08000000u)
if(VC3_ICALL_TRACE)
    add_compile_definitions(RECOMP_ICALL_TRACE=1)
endif()

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")
set(CHIHIRO_RENDERER "opengl" CACHE STRING "Game renderer: opengl or vulkan")
set_property(CACHE CHIHIRO_RENDERER PROPERTY STRINGS opengl vulkan)
option(CHIHIRO_VULKAN_OFFSCREEN_CLI "Build Android native Vulkan guest diagnostic without an XR session" OFF)
if(CHIHIRO_RENDERER STREQUAL "vulkan")
    set(XBOXRECOMP_NO_HOST_D3D8 ON CACHE BOOL "Guest renders through NV2A" FORCE)
elseif(NOT CHIHIRO_RENDERER STREQUAL "opengl")
    message(FATAL_ERROR "Unsupported CHIHIRO_RENDERER=${CHIHIRO_RENDERER}")
endif()
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)
add_subdirectory(runtime)

# chihiro_game(<id>): games/<id>/src/*.c (game.c: its profile; recomp_manual.c:
# the guest functions it replaces) and its lifted code, on the runtime.
function(chihiro_game id)
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/games/${id}")
    set(gen "${dir}/generated")
    file(GLOB lifted CONFIGURE_DEPENDS "${gen}/*.c")
    if(NOT lifted)
        message(STATUS "${id}: no lifted code in ${gen} yet, not built (scripts/lift.sh ${id})")
        return()
    endif()
    # The generated code includes its own copy of the runtime header; keep
    # it the toolkit's (the register layout must match the runtime's).
    configure_file("${XBOXRECOMP_DIR}/templates/runtime/recomp_types.h"
                   "${gen}/recomp_types.h" COPYONLY)
    file(GLOB game_src CONFIGURE_DEPENDS "${dir}/src/*.c")
    if(ANDROID)
        add_library(${id} SHARED ${game_src} ${lifted})
    else()
        add_executable(${id} ${game_src} ${lifted})
    endif()
    set_source_files_properties(${lifted} PROPERTIES
        COMPILE_OPTIONS "${CHIHIRO_GEN_OPT};-Wno-everything")
    chihiro_runtime(${id} "${gen}")
    if(ANDROID AND CHIHIRO_RENDERER STREQUAL "vulkan" AND CHIHIRO_VULKAN_OFFSCREEN_CLI)
        add_executable(${id}_vulkan_offscreen tools/android-vulkan-offscreen.c)
        target_link_libraries(${id}_vulkan_offscreen PRIVATE ${id})
        set_target_properties(${id}_vulkan_offscreen PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
    message(STATUS "${id}: building against lifted code in ${gen}")
endfunction()

file(GLOB game_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/games/*")
foreach(d ${game_dirs})
    if(IS_DIRECTORY "${d}" AND EXISTS "${d}/src/game.c")
        get_filename_component(id "${d}" NAME)
        if(NOT CHIHIRO_GAMES OR id IN_LIST CHIHIRO_GAMES)
            chihiro_game(${id})
        endif()
    endif()
endforeach()
