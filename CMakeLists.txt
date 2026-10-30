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
    ${RECOMP_GEN_SOURCES}
)

target_link_libraries(vc3 PRIVATE xboxrecomp)
target_include_directories(vc3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src/kernel
    ${XBOXRECOMP_DIR}/src/platform
)

# The generated chunks are huge straight-line C; full optimisation costs a lot
# of compile time for little gain during bring-up.
set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-O1;-Wno-everything")
