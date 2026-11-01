#!/bin/sh
# Build only. Use the canonical CMake recipe; never touches a device.
set -eu
cd "$(dirname "$0")/.."
ndk_dir="${ANDROID_NDK_HOME:-/Users/lialan/Library/Android/sdk/ndk/27.3.13750724}"
output="${1:-/tmp/vc3-vulkan-offscreen-test}"
openxr_dir="${OPENXR_DIR:-$(pwd)/../deps/oxr/prefab/modules/headers/include}"
build_dir="${CHIHIRO_VULKAN_TEST_BUILD:-/tmp/vc3-direct-quest-checks}"
cmake -S runtime/src/gpu/vulkan -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk_dir/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DCMAKE_BUILD_TYPE=Release -DCHIHIRO_VULKAN_TESTS=ON -DCHIHIRO_VULKAN_CPU_TESTS=ON \
    -DOPENXR_DIR="$openxr_dir"
cmake --build "$build_dir" --target vulkan_offscreen_test -j "${JOBS:-4}"
if [ "$build_dir/vulkan_offscreen_test" != "$output" ]; then
    cp "$build_dir/vulkan_offscreen_test" "$output"
fi
