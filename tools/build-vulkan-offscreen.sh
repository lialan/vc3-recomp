#!/bin/sh
# Build only. Running on a device is explicit and never installs a game APK.
set -eu
cd "$(dirname "$0")/.."
ndk_dir="${ANDROID_NDK_HOME:-/Users/lialan/Library/Android/sdk/ndk/27.3.13750724}"
output="${1:-/tmp/vc3-vulkan-offscreen-test}"
shader_build=$(mktemp -d /tmp/vc3-vulkan-shaders.XXXXXX)
trap 'rm -rf "$shader_build"' EXIT
for stage in vert frag; do
    "$ndk_dir/shader-tools/darwin-x86_64/glslc" --target-env=vulkan1.0 \
        "runtime/src/gpu/vulkan/color.$stage" -o "$shader_build/color.$stage.spv"
done
"$ndk_dir/shader-tools/darwin-x86_64/glslc" --target-env=vulkan1.0 -DTEXTURED \
    runtime/src/gpu/vulkan/color.frag -o "$shader_build/color.textured.spv"
"$ndk_dir/shader-tools/darwin-x86_64/glslc" --target-env=vulkan1.0 -DCOMBINER \
    runtime/src/gpu/vulkan/color.frag -o "$shader_build/color.combiner.spv"
"$ndk_dir/shader-tools/darwin-x86_64/glslc" --target-env=vulkan1.0 -DEFFECT \
    runtime/src/gpu/vulkan/color.frag -o "$shader_build/color.effect.spv"
python3 runtime/src/gpu/vulkan/embed_spirv.py "$shader_build/color_spv.h" \
    "$shader_build/color.vert.spv" "$shader_build/color.frag.spv" "$shader_build/color.textured.spv" "$shader_build/color.combiner.spv" "$shader_build/color.effect.spv"
set -- -lEGL -lGLESv3
# Only reachable texture-decoder sections are linked from the executor. Its
# existing nested-comment warning is suppressed; migration sources stay -Werror.
"$ndk_dir/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang" \
    -std=c11 -O1 -ffunction-sections -fdata-sections -Wno-comment \
    -Iexternal/xboxrecomp/src -Iexternal/xboxrecomp/include \
    -Iexternal/xboxrecomp/templates/runtime \
    -c external/xboxrecomp/src/kernel/nv2a_pb_exec.c -o "$shader_build/decoder.o"
"$ndk_dir/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang" \
    -std=c11 -O1 -ffunction-sections -fdata-sections -Iexternal/xboxrecomp/src \
    -c external/xboxrecomp/src/kernel/nv2a_pb_defer.c -o "$shader_build/defer.o"
"$ndk_dir/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang" \
    "$shader_build/defer.o" -Wl,--gc-sections "$shader_build/decoder.o" tools/vulkan-guest-texture-test.c -I"$shader_build" -std=c11 -Wall -Wextra -Werror -Iruntime/src/gpu/vulkan -Iexternal/xboxrecomp/src/kernel \
    external/xboxrecomp/src/kernel/nv2a_combiner.c external/xboxrecomp/src/kernel/nv2a_vsh_interp.c external/xboxrecomp/src/kernel/nv2a_flip_hold.c external/xboxrecomp/src/kernel/nv2a_pb_scan.c tools/vulkan-offscreen-test.c runtime/src/gpu/vulkan/vk_target.c runtime/src/gpu/vulkan/vk_state.c runtime/src/gpu/vulkan/vk_nv2a.c runtime/src/gpu/vulkan/vk_texture.c runtime/src/gpu/vulkan/vk_backend.c runtime/src/gpu/vulkan/vk_depth_guest.c runtime/src/gpu/vulkan/vk_menu.c runtime/src/gpu/vulkan/vk_overlay.c runtime/src/gpu/vulkan/vk_frame_mailbox.c runtime/src/gpu/vulkan/vk_scene.c runtime/src/gpu/vulkan/vk_submission.c runtime/src/xr/quest_menu.c runtime/src/xr/quest_overlay.c tools/vulkan-overlay-parity.c tools/vulkan-frame-mailbox-test.c tools/vulkan-scene-test.c tools/vulkan-submission-test.c -Iruntime/src \
    -pthread -lvulkan "$@" -lm -o "$output"
