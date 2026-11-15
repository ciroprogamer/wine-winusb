#!/bin/bash
# Installs a working WinUSB implementation (winusb.dll + winusb.sys) and a
# patched wineusb.sys into a Wine 11.22 installation, and prepares a prefix.
#
# Usage:
#   sudo ./install.sh                      install into the Wine found in PATH
#   WINEPREFIX=~/.wine ./install.sh --prefix   configure a prefix (run as your user, not root)
#   sudo ./install.sh --uninstall          restore the original Wine files
#
# Environment:
#   WINE_DIR   Wine installation root (default: derived from `wine` in PATH, e.g. /opt/wine-staging)
#   FORCE=1    install even if the Wine version is not 11.22
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPECTED_VERSION="wine-11.22"

die() { echo "error: $*" >&2; exit 1; }
info() { echo ":: $*"; }

find_wine_dir() {
    if [ -n "${WINE_DIR:-}" ]; then echo "$WINE_DIR"; return; fi
    local wine_bin
    wine_bin="$(command -v wine || true)"
    [ -n "$wine_bin" ] || die "wine not found in PATH; set WINE_DIR"
    wine_bin="$(readlink -f "$wine_bin")"
    dirname "$(dirname "$wine_bin")"
}

lib_dir() {
    local root="$1"
    for d in "$root/lib/wine" "$root/lib64/wine" "$root/lib/x86_64-linux-gnu/wine"; do
        [ -d "$d/x86_64-windows" ] && { echo "$d"; return; }
    done
    die "could not find x86_64-windows directory under $root"
}

# file in package -> path relative to the wine lib dir
FILES=(
    "x86_64-windows/winusb.dll"
    "i386-windows/winusb.dll"
    "x86_64-windows/winusb.sys"
    "i386-windows/winusb.sys"
    "x86_64-windows/wineusb.sys"
    "i386-windows/wineusb.sys"
    "x86_64-unix/wineusb.so"
)

do_install() {
    [ "$(id -u)" = 0 ] || die "installing into the Wine directory needs root: sudo $0"
    local root libdir version backup
    root="$(find_wine_dir)"
    libdir="$(lib_dir "$root")"
    version="$("$root/bin/wine" --version 2>/dev/null || true)"
    info "Wine: $root ($version)"
    if [[ "$version" != "$EXPECTED_VERSION"* ]] && [ "${FORCE:-0}" != 1 ]; then
        die "these binaries were built for $EXPECTED_VERSION, found '$version' (FORCE=1 to override)"
    fi

    backup="$libdir/.winusb-backup"
    if [ ! -d "$backup" ]; then
        info "Backing up original files to $backup"
        mkdir -p "$backup"
        for f in "${FILES[@]}"; do
            if [ -e "$libdir/$f" ]; then
                mkdir -p "$backup/$(dirname "$f")"
                cp -a "$libdir/$f" "$backup/$f"
            else
                echo "$f" >> "$backup/NEW_FILES"
            fi
        done
    fi

    for f in "${FILES[@]}"; do
        [ -d "$libdir/$(dirname "$f")" ] || { info "skipping $f (no $(dirname "$f") directory)"; continue; }
        install -m 0644 "$HERE/lib/$f" "$libdir/$f"
        [[ "$f" == *.so ]] && chmod 0755 "$libdir/$f"
        info "installed $f"
    done

    if [ -d /etc/udev/rules.d ]; then
        info "Installing udev rule for Huawei/Android USB devices"
        install -m 0644 "$HERE/51-android-wine.rules" /etc/udev/rules.d/51-android-wine.rules
        udevadm control --reload-rules 2>/dev/null || true
        udevadm trigger --subsystem-match=usb 2>/dev/null || true
    fi

    echo
    info "Done. Now configure your prefix as your normal user (NOT with sudo):"
    echo "    WINEPREFIX=\$HOME/.wine $0 --prefix"
}

