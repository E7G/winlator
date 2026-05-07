#!/bin/bash
# Build the AHB Direct Compositing Vulkan Layer for ARM64 and x86_64
#
# Prerequisites: Linux NDK at ~/android-ndk-r27c
# Run from: dlls/vulkan_layer/

set -e

NDK="$HOME/android-ndk-r27c"
SYSROOT="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
CC_ARM64="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang"
CC_X86_64="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android26-clang"

CFLAGS="-shared -fPIC -DANDROID -DVK_USE_PLATFORM_ANDROID_KHR -O2 -Wall -Wno-unused-function"
LDFLAGS="-llog -landroid -Wl,--allow-shlib-undefined"

echo "=== Building AHB LD_PRELOAD Interceptor for ARM64 ==="
$CC_ARM64 $CFLAGS --sysroot="$SYSROOT" \
    -o libahb_preload.arm64.so \
    ahb_preload.c ../wineandroid.drv/vulkan_ahb.c \
    $LDFLAGS -ldl

echo "=== Building AHB ICD Wrapper for ARM64 ==="
$CC_ARM64 $CFLAGS --sysroot="$SYSROOT" \
    -o libvulkan_wrapper_ahb.arm64.so \
    ahb_icd_wrapper.c ../wineandroid.drv/vulkan_ahb.c \
    $LDFLAGS -ldl

echo "=== Building AHB LD_PRELOAD Interceptor for x86_64 ==="
$CC_X86_64 $CFLAGS --sysroot="$SYSROOT" \
    -o libahb_preload.x86_64.so \
    ahb_preload.c ../wineandroid.drv/vulkan_ahb.c \
    $LDFLAGS -ldl

echo "=== Building AHB ICD Wrapper for x86_64 ==="
$CC_X86_64 $CFLAGS --sysroot="$SYSROOT" \
    -o libvulkan_wrapper_ahb.x86_64.so \
    ahb_icd_wrapper.c ../wineandroid.drv/vulkan_ahb.c \
    $LDFLAGS -ldl

echo "=== Building AHB Implicit Layer for ARM64 ==="
$CC_ARM64 $CFLAGS --sysroot="$SYSROOT" \
    -o libahb_layer.arm64.so \
    ahb_layer.c ../wineandroid.drv/vulkan_ahb.c \
    $LDFLAGS -ldl

echo "=== Building AHB Implicit Layer for x86_64 ==="
$CC_X86_64 $CFLAGS --sysroot="$SYSROOT" \
    -o libahb_layer.x86_64.so \
    ahb_layer.c ../wineandroid.drv/vulkan_ahb.c \
    $LDFLAGS -ldl

echo ""
echo "=== Build complete ==="
file libahb_preload.arm64.so
file libvulkan_wrapper_ahb.arm64.so
file libahb_layer.arm64.so
file libahb_preload.x86_64.so
file libvulkan_wrapper_ahb.x86_64.so
file libahb_layer.x86_64.so
echo ""
echo "Deploy ICD Wrapper (replaces libvulkan_wrapper.so):"
echo "  # Step 1: Rename the real driver"
echo "  adb shell 'run-as com.winlator.cmod mv files/imagefs/usr/lib/libvulkan_wrapper.so files/imagefs/usr/lib/libvulkan_wrapper_real.so'"
echo "  # Step 2: Push our wrapper as the new libvulkan_wrapper.so"
echo "  adb push libvulkan_wrapper_ahb.arm64.so /data/local/tmp/libvulkan_wrapper.so"
echo "  adb shell 'run-as com.winlator.cmod cp /data/local/tmp/libvulkan_wrapper.so files/imagefs/usr/lib/libvulkan_wrapper.so'"
echo ""
echo "To revert:"
echo "  adb shell 'run-as com.winlator.cmod mv files/imagefs/usr/lib/libvulkan_wrapper_real.so files/imagefs/usr/lib/libvulkan_wrapper.so'"
echo ""
echo "Deploy Implicit Layer:"
echo "  adb push libahb_layer.arm64.so /data/local/tmp/libahb_layer.so"
echo "  adb shell 'run-as com.winlator.cmod cp /data/local/tmp/libahb_layer.so files/imagefs/usr/lib/libahb_layer.so'"
echo "  adb push ahb_layer.json /data/local/tmp/ahb_layer.json"
echo "  adb shell 'run-as com.winlator.cmod mkdir -p files/imagefs/usr/share/vulkan/implicit_layer.d'"
echo "  adb shell 'run-as com.winlator.cmod cp /data/local/tmp/ahb_layer.json files/imagefs/usr/share/vulkan/implicit_layer.d/ahb_layer.json'"
