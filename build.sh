#!/bin/bash
# Rebuilds the binaries in lib/ from source (e.g. for a newer Wine version).
#
#   ./build.sh [wine-tag]        default tag: wine-11.22
#
# Needs: git, gcc, gcc-mingw-w64, flex, bison, autoconf, libusb-1.0-0-dev
#   sudo apt install git gcc gcc-mingw-w64 flex bison autoconf libusb-1.0-0-dev
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="${1:-wine-11.22}"
WORK="${WORK:-$HERE/build-$TAG}"

mkdir -p "$WORK"
cd "$WORK"

if [ ! -d wine ]; then
    git clone --depth 1 --branch "$TAG" https://gitlab.winehq.org/wine/wine.git wine
    (cd wine && git apply "$HERE/patches/wine-11.22-winusb.patch" && git add -N dlls/winusb.sys include/winusbio.h include/wine/winusb_ioctl.h \
        && tools/make_makefiles && autoreconf -f)
fi

mkdir -p build && cd build
[ -f Makefile ] || ../wine/configure --enable-archs=i386,x86_64 --without-x --without-freetype \
    --without-gstreamer --without-alsa --without-pulse --without-cups --without-sane --without-v4l2 \
    --without-gphoto --without-krb5 --without-netapi --without-opencl --without-pcap --without-dbus \
    --without-gnutls --without-sdl --without-capi --without-vulkan --without-wayland --without-ffmpeg \
    --without-osmesa --without-opengl --without-oss --without-udev --without-unwind --without-pcsclite

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

echo "Built for $TAG. Install with: sudo FORCE=1 $HERE/install.sh   (FORCE only needed if the tag isn't 11.22)"
