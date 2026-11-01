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
set(combiner_program "${CMAKE_CURRENT_BINARY_DIR}/combiner_program.glsl")
set(combiner_asserts "${CMAKE_CURRENT_BINARY_DIR}/combiner_program_assert.h")
add_custom_command(OUTPUT "${combiner_program}" "${combiner_asserts}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_combiner_program.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/vk_combiner_program.h" "${combiner_program}" "${combiner_asserts}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate_combiner_program.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/vk_combiner_program.h" VERBATIM)
set(vertex_program "${CMAKE_CURRENT_BINARY_DIR}/vertex_program.glsl")
add_custom_command(OUTPUT "${vertex_program}" "${CMAKE_CURRENT_BINARY_DIR}/vertex_steps.glsl"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_program.py" "${vertex_program}"
    DEPENDS generate_program.py VERBATIM)
set(spirv_outputs)
set(quant_spirv_outputs)
foreach(stage program vert frag textured combiner effect fixed frag_noat textured_noat combiner_noat effect_noat combiner_specialized combiner_specialized_noat)
    string(REGEX REPLACE "_noat$" "" base_stage "${stage}")
    if(base_stage STREQUAL "program")
        set(input program.vert)
        set(defines -O)
    elseif(base_stage STREQUAL "fixed")
        set(input fixed.vert)
        set(defines)
    elseif(base_stage STREQUAL "textured")
        set(input color.frag)
        set(defines -DTEXTURED)
    elseif(base_stage STREQUAL "combiner")
        set(input color.frag)
        set(defines -DCOMBINER)
    elseif(base_stage STREQUAL "combiner_specialized")
        set(input color.frag)
        set(defines -DCOMBINER -DSPECIALIZED -O)
    elseif(base_stage STREQUAL "effect")
        set(input color.frag)
        set(defines -DEFFECT)
    else()
        set(input color.${base_stage})
        set(defines)
    endif()
    if(stage MATCHES "_noat$")
        list(APPEND defines -DNOAT)
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/color.${stage}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 -I "${CMAKE_CURRENT_BINARY_DIR}" ${defines}
                "${CMAKE_CURRENT_SOURCE_DIR}/${input}" -o "${output}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${input}" "${CMAKE_CURRENT_SOURCE_DIR}/combiner.glsl" "${CMAKE_CURRENT_SOURCE_DIR}/../nv2a_stage_fetch.h" "${CMAKE_CURRENT_SOURCE_DIR}/../nv2a_fixed_shader.h" "${fixed_layout}" "${combiner_program}" "${vertex_program}" program.glsl VERBATIM)
    list(APPEND spirv_outputs "${output}")
    if(input STREQUAL "color.frag")
        set(quant_output "${CMAKE_CURRENT_BINARY_DIR}/color.${stage}_depth.spv")
        add_custom_command(OUTPUT "${quant_output}"
            COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 -I "${CMAKE_CURRENT_BINARY_DIR}" ${defines} -DDEPTH_QUANT_FRAGMENT
                "${CMAKE_CURRENT_SOURCE_DIR}/${input}" -o "${quant_output}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${input}" depth_quant.glsl combiner.glsl
                "${CMAKE_CURRENT_SOURCE_DIR}/../nv2a_stage_fetch.h" "${combiner_program}" VERBATIM)
        list(APPEND quant_spirv_outputs "${quant_output}")
    endif()
