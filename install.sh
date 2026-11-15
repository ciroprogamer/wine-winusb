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
#
# The original files are kept in <wine lib dir>/.winusb-backup, together with
# a record of what was installed. After a Wine upgrade, just install again.
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

# Path of a module in the Wine installation; empty if Wine doesn't have
# that architecture. Depending on how Wine was packaged, the 32-bit and
# 64-bit modules are not always in the same directory.
target_path() {
    local root="$1" f="$2" d
    for d in "$root/lib/wine" "$root/lib64/wine" "$root/lib32/wine" \
             "$root/lib/x86_64-linux-gnu/wine" "$root/lib/i386-linux-gnu/wine"; do
        [ -d "$d/${f%%/*}" ] && { echo "$d/$f"; return; }
    done
    return 0
}

# The backup lives next to the 64-bit Windows modules.
backup_dir() {
    local root="$1" t
    t="$(target_path "$root" "x86_64-windows/winusb.dll")"
    [ -n "$t" ] || die "could not find the x86_64-windows directory under $root"
    echo "$(dirname "$(dirname "$t")")/.winusb-backup"
}

# file in package -> path relative to a wine lib dir
FILES=(
    "x86_64-windows/winusb.dll"
    "i386-windows/winusb.dll"
    "x86_64-windows/winusb.sys"
    "i386-windows/winusb.sys"
    "x86_64-windows/wineusb.sys"
    "i386-windows/wineusb.sys"
    "x86_64-unix/wineusb.so"
    "i386-unix/wineusb.so"
)

checksum() { sha256sum "$1" | cut -d' ' -f1; }

# Is the file in the Wine installation one that this script put there? A
# Wine upgrade replaces our files with new original ones, which must
# neither be mistaken for ours nor be overwritten with an old backup.
is_ours() {
    local backup="$1" f="$2" target="$3" recorded
    [ -e "$target" ] || return 1
    recorded="$(awk -v f="$f" '$2 == f { print $1 }' "$backup/INSTALLED" 2>/dev/null || true)"
    if [ -n "$recorded" ]; then
        [ "$(checksum "$target")" = "$recorded" ]
        return
    fi
    # Installed by an earlier version of this script, which kept no record:
    # tell by something that only our modules contain.
    case "$f" in
        */winusb.dll)  grep -q "is winusb.sys loaded for this device" "$target" ;;
        */winusb.sys)  grep -q "No DeviceInterfaceGUIDs found" "$target" ;;
        */wineusb.sys) grep -q "IRP %p was already cancelled" "$target" ;;
        */wineusb.so)  grep -q "Cancelling transfer %p for IRP %p" "$target" ;;
        *) return 1 ;;
    esac
}

is_new_file() { grep -qxF "$2" "$1/NEW_FILES" 2>/dev/null; }

forget_new_file() {
    [ -f "$1/NEW_FILES" ] || return 0
    grep -vxF "$2" "$1/NEW_FILES" > "$1/NEW_FILES.tmp" || true
    mv "$1/NEW_FILES.tmp" "$1/NEW_FILES"
}

record_installed() {
    local backup="$1" f="$2" target="$3"
    if [ -f "$backup/INSTALLED" ]; then
        awk -v f="$f" '$2 != f' "$backup/INSTALLED" > "$backup/INSTALLED.tmp"
        mv "$backup/INSTALLED.tmp" "$backup/INSTALLED"
    fi
    echo "$(checksum "$target")  $f" >> "$backup/INSTALLED"
}

