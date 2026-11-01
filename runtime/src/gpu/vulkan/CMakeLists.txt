cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(chihiro_vulkan_validation C)
endif()
get_filename_component(CH_VK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
if(NOT XBOXRECOMP_DIR)
    set(XBOXRECOMP_DIR "${CH_VK_ROOT}/external/xboxrecomp")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(shader_host darwin-x86_64)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(shader_host linux-x86_64)
else()
    set(shader_host windows-x86_64)
endif()
find_program(CHIHIRO_GLSLC glslc HINTS "${ANDROID_NDK}/shader-tools/${shader_host}" REQUIRED)
find_library(CHIHIRO_VULKAN_LIBRARY vulkan REQUIRED)
find_path(CHIHIRO_VULKAN_INCLUDE_DIR vulkan/vulkan.h REQUIRED)
set(fixed_layout "${CMAKE_CURRENT_BINARY_DIR}/fixed_layout.glsl")
set(fixed_asserts "${CMAKE_CURRENT_BINARY_DIR}/fixed_layout_assert.h")
add_custom_command(OUTPUT "${fixed_layout}" "${fixed_asserts}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_fixed_layout.py"
        "${XBOXRECOMP_DIR}/src/kernel/nv2a_backend.h" "${CMAKE_CURRENT_SOURCE_DIR}/vk_fixed.h"
        "${fixed_layout}" "${fixed_asserts}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate_fixed_layout.py" "${CMAKE_CURRENT_SOURCE_DIR}/vk_fixed.h"
        "${XBOXRECOMP_DIR}/src/kernel/nv2a_backend.h" VERBATIM)
set(spirv_outputs)
foreach(stage vert frag textured combiner effect fixed)
    if(stage STREQUAL "fixed")
        set(input fixed.vert)
        set(defines)
    elseif(stage STREQUAL "textured")
        set(input color.frag)
        set(defines -DTEXTURED)
    elseif(stage STREQUAL "combiner")
        set(input color.frag)
        set(defines -DCOMBINER)
    elseif(stage STREQUAL "effect")
        set(input color.frag)
        set(defines -DEFFECT)
    else()
        set(input color.${stage})
        set(defines)
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/color.${stage}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 -I "${CMAKE_CURRENT_BINARY_DIR}" ${defines}
                "${CMAKE_CURRENT_SOURCE_DIR}/${input}" -o "${output}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${input}" "${CMAKE_CURRENT_SOURCE_DIR}/combiner.glsl" "${CMAKE_CURRENT_SOURCE_DIR}/../nv2a_stage_fetch.h" "${CMAKE_CURRENT_SOURCE_DIR}/../nv2a_fixed_shader.h" "${fixed_layout}" VERBATIM)
    list(APPEND spirv_outputs "${output}")
endforeach()
set(header "${CMAKE_CURRENT_BINARY_DIR}/color_spv.h")
add_custom_command(OUTPUT "${header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.py"
        "${header}" ${spirv_outputs}
    DEPENDS ${spirv_outputs} "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.py" VERBATIM)
add_library(chihiro_vulkan STATIC vk_fixed.c vk_target.c vk_texture.c vk_texture_cache.c vk_state.c vk_nv2a.c vk_backend.c vk_depth_guest.c vk_depth_resident.c vk_menu.c vk_overlay.c vk_frame_mailbox.c vk_scene.c vk_submission.c "${header}" "${fixed_asserts}")
target_sources(chihiro_vulkan PRIVATE ../guest_write_watch.c)
target_compile_features(chihiro_vulkan PUBLIC c_std_11)
target_compile_options(chihiro_vulkan PRIVATE -Wall -Wextra -Werror)
target_include_directories(chihiro_vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CHIHIRO_VULKAN_INCLUDE_DIR}"
    "${XBOXRECOMP_DIR}/src/kernel" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(chihiro_vulkan PUBLIC "${CHIHIRO_VULKAN_LIBRARY}" m Threads::Threads)
