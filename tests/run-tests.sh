#!/bin/bash
# End-to-end tests with an emulated USB phone (no hardware needed).
#
# A stand-in libusb-1.0.so.0 (tests/fakeusb) emulates a phone in fastboot
# mode (18d1:d00d) or a composite Huawei phone (12d1:1052, HDB on MI_01).
# Windows test clients then go through winusb.dll -> winusb.sys ->
# wineusb.sys -> libusb, exactly like HiSuite would.
#
# Requires the patched modules to be installed (sudo ./install.sh) and:
#   sudo apt install gcc gcc-mingw-w64 libusb-1.0-0-dev
# With gcc-multilib as well, and a Wine that has 32-bit Unix libraries, a
# 32-bit prefix is tested too.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT="${OUT:-$HERE/out}"
export WINEPREFIX="$OUT/prefix" WINEDEBUG="${WINEDEBUG:--all}" DISPLAY=
export FAKEUSB_LOG="$OUT/fakeusb.log"

mkdir -p "$OUT" "$OUT/lib32"
echo ":: building emulator and clients"
fakeusb_flags=(-Wall -O1 -fPIC -shared -I/usr/include/libusb-1.0 -Wl,-soname,libusb-1.0.so.0)
gcc "${fakeusb_flags[@]}" -o "$OUT/libusb-1.0.so.0" "$HERE/fakeusb/fakeusb.c" -lpthread
gcc -m32 "${fakeusb_flags[@]}" -o "$OUT/lib32/libusb-1.0.so.0" "$HERE/fakeusb/fakeusb.c" -lpthread 2>/dev/null \
    || rm -f "$OUT/lib32/libusb-1.0.so.0"
for bits in 32 64; do
    cc=$([ $bits = 32 ] && echo i686-w64-mingw32-gcc || echo x86_64-w64-mingw32-gcc)
    $cc -O1 -o "$OUT/winusbtest$bits.exe" "$HERE/client/winusbtest.c" -lwinusb -lsetupapi
    $cc -O1 -o "$OUT/regress$bits.exe" "$HERE/client/regress.c" -lwinusb -lsetupapi
done
i686-w64-mingw32-gcc -O1 -o "$OUT/failtest.exe" "$HERE/client/failtest.c" -lwinusb -lsetupapi
i686-w64-mingw32-gcc -O1 -o "$OUT/probe.exe" "$ROOT/tools/winusb-probe.c" -lwinusb -lsetupapi

# The emulated device is only there while a test runs.
fakeusb_path="$OUT/lib32:$OUT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Sets up a prefix with the test INF; an optional second argument is its
# WINEARCH. A prefix whose setup did not finish is made again from scratch.
setup_prefix() {
    local prefix="$1"
    [ ! -f "$prefix/.winusb-test-ready" ] || return 0
    echo ":: creating test prefix $prefix"
    rm -rf "$prefix"
    (
        export WINEPREFIX="$prefix"
        [ -z "${2:-}" ] || export WINEARCH="$2"
        wineboot -i >/dev/null 2>&1
        wineserver -w
        cp "$HERE/inf/test_winusb.inf" "$prefix/drive_c/windows/inf/oem100.inf"
        ALLOW_ROOT=1 "$ROOT/install.sh" --prefix
        touch "$prefix/.winusb-test-ready"
    )
}

fail=0
# run <name> [VAR=value ...] <command ...>
run() {
    local name="$1" status=0 envs=(LD_LIBRARY_PATH="$fakeusb_path"); shift
    while [[ "${1:-}" == *=* ]]; do envs+=("$1"); shift; done

    echo; echo ":: $name"
    : > "$FAKEUSB_LOG"
    env "${envs[@]}" "$@" > "$OUT/last.log" 2>&1 || status=$?
    env "${envs[@]}" wineserver -k 2>/dev/null || true
    grep -E "PASS|FAIL|==|->|interface\(s\)" "$OUT/last.log" || true
    if [ $status != 0 ] || grep -q "^FAIL" "$OUT/last.log"; then
        echo "   (exit status $status)"
        fail=1
    fi
    # the USB stack broke a rule of libusb
    if grep "^FAKEUSB ERROR" "$FAKEUSB_LOG"; then fail=1; fi
}

setup_prefix "$WINEPREFIX"

run "fastboot device, 32-bit client" timeout 120 wine "$OUT/winusbtest32.exe"
run "fastboot device, 64-bit client" timeout 120 wine "$OUT/winusbtest64.exe"
run "failed submission does not hang" FAKEUSB_FAIL_SUBMIT=1 timeout 60 wine "$OUT/failtest.exe"
run "timeouts of overlapped and synchronous transfers" timeout 60 wine "$OUT/regress32.exe" timeouts
run "transfer that completes while it times out" FAKEUSB_CANCEL_COMPLETES=1 timeout 60 wine "$OUT/regress64.exe" timeout-race
run "stalls and AUTO_CLEAR_STALL" timeout 60 wine "$OUT/regress32.exe" stall
run "device handle bound to a completion port" timeout 60 wine "$OUT/regress64.exe" iocp
run "partial reads and read policies" timeout 60 wine "$OUT/regress32.exe" partial
run "several reads on one pipe" timeout 60 wine "$OUT/regress64.exe" queue
run "alternate settings" FAKEUSB_ALT=1 timeout 60 wine "$OUT/regress32.exe" altsetting
run "full speed device" FAKEUSB_SPEED=full timeout 60 wine "$OUT/regress64.exe" speed
run "hot unplug" timeout 60 wine "$OUT/regress32.exe" unplug
run "hot unplug, transfers finish after the removal" FAKEUSB_UNPLUG_LATE=1 timeout 60 wine "$OUT/regress64.exe" unplug
run "hot unplug while transfers are being cancelled" FAKEUSB_UNPLUG_LATE=1 timeout 90 wine "$OUT/regress32.exe" unplug-storm
run "composite Huawei device (HDB on MI_01)" FAKEUSB_COMPOSITE=1 timeout 60 wine "$OUT/regress32.exe" composite
run "probe tool on the composite device" FAKEUSB_COMPOSITE=1 timeout 90 wine "$OUT/probe.exe" --roundtrip

# 32-bit prefixes use the 32-bit drivers, and the 32-bit wineusb.so if this
# Wine has 32-bit Unix libraries.
wine_root="$(dirname "$(dirname "$(readlink -f "$(command -v wine)")")")"
unix32=0
for d in "$wine_root"/lib*/wine/i386-unix "$wine_root"/lib/i386-linux-gnu/wine/i386-unix; do
    if [ -d "$d" ]; then unix32=1; fi
done
if [ $unix32 = 0 ]; then
    echo; echo ":: 32-bit prefix: skipped, this Wine has no 32-bit Unix libraries"
elif [ ! -f "$OUT/lib32/libusb-1.0.so.0" ]; then
    echo; echo ":: 32-bit prefix: skipped, the 32-bit emulator could not be built (needs gcc-multilib)"
else
    setup_prefix "$OUT/prefix32" win32
    run "fastboot device in a 32-bit prefix" WINEPREFIX="$OUT/prefix32" WINEARCH=win32 timeout 120 wine "$OUT/winusbtest32.exe"
    run "hot unplug in a 32-bit prefix" WINEPREFIX="$OUT/prefix32" WINEARCH=win32 FAKEUSB_UNPLUG_LATE=1 \
        timeout 60 wine "$OUT/regress32.exe" unplug
fi

echo
[ $fail = 0 ] && echo "ALL TESTS PASSED" || { echo "SOME TESTS FAILED"; exit 1; }