endforeach()
list(REMOVE_ITEM spirv_outputs "${CMAKE_CURRENT_BINARY_DIR}/color.program.spv")
list(APPEND spirv_outputs ${quant_spirv_outputs} "${CMAKE_CURRENT_BINARY_DIR}/color.program.spv")
set(header "${CMAKE_CURRENT_BINARY_DIR}/color_spv.h")
add_custom_command(OUTPUT "${header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.py"
        "${header}" ${spirv_outputs}
    DEPENDS ${spirv_outputs} "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.py" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip.spv"
 COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 "${CMAKE_CURRENT_SOURCE_DIR}/depth_roundtrip.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip.spv"
 DEPENDS depth_roundtrip.comp depth_quant.glsl VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_spv.h"
 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary_spirv.py" "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip.spv" "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_spv.h"
 DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip.spv" embed_binary_spirv.py VERBATIM)
foreach(stage program vert frag)
 add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_${stage}.spv"
  COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 "${CMAKE_CURRENT_SOURCE_DIR}/depth_roundtrip.${stage}" -o "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_${stage}.spv"
  DEPENDS "depth_roundtrip.${stage}" depth_quant.glsl VERBATIM)
 add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_${stage}.h"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary_spirv.py" "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_${stage}.spv" "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_${stage}.h" "depth_roundtrip_${stage}_spv"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_${stage}.spv" embed_binary_spirv.py VERBATIM)
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth_input.spv"
 COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 "${CMAKE_CURRENT_SOURCE_DIR}/depth_input.frag" -o "${CMAKE_CURRENT_BINARY_DIR}/depth_input.spv"
 DEPENDS depth_input.frag depth_quant.glsl VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth_input.h"
 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary_spirv.py" "${CMAKE_CURRENT_BINARY_DIR}/depth_input.spv" "${CMAKE_CURRENT_BINARY_DIR}/depth_input.h" "depth_input_spv"
 DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/depth_input.spv" embed_binary_spirv.py VERBATIM)