option(CHIHIRO_VULKAN_CPU_TESTS "Build guest storage tests without a graphics context" OFF)
if(CHIHIRO_VULKAN_CPU_TESTS)
    add_executable(guest_write_watch_test "${CH_VK_ROOT}/tools/guest-write-watch-test.c")
    target_include_directories(guest_write_watch_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_compile_options(guest_write_watch_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(guest_write_watch_test PRIVATE chihiro_vulkan)
    add_executable(vulkan_guest_layout_test "${CH_VK_ROOT}/tools/vulkan-guest-layout-test.c")
    target_compile_options(vulkan_guest_layout_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_guest_layout_test PRIVATE chihiro_vulkan)
endif()
option(CHIHIRO_VULKAN_TESTS "Build standalone offscreen tests; never installs an APK" OFF)
if(CHIHIRO_VULKAN_TESTS)
    add_executable(vulkan_fence_wait_test "${CH_VK_ROOT}/tools/vulkan-fence-wait-test.c"
        "${CH_VK_ROOT}/runtime/src/chihiro/d3d_fence.c")
    target_include_directories(vulkan_fence_wait_test PRIVATE "${CH_VK_ROOT}/runtime/src"
        "${XBOXRECOMP_DIR}/templates/runtime" "${XBOXRECOMP_DIR}/src/platform")
    target_compile_options(vulkan_fence_wait_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(vulkan_fence_wait_test PRIVATE CHIHIRO_RENDERER_VULKAN=1)
    add_executable(vulkan_defer_flush_test "${CH_VK_ROOT}/tools/vulkan-defer-flush-test.c"
        "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_defer.c")
    target_include_directories(vulkan_defer_flush_test PRIVATE "${XBOXRECOMP_DIR}/src/kernel"
        "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/templates/runtime")
    target_link_libraries(vulkan_defer_flush_test PRIVATE Threads::Threads)
    add_library(chihiro_vulkan_test_executor OBJECT
        "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_exec.c"
        "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_defer.c" "${XBOXRECOMP_DIR}/src/kernel/nv2a_vsh_interp.c" "${XBOXRECOMP_DIR}/src/kernel/nv2a_flip_hold.c" "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_scan.c")
    target_include_directories(chihiro_vulkan_test_executor PRIVATE
        "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/templates/runtime")
    target_compile_options(chihiro_vulkan_test_executor PRIVATE -O1 -ffunction-sections -fdata-sections -Wno-comment)
    set(fixed_reference "${CMAKE_CURRENT_BINARY_DIR}/fixed_gl_reference.h")
    add_custom_command(OUTPUT "${fixed_reference}"
        COMMAND "${Python3_EXECUTABLE}" "${CH_VK_ROOT}/tools/generate-fixed-gl-reference.py"
            "${CH_VK_ROOT}/runtime/src/gpu/gl_backend.c" "${fixed_reference}"
        DEPENDS "${CH_VK_ROOT}/tools/generate-fixed-gl-reference.py" "${CH_VK_ROOT}/runtime/src/gpu/gl_backend.c" VERBATIM)
    add_executable(vulkan_offscreen_test "${fixed_reference}" "${CH_VK_ROOT}/tools/vulkan-fixed-parity.c" "${CH_VK_ROOT}/tools/vulkan-offscreen-test.c" "${CH_VK_ROOT}/tools/vulkan-depth24-test.c" "${CH_VK_ROOT}/tools/vulkan-depth-resident-test.c"
        "${CH_VK_ROOT}/tools/vulkan-guest-texture-test.c" "${CH_VK_ROOT}/tools/vulkan-texture-cache-test.c" "${CH_VK_ROOT}/tools/vulkan-resource-reuse-test.c" "${CH_VK_ROOT}/tools/vulkan-command-batch-test.c" "${CH_VK_ROOT}/tools/vulkan-overlay-parity.c" "${CH_VK_ROOT}/tools/vulkan-mipmap-parity.c" "${CH_VK_ROOT}/tools/vulkan-cube-parity.c" "${CH_VK_ROOT}/tools/vulkan-cube-cache-test.c" "${CH_VK_ROOT}/tools/vulkan-frame-mailbox-test.c" "${CH_VK_ROOT}/tools/vulkan-scene-test.c" "${CH_VK_ROOT}/tools/vulkan-submission-test.c" "${CH_VK_ROOT}/runtime/src/xr/quest_overlay.c" "${CH_VK_ROOT}/runtime/src/xr/quest_menu.c" "${XBOXRECOMP_DIR}/src/kernel/nv2a_combiner.c" $<TARGET_OBJECTS:chihiro_vulkan_test_executor>)
    target_compile_options(vulkan_offscreen_test PRIVATE -Wall -Wextra -Werror)
    target_sources(vulkan_offscreen_test PRIVATE "${CH_VK_ROOT}/tools/vulkan-write-watch-test.c")
    target_link_options(vulkan_offscreen_test PRIVATE -Wl,--gc-sections)
    target_include_directories(vulkan_offscreen_test PRIVATE "${CH_VK_ROOT}/runtime/src" "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(vulkan_offscreen_test PRIVATE chihiro_vulkan EGL GLESv3)
endif()

# OpenXR headers are optional for standalone offscreen-only builds. Production
# host enables XR_KHR_vulkan_enable2 before calling this device factory.
if(OPENXR_DIR)
    target_sources(chihiro_vulkan PRIVATE vk_xr.c vk_xr_swapchain.c vk_xr_frame.c vk_xr_quad.c vk_xr_screen.c)
    target_include_directories(chihiro_vulkan PUBLIC "${OPENXR_DIR}")
    if(CHIHIRO_VULKAN_TESTS)
        add_executable(vulkan_xr_device_test "${CH_VK_ROOT}/tools/vulkan-xr-device-test.c")
        target_compile_options(vulkan_xr_device_test PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(vulkan_xr_device_test PRIVATE chihiro_vulkan)
    endif()
endif()

if(CHIHIRO_VULKAN_TESTS)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/combiner_test.spv"
        COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0
            "${CMAKE_CURRENT_SOURCE_DIR}/combiner_test.comp"
            -o "${CMAKE_CURRENT_BINARY_DIR}/combiner_test.spv"
        DEPENDS combiner_test.comp combiner.glsl VERBATIM)
    add_custom_target(vulkan_combiner_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/combiner_test.spv")
    add_executable(vulkan_combiner_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c"
        "${CH_VK_ROOT}/tools/vulkan-combiner-check.c" "${XBOXRECOMP_DIR}/src/kernel/nv2a_combiner.c")
    target_include_directories(vulkan_combiner_check PRIVATE "${XBOXRECOMP_DIR}/src/kernel")
    target_compile_options(vulkan_combiner_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_combiner_check PRIVATE "${CHIHIRO_VULKAN_LIBRARY}" m)
    add_dependencies(vulkan_combiner_check vulkan_combiner_shader)
endif()