do_uninstall() {
    [ "$(id -u)" = 0 ] || die "needs root: sudo $0 --uninstall"
    local root libdir backup
    root="$(find_wine_dir)"
    libdir="$(lib_dir "$root")"
    backup="$libdir/.winusb-backup"
    [ -d "$backup" ] || die "no backup found in $backup"
    for f in "${FILES[@]}"; do
        if [ -e "$backup/$f" ]; then
            cp -a "$backup/$f" "$libdir/$f"; info "restored $f"
        elif grep -qx "$f" "$backup/NEW_FILES" 2>/dev/null; then
            rm -f "$libdir/$f"; info "removed $f"
        fi
    done
    rm -rf "$backup"
    rm -f /etc/udev/rules.d/51-android-wine.rules
    info "Original Wine files restored."
}

do_prefix() {
    [ "$(id -u)" != 0 ] || [ "${ALLOW_ROOT:-0}" = 1 ] || die "run --prefix as your normal user, not root"
    local prefix="${WINEPREFIX:-$HOME/.wine}"
    [ -d "$prefix/drive_c" ] || die "no Wine prefix at $prefix"
    export WINEPREFIX="$prefix"
    info "Configuring prefix $prefix"

    wineserver -k 2>/dev/null || true

    # A native (Microsoft) WinUSB.sys copied by a driver installer can't run
    # under Wine and would shadow the builtin one.
    local f
    for f in "$prefix/drive_c/windows/system32/drivers/winusb.sys" \
             "$prefix/drive_c/windows/system32/drivers/WinUSB.sys"; do
        if [ -f "$f" ] && ! grep -q "Wine builtin DLL" "$f" 2>/dev/null && ! grep -q "Wine placeholder DLL" "$f" 2>/dev/null; then
            info "moving native $(basename "$f") out of the way"
            mv "$f" "$f.native-backup"
        fi
    done

    # Copy the new builtin modules (winusb.sys, winusb.dll, wineusb.sys)
    # into the prefix; Wine loads drivers from system32\drivers.
    info "Updating prefix (wineboot -u)"
    wineboot -u >/dev/null 2>&1 || true
    wineserver -w 2>/dev/null || true

    # Make sure both the driver and the dll are always Wine's builtin ones,
    # even if an application ships its own winusb.dll.
    wine reg add 'HKCU\Software\Wine\DllOverrides' /v winusb /d builtin /f >/dev/null 2>&1
    wine reg add 'HKCU\Software\Wine\DllOverrides' /v winusb.sys /d builtin /f >/dev/null 2>&1

    # The WinUSB service (created by Android/HiSuite driver INFs) must point
    # at the driver; create it if no INF did.
    wine reg add 'HKLM\System\CurrentControlSet\Services\WinUSB' /v ImagePath /t REG_EXPAND_SZ /d 'C:\windows\system32\drivers\winusb.sys' /f >/dev/null 2>&1
    wine reg add 'HKLM\System\CurrentControlSet\Services\WinUSB' /v Type /t REG_DWORD /d 1 /f >/dev/null 2>&1
    wine reg add 'HKLM\System\CurrentControlSet\Services\WinUSB' /v Start /t REG_DWORD /d 3 /f >/dev/null 2>&1
    wine reg add 'HKLM\System\CurrentControlSet\Services\WinUSB' /v ErrorControl /t REG_DWORD /d 1 /f >/dev/null 2>&1


    wineserver -k 2>/dev/null || true

    if [ ! -f "$prefix/drive_c/windows/system32/drivers/winusb.sys" ]; then
        die "winusb.sys was not copied into the prefix; was 'sudo ./install.sh' run for this Wine?"
    fi
    info "Prefix ready. Unplug and replug the phone (or restart Wine) before starting HiSuite."
}

case "${1:-}" in
    ""|--install) do_install ;;
    --uninstall)  do_uninstall ;;
    --prefix)     do_prefix ;;
    *) die "usage: $0 [--install|--prefix|--uninstall]" ;;
esac
