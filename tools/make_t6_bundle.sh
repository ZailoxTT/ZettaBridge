#!/bin/sh
# Assembles everything the on-device T6 test app needs into build/t6/:
#   build/t6/jniLibs/arm64-v8a/libzbridge.so   -> app/src/main/jniLibs/
#   build/t6/assets/zb/...                     -> app/src/main/assets/
#   build/t6/java/com/zettabridge/core/*.java  -> app/src/main/java/
# Prerequisites: the Android arm64 build (ninja -C build/android-arm64 zbridge),
# tools/build_guest.sh and tools/run_guest_tests.sh (which prepares build/or/).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/t6"
LIB="$ROOT/build/android-arm64/core/libzbridge.so"
GUEST="$ROOT/build/guest"
TESTS="hello_static hello_dynamic threads_dynamic kuser_dynamic signals_dynamic sigio_race_dynamic syscalls_dynamic log_dynamic cxx_dynamic or_dlopen_dynamic"

for required in "$LIB" "$ROOT/sysroot/system/bin/linker" "$ROOT/build/or/libApplicationMain.so"; do
    if [ ! -e "$required" ]; then
        echo "missing $required (see the prerequisites at the top of this script)"
        exit 1
    fi
done

rm -rf "$OUT"
mkdir -p "$OUT/jniLibs/arm64-v8a" "$OUT/assets/zb/guest/lib" "$OUT/assets/zb/or" "$OUT/assets/zb/expected" "$OUT/java"
cp "$LIB" "$OUT/jniLibs/arm64-v8a/"
cp -r "$ROOT/sysroot" "$OUT/assets/zb/sysroot"
for t in $TESTS; do
    cp "$GUEST/$t" "$OUT/assets/zb/guest/"
    cp "$ROOT/guest/tests/expected/$t.out" "$OUT/assets/zb/expected/"
done
cp "$GUEST"/lib/*.so "$OUT/assets/zb/guest/lib/"
cp "$ROOT"/build/or/*.so "$OUT/assets/zb/or/"
cp -r "$ROOT/android/t6/java/." "$OUT/java/"

# The asset list the activity extracts (AssetManager cannot list recursively in one call).
(cd "$OUT/assets" && find zb -type f | sort) > "$OUT/assets/zb-files.txt"

echo "T6 bundle ready in $OUT"
du -sh "$OUT/jniLibs" "$OUT/assets"
