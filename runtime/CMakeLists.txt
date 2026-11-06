# The Chihiro runtime: everything a Sega Chihiro game runs on besides its own
# lifted code -- the baseboard / JVS / storage HLE, D3D's fence wait, the GL
# back end (and on the Quest the OpenXR host), the desktop host -- built once
# as chihiro_rt, and chihiro_runtime(<target>) to put a game on it. Mirrors
# the Lindbergh runtime's lindbergh_runtime() (lindberghrecomp/CMakeLists.txt).
#
# What a game is to the runtime is its ChihiroProfile (src/chihiro/
# chihiro_profile.h), returned by chihiro_game() in games/<id>/src/game.c.

set(CHIHIRO_RT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src CACHE INTERNAL "")

set(rt_sources
    src/chihiro/chihiro.c
    src/chihiro/storage.c
    src/chihiro/jvs.c
    src/chihiro/baseboard.c
    src/chihiro/ch_env.c
    src/chihiro/chihiro_profile.c
    src/chihiro/d3d_fence.c
    src/chihiro/cri_lock.c
    src/host/host_input.c
    src/host/display_settings.c
    src/host/recomp_diag.c
    src/host/vr_rifle.c
    src/host/vr_lens.c
    src/host/scope_game.c
    src/host/retail_pad.c
    )
if(CHIHIRO_RENDERER STREQUAL "vulkan")
    list(APPEND rt_sources src/gpu/vulkan/vk_runtime.c)
    set(CHIHIRO_BUILD_VULKAN ON CACHE BOOL "Build experimental Vulkan backend library" FORCE)
else()
    list(APPEND rt_sources src/gpu/gl_backend.c src/gpu/gl_texdec.c)
endif()
# Desktop Linux with a headset (Steam Frame under SteamVR): the Quest's
# OpenXR host on the Vulkan back end, instead of the toolkit's window.
option(CHIHIRO_XR_DESKTOP "Linux: the OpenXR headset host (Vulkan)" OFF)
if(ANDROID)
    # Meta Quest (quest/build.sh): the headset, its controllers and the
    # window; the GL back end built for GLES 3.2 on EGL.
    set(OPENXR_DIR "" CACHE PATH "OpenXR headers (the loader .aar's prefab headers)")
    set(OPENXR_LIB "" CACHE FILEPATH "libopenxr_loader.so for arm64-v8a")
    find_package(SDL2 REQUIRED)
    list(APPEND rt_sources src/xr/quest_host.c src/xr/quest_overlay.c src/xr/quest_menu.c src/xr/host_stats.c)
elseif(CHIHIRO_XR_DESKTOP)
    if(NOT CHIHIRO_RENDERER STREQUAL "vulkan")
        message(FATAL_ERROR "CHIHIRO_XR_DESKTOP needs CHIHIRO_RENDERER=vulkan")
    endif()
    set(OPENXR_DIR "/usr/include" CACHE PATH "OpenXR headers (the folder holding openxr/)")
    find_library(OPENXR_LIB NAMES openxr_loader REQUIRED)
    find_package(SDL2 REQUIRED)
    list(APPEND rt_sources src/xr/quest_host.c src/xr/quest_overlay.c src/xr/quest_menu.c src/xr/host_stats.c)
else()
    list(APPEND rt_sources src/host/vr_view_desktop.c)
endif()

add_library(chihiro_rt STATIC ${rt_sources})
# What the runtime and a game's code include: the runtime's headers, the
# toolkit's (recomp_types.h as the toolkit's template: the register layout
# must be the runtime's).
set(rt_includes
    ${CHIHIRO_RT_DIR}
    ${CHIHIRO_RT_DIR}/host
    ${CHIHIRO_RT_DIR}/xr
    ${CHIHIRO_RT_DIR}/gpu
    ${XBOXRECOMP_DIR}/templates/runtime
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src/kernel
    ${XBOXRECOMP_DIR}/src/platform)
target_include_directories(chihiro_rt PUBLIC ${rt_includes})
target_link_libraries(chihiro_rt PUBLIC xboxrecomp)
if(ANDROID)
    if(CHIHIRO_RENDERER STREQUAL "opengl")
        target_compile_definitions(chihiro_rt PUBLIC VC3_GLES=1)
        target_link_libraries(chihiro_rt PUBLIC EGL GLESv3)
    endif()
    target_include_directories(chihiro_rt PUBLIC ${OPENXR_DIR})
    target_link_libraries(chihiro_rt PUBLIC SDL2::SDL2 ${OPENXR_LIB} android log m)
elseif(CHIHIRO_XR_DESKTOP)
    target_compile_definitions(chihiro_rt PUBLIC CHIHIRO_XR_DESKTOP=1)
    target_include_directories(chihiro_rt PUBLIC ${OPENXR_DIR})
    # the loader may be the static one (SteamOS ships libopenxr_loader.a)
    target_link_libraries(chihiro_rt PUBLIC SDL2::SDL2 ${OPENXR_LIB} stdc++ dl pthread m)
endif()
if(CHIHIRO_RENDERER STREQUAL "vulkan")
    target_compile_definitions(chihiro_rt PUBLIC CHIHIRO_RENDERER_VULKAN=1)
    target_link_libraries(chihiro_rt PUBLIC chihiro_vulkan)
elseif(APPLE)
    target_link_libraries(chihiro_rt PUBLIC "-framework OpenGL")
endif()
# The Display menu (frame rate) in the menu bar SDL builds; Cocoa, so macOS
# only. Elsewhere there is no menu and <prefix>_FPS / F10 / the settings
# file still set the rate.
if(APPLE)
    enable_language(OBJC)
    target_sources(chihiro_rt PRIVATE src/host/macos_menu.m)
    target_compile_definitions(chihiro_rt PUBLIC VC3_MACOS_MENU=1)
    target_link_libraries(chihiro_rt PUBLIC "-framework Cocoa")
endif()

# chihiro_runtime(<target> <generated dir>): the game target on the runtime.
# The entry points are the target's own objects (a static library's main or
# SDL_main would not be linked in), as is the profile the runtime asks for.
function(chihiro_runtime target generated_dir)
    if(ANDROID)
        target_sources(${target} PRIVATE ${CHIHIRO_RT_DIR}/host/main.c
                                         ${CHIHIRO_RT_DIR}/xr/android_main.c)
        # main.c's main, run by SDL_main (android_main.c)
        set_source_files_properties(${CHIHIRO_RT_DIR}/host/main.c TARGET_DIRECTORY ${target}
            PROPERTIES COMPILE_DEFINITIONS main=chihiro_main)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME main)
        # Calls between the generated functions bind inside libmain.so, not
        # through the PLT: 5600 jump slots, one indirect branch per guest
        # call. Only SDL_main and the JNI entry points are called from outside.
        target_link_options(${target} PRIVATE "-Wl,-Bsymbolic-functions")
    else()
        target_sources(${target} PRIVATE ${CHIHIRO_RT_DIR}/host/main.c)
    endif()
    target_include_directories(${target} PRIVATE ${generated_dir})
    target_link_libraries(${target} PRIVATE chihiro_rt)
    # Let the crash handler name the lifted function at a host PC (dladdr).
    if(APPLE)
        target_link_options(${target} PRIVATE -Wl,-export_dynamic)
    endif()
endfunction()

# Direct Vulkan remains optional alongside the reference OpenGL runtime.
option(CHIHIRO_BUILD_VULKAN "Build experimental Vulkan backend library" OFF)
if(CHIHIRO_BUILD_VULKAN)
    add_subdirectory(src/gpu/vulkan)
endif()