add_library(chihiro_vulkan STATIC "${CMAKE_CURRENT_BINARY_DIR}/depth_input.h" "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_vert.h" "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_frag.h" vk_depth_quant.c "${CMAKE_CURRENT_BINARY_DIR}/depth_roundtrip_spv.h" vk_fixed.c vk_target.c vk_texture.c vk_texture_cache.c vk_state.c vk_nv2a.c vk_backend.c vk_pipeline_cache_file.c vk_depth_guest.c vk_depth_resident.c vk_menu.c vk_overlay.c vk_frame_mailbox.c vk_scene.c vk_submission.c "${header}" "${fixed_asserts}" "${combiner_asserts}")
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
    # Include the executor in this CPU fixture to compare bulk uploads with
    # its private coordinate sampler. Dead-strip unrelated runtime entrypoints.
    add_executable(nv2a_texture_decode_test "${CH_VK_ROOT}/tools/nv2a-texture-decode-test.c")
    target_include_directories(nv2a_texture_decode_test PRIVATE
        "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/templates/runtime")
    target_compile_options(nv2a_texture_decode_test PRIVATE -O2 -ffunction-sections -fdata-sections -Wno-comment)
    if(APPLE)
        target_link_options(nv2a_texture_decode_test PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(nv2a_texture_decode_test PRIVATE -Wl,--gc-sections)
    endif()
    target_link_libraries(nv2a_texture_decode_test PRIVATE m Threads::Threads)
    add_executable(vulkan_depth_quant_exact_test "${CH_VK_ROOT}/tools/vulkan-depth-quant-exact-test.c")
    target_compile_options(vulkan_depth_quant_exact_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_depth_quant_exact_test PRIVATE m)
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
    target_sources(vulkan_offscreen_test PRIVATE "${CH_VK_ROOT}/tools/vulkan-no-alpha-test.c")
    target_sources(vulkan_offscreen_test PRIVATE "${CH_VK_ROOT}/tools/vulkan-specialized-combiner-test.c")
    target_sources(vulkan_offscreen_test PRIVATE "${CH_VK_ROOT}/tools/vulkan-render-texture-test.c" "${CH_VK_ROOT}/tools/vulkan-texture-watch-test.c")
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

option(CHIHIRO_VULKAN_SHADER_TESTS "Build headless Vulkan shader fixtures without EGL/OpenGL" OFF)
if(CHIHIRO_VULKAN_TESTS OR CHIHIRO_VULKAN_SHADER_TESTS)
    add_executable(vulkan_depth_fragment_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c" "${CH_VK_ROOT}/tools/vulkan-depth-fragment-test.c")
    target_compile_definitions(vulkan_depth_fragment_check PRIVATE CH_VK_DEPTH_FRAGMENT_TEST=1)
    target_compile_options(vulkan_depth_fragment_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_depth_fragment_check PRIVATE chihiro_vulkan)
    add_executable(vulkan_depth24_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c" "${CH_VK_ROOT}/tools/vulkan-depth24-test.c")
    target_compile_definitions(vulkan_depth24_check PRIVATE CH_VK_DEPTH24_TEST=1)
    target_compile_options(vulkan_depth24_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_depth24_check PRIVATE chihiro_vulkan)
    add_executable(vulkan_depth_roundtrip_bench "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c" "${CH_VK_ROOT}/tools/vulkan-depth-roundtrip-bench.c")
    target_compile_definitions(vulkan_depth_roundtrip_bench PRIVATE CH_VK_DEPTH_ROUNDTRIP_BENCH=1)
    target_compile_options(vulkan_depth_roundtrip_bench PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_depth_roundtrip_bench PRIVATE chihiro_vulkan)
    add_executable(vulkan_command_batch_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c" "${CH_VK_ROOT}/tools/vulkan-command-batch-test.c")
    target_compile_definitions(vulkan_command_batch_check PRIVATE CH_VK_BATCH_TEST=1)
    target_compile_options(vulkan_command_batch_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_command_batch_check PRIVATE chihiro_vulkan)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/combiner_test.spv"
        COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0
            "${CMAKE_CURRENT_SOURCE_DIR}/combiner_test.comp"
            -o "${CMAKE_CURRENT_BINARY_DIR}/combiner_test.spv"
        DEPENDS combiner_test.comp combiner.glsl VERBATIM)
    add_custom_target(vulkan_combiner_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/combiner_test.spv")
    add_executable(vulkan_combiner_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c"
        "${CH_VK_ROOT}/tools/vulkan-combiner-check.c" "${XBOXRECOMP_DIR}/src/kernel/nv2a_combiner.c")
    target_include_directories(vulkan_combiner_check PRIVATE "${XBOXRECOMP_DIR}/src/kernel" "${CHIHIRO_VULKAN_INCLUDE_DIR}")
    target_compile_options(vulkan_combiner_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_combiner_check PRIVATE "${CHIHIRO_VULKAN_LIBRARY}" m)
    add_dependencies(vulkan_combiner_check vulkan_combiner_shader)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth_quant_test.spv"
        COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 "${CMAKE_CURRENT_SOURCE_DIR}/depth_quant_test.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/depth_quant_test.spv"
        DEPENDS depth_quant_test.comp depth_quant.glsl VERBATIM)
    add_custom_target(vulkan_depth_quant_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/depth_quant_test.spv")
    add_executable(vulkan_depth_quant_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c" "${CH_VK_ROOT}/tools/vulkan-depth-quant-check.c")
    target_include_directories(vulkan_depth_quant_check PRIVATE "${CHIHIRO_VULKAN_INCLUDE_DIR}")
    target_compile_options(vulkan_depth_quant_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_depth_quant_check PRIVATE "${CHIHIRO_VULKAN_LIBRARY}" m)
    add_dependencies(vulkan_depth_quant_check vulkan_depth_quant_shader)
    add_executable(vulkan_specialized_combiner_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c"
        "${CH_VK_ROOT}/tools/vulkan-specialized-combiner-test.c" "${XBOXRECOMP_DIR}/src/kernel/nv2a_combiner.c")
    target_compile_definitions(vulkan_specialized_combiner_check PRIVATE CH_VK_SPECIALIZATION_TEST=1)
    target_compile_options(vulkan_specialized_combiner_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_specialized_combiner_check PRIVATE chihiro_vulkan)
    add_executable(vulkan_render_texture_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c"
        "${CH_VK_ROOT}/tools/vulkan-render-texture-test.c" "${CH_VK_ROOT}/tools/vulkan-texture-watch-test.c")
    target_compile_definitions(vulkan_render_texture_check PRIVATE CH_VK_RENDER_TEXTURE_TEST=1)
    target_compile_options(vulkan_render_texture_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_render_texture_check PRIVATE chihiro_vulkan)
endif()

if(CHIHIRO_VULKAN_TESTS OR CHIHIRO_VULKAN_SHADER_TESTS)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/program_test.spv"
        COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 -O -I "${CMAKE_CURRENT_BINARY_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/program_test.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/program_test.spv"
        DEPENDS program_test.comp program.glsl "${fixed_layout}" "${vertex_program}" VERBATIM)
    add_custom_target(vulkan_program_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/program_test.spv")
    add_executable(vulkan_program_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c"
        "${CH_VK_ROOT}/tools/vulkan-program-check.c" "${XBOXRECOMP_DIR}/src/kernel/nv2a_vsh_interp.c")
    target_compile_options(vulkan_program_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_program_check PRIVATE chihiro_vulkan)
    add_dependencies(vulkan_program_check vulkan_program_shader)
endif()

if(CHIHIRO_VULKAN_TESTS OR CHIHIRO_VULKAN_SHADER_TESTS)
    add_executable(vulkan_program_render_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c"
        "${CH_VK_ROOT}/tools/vulkan-program-render-test.c")
    target_compile_definitions(vulkan_program_render_check PRIVATE CH_VK_PROGRAM_RENDER_TEST=1)
    target_compile_options(vulkan_program_render_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_program_render_check PRIVATE chihiro_vulkan)
endif()

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth_pack.spv"
    COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/depth_pack.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/depth_pack.spv"
    DEPENDS depth_pack.comp depth_quant.glsl VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth_pack_spv.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary_spirv.py"
        "${CMAKE_CURRENT_BINARY_DIR}/depth_pack.spv" "${CMAKE_CURRENT_BINARY_DIR}/depth_pack_spv.h" depth_pack_spv
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/depth_pack.spv" embed_binary_spirv.py VERBATIM)
target_sources(chihiro_vulkan PRIVATE vk_depth_pack.c "${CMAKE_CURRENT_BINARY_DIR}/depth_pack_spv.h")

if(CHIHIRO_VULKAN_TESTS OR CHIHIRO_VULKAN_SHADER_TESTS)
    add_executable(vulkan_depth_pack_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c"
        "${CH_VK_ROOT}/tools/vulkan-depth-pack-test.c")
    target_compile_definitions(vulkan_depth_pack_check PRIVATE CH_VK_DEPTH_FRAGMENT_TEST=1)
    target_compile_options(vulkan_depth_pack_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_depth_pack_check PRIVATE chihiro_vulkan)
endif()

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/texture_decode.spv"
    COMMAND "${CHIHIRO_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/texture_decode.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/texture_decode.spv"
    DEPENDS texture_decode.comp VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/texture_decode_spv.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary_spirv.py"
        "${CMAKE_CURRENT_BINARY_DIR}/texture_decode.spv" "${CMAKE_CURRENT_BINARY_DIR}/texture_decode_spv.h" texture_decode_spv
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/texture_decode.spv" embed_binary_spirv.py VERBATIM)
target_sources(chihiro_vulkan PRIVATE vk_texture_decode.c "${CMAKE_CURRENT_BINARY_DIR}/texture_decode_spv.h")

if(CHIHIRO_VULKAN_TESTS OR CHIHIRO_VULKAN_SHADER_TESTS)
    add_executable(vulkan_texture_decode_check "${CH_VK_ROOT}/tools/vulkan-combiner-driver.c"
        "${CH_VK_ROOT}/tools/vulkan-texture-decode-test.c")
    target_compile_definitions(vulkan_texture_decode_check PRIVATE CH_VK_BATCH_TEST=1)
    target_compile_options(vulkan_texture_decode_check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(vulkan_texture_decode_check PRIVATE chihiro_vulkan)
endif()
