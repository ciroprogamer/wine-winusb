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
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT="${OUT:-$HERE/out}"
export WINEPREFIX="$OUT/prefix" WINEDEBUG="${WINEDEBUG:--all}" DISPLAY=
export FAKEUSB_LOG="$OUT/fakeusb.log"

mkdir -p "$OUT"
echo ":: building emulator and clients"
gcc -Wall -O1 -fPIC -shared -I/usr/include/libusb-1.0 -Wl,-soname,libusb-1.0.so.0 \
    -o "$OUT/libusb-1.0.so.0" "$HERE/fakeusb/fakeusb.c" -lpthread
for bits in 32 64; do
    cc=$([ $bits = 32 ] && echo i686-w64-mingw32-gcc || echo x86_64-w64-mingw32-gcc)
    $cc -O1 -o "$OUT/winusbtest$bits.exe" "$HERE/client/winusbtest.c" -lwinusb -lsetupapi
done
i686-w64-mingw32-gcc -O1 -o "$OUT/failtest.exe" "$HERE/client/failtest.c" -lwinusb -lsetupapi
i686-w64-mingw32-gcc -O1 -o "$OUT/probe.exe" "$ROOT/tools/winusb-probe.c" -lwinusb -lsetupapi

export LD_LIBRARY_PATH="$OUT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ ! -d "$WINEPREFIX" ]; then
    echo ":: creating test prefix"
    wineboot -i >/dev/null 2>&1; wineserver -w
    cp "$HERE/inf/test_winusb.inf" "$WINEPREFIX/drive_c/windows/inf/oem100.inf"
    ALLOW_ROOT=1 "$ROOT/install.sh" --prefix
fi

fail=0
run() {
    local name="$1" status=0; shift
    echo; echo ":: $name"
    env "$@" > "$OUT/last.log" 2>&1 || status=$?
    grep -E "PASS|FAIL|==|->|interface\(s\)" "$OUT/last.log" || true
    { [ $status = 0 ] && ! grep -q "^FAIL" "$OUT/last.log"; } || { echo "   (exit status $status)"; fail=1; }
    wineserver -k 2>/dev/null || true
}

run "fastboot device, 32-bit client" timeout 120 wine "$OUT/winusbtest32.exe"
run "fastboot device, 64-bit client" timeout 120 wine "$OUT/winusbtest64.exe"
run "failed submission does not hang" FAKEUSB_FAIL_SUBMIT=1 timeout 60 wine "$OUT/failtest.exe"
run "composite Huawei device (HDB on MI_01)" FAKEUSB_COMPOSITE=1 timeout 90 wine "$OUT/probe.exe" --roundtrip

echo
[ $fail = 0 ] && echo "ALL TESTS PASSED" || { echo "SOME TESTS FAILED"; exit 1; }
