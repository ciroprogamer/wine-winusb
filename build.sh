#!/bin/bash
# Rebuilds the binaries in lib/ from source (e.g. for a newer Wine version).
#
#   ./build.sh [wine-tag]        default tag: wine-11.22
#
# Needs: git, gcc, gcc-mingw-w64, flex, bison, autoconf, libusb-1.0-0-dev
#   sudo apt install git gcc gcc-mingw-w64 flex bison autoconf libusb-1.0-0-dev
#
# Wine packages that still have 32-bit Unix libraries (lib/wine/i386-unix,
# like the WineHQ ones) also need a 32-bit wineusb.so, which 32-bit prefixes
# use. It is built if a 32-bit toolchain is installed:
#   sudo dpkg --add-architecture i386 && sudo apt update
#   sudo apt install gcc-multilib libusb-1.0-0-dev:i386
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="${1:-wine-11.22}"
WORK="${WORK:-$HERE/build-$TAG}"
PATCH="$HERE/patches/wine-11.22-winusb.patch"

CONFIGURE_ARGS=(--without-x --without-freetype
    --without-gstreamer --without-alsa --without-pulse --without-cups --without-sane --without-v4l2
    --without-gphoto --without-krb5 --without-netapi --without-opencl --without-pcap --without-dbus
    --without-gnutls --without-sdl --without-capi --without-vulkan --without-wayland --without-ffmpeg
    --without-osmesa --without-opengl --without-oss --without-udev --without-unwind --without-pcsclite)

mkdir -p "$WORK"
cd "$WORK"

# The source tree is usable once the patch is applied and the build files are
# regenerated; the stamp says which patch that was. Start over if an earlier
# run stopped half-way, or if the patch has changed since.
stamp="$(sha256sum "$PATCH" | cut -d' ' -f1)"
if [ "$(cat wine/.winusb-patch 2>/dev/null || true)" != "$stamp" ]; then
    rm -rf wine build build32
    git clone --depth 1 --branch "$TAG" https://gitlab.winehq.org/wine/wine.git wine
    (cd wine && git apply "$PATCH" && git add -N dlls/winusb.sys include/winusbio.h include/wine/winusb_ioctl.h \
        && tools/make_makefiles && autoreconf -f && echo "$stamp" > .winusb-patch)
fi

# 64-bit Unix library and the Windows modules for both architectures
mkdir -p build && cd build
[ -f Makefile ] || ../wine/configure --enable-archs=i386,x86_64 "${CONFIGURE_ARGS[@]}"

grep -q "^USB_LIBS = -lusb" Makefile || { echo "libusb not detected; install libusb-1.0-0-dev" >&2; exit 1; }

make -j"$(nproc)" __tooldeps__
make -j"$(nproc)" \
    dlls/winusb/x86_64-windows/winusb.dll dlls/winusb/i386-windows/winusb.dll \
    dlls/winusb.sys/x86_64-windows/winusb.sys dlls/winusb.sys/i386-windows/winusb.sys \
    dlls/wineusb.sys/x86_64-windows/wineusb.sys dlls/wineusb.sys/i386-windows/wineusb.sys \
    dlls/wineusb.sys/wineusb.so

for arch in x86_64-windows i386-windows; do
    mkdir -p "$HERE/lib/$arch"
    cp dlls/winusb/$arch/winusb.dll dlls/winusb.sys/$arch/winusb.sys dlls/wineusb.sys/$arch/wineusb.sys "$HERE/lib/$arch/"
done
mkdir -p "$HERE/lib/x86_64-unix"
cp dlls/wineusb.sys/wineusb.so "$HERE/lib/x86_64-unix/"

# 32-bit Unix library. Without --enable-archs, configure builds a 32-bit Wine.
build_unix32() {
    echo 'int main(void) { return 0; }' | gcc -m32 -x c - -o /dev/null 2>/dev/null || return 1
    mkdir -p "$WORK/build32" && cd "$WORK/build32" || return 1
    [ -f Makefile ] || ../wine/configure "${CONFIGURE_ARGS[@]}" || return 1
    grep -q "^USB_LIBS = -lusb" Makefile || return 1
    make -j"$(nproc)" __tooldeps__ && make -j"$(nproc)" dlls/wineusb.sys/wineusb.so
}

# A library from an earlier build would not match the new wineusb.sys.
rm -f "$HERE/lib/i386-unix/wineusb.so"
if build_unix32; then
    mkdir -p "$HERE/lib/i386-unix"
    cp "$WORK/build32/dlls/wineusb.sys/wineusb.so" "$HERE/lib/i386-unix/"
else
    echo "note: the 32-bit wineusb.so was not built (needs gcc-multilib and libusb-1.0-0-dev:i386)." >&2
    echo "      On a Wine that has lib/wine/i386-unix, 32-bit prefixes (WINEARCH=win32) won't get WinUSB." >&2
fi

echo "Built for $TAG. Install with: sudo FORCE=1 $HERE/install.sh   (FORCE only needed if the tag isn't 11.22)"
