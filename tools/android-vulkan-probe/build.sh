#!/bin/sh
# Build/sign an isolated probe APK. Does not install or launch anything.
set -eu
probe_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sdk_dir="${ANDROID_SDK_ROOT:-/Users/lialan/Library/Android/sdk}"
ndk_dir="$sdk_dir/ndk/27.3.13750724"
build_dir="${1:-/tmp/vc3-vulkan-android-probe}"
deps_dir="${DEPS:-/Users/lialan/Downloads/VR/deps}"
java_dir="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
export JAVA_HOME="$java_dir"
cmake -S "$probe_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk_dir/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DOPENXR_DIR="$deps_dir/oxr/prefab/modules/headers/include" \
    -DOPENXR_LIB="$deps_dir/oxr/jni/arm64-v8a/libopenxr_loader.so"
cmake --build "$build_dir" -j4
mkdir -p "$build_dir/apk/lib/arm64-v8a"
cp "$build_dir/libch_vk_probe.so" "$deps_dir/oxr/jni/arm64-v8a/libopenxr_loader.so" "$build_dir/apk/lib/arm64-v8a/"
bt_dir="$sdk_dir/build-tools/34.0.0"
"$bt_dir/aapt" package -f -M "$probe_dir/AndroidManifest.xml" \
    -I "$sdk_dir/platforms/android-34/android.jar" -F "$build_dir/unsigned.apk"
(cd "$build_dir/apk" && zip -q -r "$build_dir/unsigned.apk" lib)
"$bt_dir/zipalign" -f 4 "$build_dir/unsigned.apk" "$build_dir/aligned.apk"
if [ ! -f "$build_dir/probe.keystore" ]; then
    "$java_dir/bin/keytool" -genkeypair -keystore "$build_dir/probe.keystore" \
        -storepass android -keypass android -alias probe -keyalg RSA -validity 3650 \
        -dname "CN=Local Vulkan Probe"
fi
"$bt_dir/apksigner" sign --ks "$build_dir/probe.keystore" --ks-key-alias probe \
    --ks-pass pass:android --out "$build_dir/VulkanRuntimeProbe.apk" "$build_dir/aligned.apk"
"$bt_dir/apksigner" verify "$build_dir/VulkanRuntimeProbe.apk"
