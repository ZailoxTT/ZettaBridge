#!/bin/sh
# Runs arm32 guest test programs under zbrun and checks exit code and stdout.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZBRUN=${ZBRUN:-$ROOT/build/host/cli/zbrun/zbrun}
ZBFIX=${ZBFIX:-$ROOT/build/host/cli/zbfix/zbfix}
SYSROOT=${ZB_SYSROOT:-$ROOT/sysroot}
GUEST="$ROOT/build/guest"
EXPECTED="$ROOT/guest/tests/expected"
APK="$ROOT/orange-roulette-1-0-0.apk"
OR_LIBS="$ROOT/build/or"
failures=0

# run_case <name> <expected exit> [args...]; CASE_ARGS holds extra zbrun options (e.g. --env).
run_case() {
    name=$1
    expected_exit=$2
    shift 2
    out=$("$ZBRUN" --sysroot "$SYSROOT" $CASE_ARGS "$GUEST/$name" "$@" 2>"$GUEST/$name.stderr")
    code=$?
    if [ "$code" != "$expected_exit" ]; then
        echo "FAIL $name: exit $code, expected $expected_exit (stderr in build/guest/$name.stderr)"
        failures=$((failures + 1))
        return
    fi
    if ! printf '%s\n' "$out" | diff -u "$EXPECTED/$name.out" - >"$GUEST/$name.diff"; then
        echo "FAIL $name: stdout differs (see build/guest/$name.diff)"
        failures=$((failures + 1))
        return
    fi
    echo "PASS $name"
}

CASE_ARGS=""
run_case hello_static 7 world
run_case tbh_static 0
run_case asimd_narrow_static 0
run_case hello_dynamic 3 "$GUEST/zb_io.tmp"
run_case threads_dynamic 0
run_case kuser_dynamic 0
run_case signals_dynamic 134
run_case sigwake_dynamic 0
run_case cp15_barrier_dynamic 0
run_case sigrestart_dynamic 0
run_case sigio_race_dynamic 0 "$GUEST/zb_sigio_race.tmp"
run_case mremap_dynamic 0
run_case syscalls_dynamic 0 "$GUEST/zb_syscalls_tmp"
run_case log_dynamic 0
CASE_ARGS="--env LD_LIBRARY_PATH=$GUEST/lib"
run_case cxx_dynamic 0
CASE_ARGS=""

if [ -f "$APK" ]; then
    mkdir -p "$OR_LIBS"
    unzip -ojq "$APK" 'lib/armeabi/*' -d "$OR_LIBS"
    "$ZBFIX" "$OR_LIBS"/*.so >"$GUEST/or_fixups.log"
    CASE_ARGS="--env LD_LIBRARY_PATH=$GUEST/lib"
    run_case or_dlopen_dynamic 0 "$OR_LIBS"
    CASE_ARGS=""
else
    echo "SKIP or_dlopen_dynamic: $APK not found"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures guest test(s) failed"
    exit 1
fi
echo "all guest tests passed"
