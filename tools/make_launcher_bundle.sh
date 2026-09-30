#!/bin/sh
# Generates the only native/assets inputs consumed by android/launcher.
# Prerequisites: tools/build_guest.sh and the Android arm64 zbridge/zbproxy build.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/launcher"
ASSETS="$OUT/assets"
ZB="$ASSETS/zb"
JNI="$OUT/jniLibs/arm64-v8a"
ANDROID="$ROOT/build/android-arm64/core"
GUEST="$ROOT/build/guest"

SYSROOT_FILES="
system/bin/linker
system/lib/ld-android.so
system/lib/libc++.so
system/lib/libc.so
system/lib/libdl_android.so
system/lib/libdl.so
system/lib/liblog.so
system/lib/libm.so
system/lib/libstdc++.so
system/lib/libz.so
"

for required in "$ANDROID/libzbridge.so" "$ANDROID/libzbproxy.so" "$GUEST/zbhost" \
        "$GUEST/lib/libzbcompat.so" "$GUEST/lib/libzbjni.so" "$GUEST/lib/libGLESv2.so" \
        "$GUEST/lib/libGLESv1_CM.so" "$GUEST/lib/libEGL.so" "$GUEST/lib/libandroid.so" \
        "$GUEST/lib/libjnigraphics.so"; do
    if [ ! -f "$required" ]; then
        echo "missing $required (build the Android core and guest artifacts first)" >&2
        exit 1
    fi
done
for relative in $SYSROOT_FILES; do
    if [ ! -f "$ROOT/sysroot/$relative" ]; then
        echo "missing $ROOT/sysroot/$relative" >&2
        exit 1
    fi
done

rm -rf "$OUT"
mkdir -p "$ZB/sysroot" "$ZB/guest/lib" "$ZB/host" "$JNI"
for relative in $SYSROOT_FILES; do
    mkdir -p "$ZB/sysroot/$(dirname "$relative")"
    cp "$ROOT/sysroot/$relative" "$ZB/sysroot/$relative"
done
cp "$GUEST/zbhost" "$ZB/guest/zbhost"
cp "$GUEST/lib/libzbcompat.so" "$GUEST/lib/libzbjni.so" \
    "$GUEST/lib/libGLESv2.so" "$GUEST/lib/libGLESv1_CM.so" "$GUEST/lib/libEGL.so" "$GUEST/lib/libandroid.so" \
    "$GUEST/lib/libjnigraphics.so" \
    "$ZB/guest/lib/"
cp "$ANDROID/libzbproxy.so" "$ZB/host/libzbproxy.so"
cp "$ANDROID/libzbridge.so" "$JNI/libzbridge.so"

(cd "$ASSETS" && find zb -type f | LC_ALL=C sort) > "$ASSETS/zb-files.txt"
VERSION=$(cd "$ASSETS" && while IFS= read -r file; do sha256sum "$file"; done < zb-files.txt | sha256sum | cut -d' ' -f1)
printf '%s\n' "$VERSION" > "$ASSETS/zb-version.txt"

"$ROOT/tools/check_launcher_bundle.py" "$OUT"
du -sh "$OUT"