do_install() {
    [ "$(id -u)" = 0 ] || die "installing into the Wine directory needs root: sudo $0"
    local root backup version f target skip_i386_drivers=0
    root="$(find_wine_dir)"
    backup="$(backup_dir "$root")"
    version="$("$root/bin/wine" --version 2>/dev/null || true)"
    info "Wine: $root ($version)"
    if [[ "$version" != "$EXPECTED_VERSION"* ]] && [ "${FORCE:-0}" != 1 ]; then
        die "these binaries were built for $EXPECTED_VERSION, found '$version' (FORCE=1 to override)"
    fi

    # wineusb.sys only works with the wineusb.so it was built with. If this
    # Wine has 32-bit Unix libraries and the package has none, the 32-bit
    # drivers stay as they are; they are only used by 32-bit prefixes.
    if [ -n "$(target_path "$root" "i386-unix/wineusb.so")" ] && [ ! -e "$HERE/lib/i386-unix/wineusb.so" ]; then
        skip_i386_drivers=1
        info "warning: lib/i386-unix/wineusb.so is missing (see build.sh); leaving the 32-bit drivers alone."
        info "         32-bit programs work in 64-bit prefixes, but 32-bit prefixes (WINEARCH=win32) get no WinUSB."
    fi

    mkdir -p "$backup"
    for f in "${FILES[@]}"; do
        target="$(target_path "$root" "$f")"
        [ -n "$target" ] || { info "skipping $f (no ${f%%/*} directory)"; continue; }
        [ -e "$HERE/lib/$f" ] || { info "skipping $f (not in this package)"; continue; }
        case "$f" in
            i386-windows/*.sys) [ $skip_i386_drivers = 0 ] || { info "skipping $f"; continue; } ;;
        esac

        if [ ! -e "$target" ]; then
            # Wine has no such file; uninstalling removes ours again
            rm -f "$backup/$f"
            is_new_file "$backup" "$f" || echo "$f" >> "$backup/NEW_FILES"
        elif ! is_ours "$backup" "$f" "$target"; then
            # an original file: from the first installation, or from a
            # Wine upgrade since the last one
            mkdir -p "$backup/$(dirname "$f")"
            cp -a "$target" "$backup/$f"
            forget_new_file "$backup" "$f"
            info "backed up $f"
        fi

        install -m 0644 "$HERE/lib/$f" "$target"
        [[ "$f" == *.so ]] && chmod 0755 "$target"
        record_installed "$backup" "$f" "$target"
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
    local root backup f target
    root="$(find_wine_dir)"
    backup="$(backup_dir "$root")"
    [ -d "$backup" ] || die "no backup found in $backup"
    for f in "${FILES[@]}"; do
        target="$(target_path "$root" "$f")"
        [ -n "$target" ] && [ -e "$target" ] || continue
        if ! is_ours "$backup" "$f" "$target"; then
            info "leaving $f alone: it has been replaced since the installation (Wine upgrade?)"
        elif [ -e "$backup/$f" ]; then
            cp -a "$backup/$f" "$target"; info "restored $f"
        elif is_new_file "$backup" "$f"; then
            rm -f "$target"; info "removed $f"
        fi
    done
    rm -rf "$backup"
    if cmp -s "$HERE/51-android-wine.rules" /etc/udev/rules.d/51-android-wine.rules; then
        rm -f /etc/udev/rules.d/51-android-wine.rules
        udevadm control --reload-rules 2>/dev/null || true
    fi
    info "Original Wine files restored."
}

reg_add() {
    wine reg add "$@" /f >/dev/null 2>&1 || die "could not write $1 to the registry of $WINEPREFIX"
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
    reg_add 'HKCU\Software\Wine\DllOverrides' /v winusb /d builtin
    reg_add 'HKCU\Software\Wine\DllOverrides' /v winusb.sys /d builtin

    # The WinUSB service (created by Android/HiSuite driver INFs) must point
    # at the driver; create it if no INF did.
    reg_add 'HKLM\System\CurrentControlSet\Services\WinUSB' /v ImagePath /t REG_EXPAND_SZ /d 'C:\windows\system32\drivers\winusb.sys'
    reg_add 'HKLM\System\CurrentControlSet\Services\WinUSB' /v Type /t REG_DWORD /d 1
    reg_add 'HKLM\System\CurrentControlSet\Services\WinUSB' /v Start /t REG_DWORD /d 3
    reg_add 'HKLM\System\CurrentControlSet\Services\WinUSB' /v ErrorControl /t REG_DWORD /d 1

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
