# wine-winusb

A working **WinUSB** stack for Wine 11.22, so Windows programs that use WinUSB (Huawei HiSuite, Android `fastboot`/`adb` tools, and others) can talk to USB devices under Wine.

## The problem

Driver packages like HiSuite's (`hw_goadb.inf`) or Google's `android_winusb.inf` bind the device to Microsoft's **WinUSB**.
Stock Wine has no WinUSB driver, and every function in its `winusb.dll`
is a stub. The result:

    err:ntoskrnl:ZwLoadDriver failed to create driver ...\Services\WinUSB: c0000142
    err:plugplay:load_function_driver Failed to load driver L"WinUSB", status 0xc0000142.

Wine's `wineusb.sys` sees the phone through libusb, but nothing above it
lets a Windows program talk to it.

## What this package adds

| File | What it is |
|---|---|
| `winusb.sys` (new) | WinUSB function driver. It registers the device interface from `DeviceInterfaceGUIDs`, as native WinUSB does, so `SetupDiGetClassDevs` finds the phone, and it turns WinUSB requests into USB transfers. It handles both single-interface devices (fastboot) and the HDB/ADB interface of composite devices (`&MI_xx`, Huawei normal mode). |
| `winusb.dll` (replaces the stub) | Full WinUSB API: `Initialize`, `QueryInterfaceSettings`, `QueryPipe(Ex)`, `GetDescriptor`, `ControlTransfer`, `Read/WritePipe` (sync and overlapped), `Abort/Reset/FlushPipe`, pipe and power policies, `GetOverlappedResult`, alternate settings and the descriptor parsers. Built for 32-bit (HiSuite) and 64-bit. |
| `wineusb.sys` + `wineusb.so` (patched) | Per-request cancellation (`CancelIoEx`, timeouts, `AbortPipe` now only aborts the given pipe), generic control transfers, alternate settings, the real device speed, and transfers that libusb refuses now fail cleanly instead of hanging forever. Unplugging a device with transfers in flight no longer completes them twice. |

The pipe policies are applied by the driver, so they work the same for synchronous and overlapped calls:

- `PIPE_TRANSFER_TIMEOUT` ends a transfer with `ERROR_SEM_TIMEOUT`, also when the caller waits on an `OVERLAPPED`.
- Reads on a pipe are served one at a time and in order, unless `RAW_IO` is set.
- A read may ask for less than the device sends: the rest of the packet is kept for the next read (`ALLOW_PARTIAL_READS`, the default), dropped (`AUTO_FLUSH`, `WinUsb_FlushPipe`), or the read fails (`ALLOW_PARTIAL_READS` off).
- `IGNORE_SHORT_PACKETS`, `SHORT_PACKET_TERMINATE` and `AUTO_CLEAR_STALL` behave as documented for WinUSB.

Sources are in `patches/wine-11.22-winusb.patch` (applies to the `wine-11.22` tag). Prebuilt binaries are in `lib/`.

## Install

The binaries are for **WineHQ wine-staging 11.22 on Ubuntu 24.04**.

```bash
git clone <this repo> wine-winusb
cd wine-winusb

# 1. Install into Wine (backs up the original files first)
sudo ./install.sh

# 2. Configure your prefix. Run this as YOUR user, not with sudo
WINEPREFIX=$HOME/.wine ./install.sh --prefix
```

Then **unplug and replug the phone**.

Step 1 also installs a udev rule so Wine can open Huawei (`12d1`) and Android (`18d1`) devices.
It gives access to the user logged in at the screen, and to members of the `plugdev` group
(for ssh sessions and the like); other accounts on the machine get none.

The original Wine files are kept in a backup directory next to the Wine modules, and `--uninstall` puts them back.

## Check it before starting HiSuite

```bash
WINEPREFIX=$HOME/.wine wine ./tools/winusb-probe.exe
```

In fastboot mode, the output should look like this:

    [0] \\?\usb#vid_18d1&pid_d00d#...#{f72fe0d4-cbcb-407d-8814-9ed673d0dd6b}
        device    18d1:d00d  ...
        interface 0  class ff/42/03  (fastboot)
        fastboot queries:
          product    -> OKAY...

The probe only reads data. In fastboot mode it sends nothing but `getvar` queries.

Then start your application, e.g. HiSuite:

```bash
env WINEPREFIX="$HOME/.wine" wine "C:\\users\\$USER\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\HiSuite\\HiSuite.lnk"
```

## Troubleshooting

- **Logs:** run with `WINEDEBUG=+winusb,+wineusb,+plugplay`.
  `Failed to load driver L"WinUSB", status 0xc0000142` means step 2 wasn't run for this prefix.
- **Native adb server:** `adb kill-server`. A running Linux adb server claims the ADB interface.
- **Phone in normal mode:** enable *Settings → Security → "Allow HiSuite to use HDB"* and choose *File transfer* as the USB mode.
  The phone then shows up as `12d1:xxxx` with several interfaces. HiSuite's INF has entries for many Huawei PIDs; check `lsusb` against it if nothing appears.
- **"Access denied (insufficient permissions)" in the log:** for hubs, webcams and similar this is normal and harmless. For the phone itself, it means the udev rule isn't active yet: replug the phone.

## Uninstall

```bash
sudo ./install.sh --uninstall
```

## After a Wine upgrade

These binaries match Wine 11.22. A Wine package upgrade overwrites
`winusb.dll`/`wineusb.sys`. For a different version, rebuild:

```bash
sudo apt install git gcc gcc-mingw-w64 flex bison autoconf libusb-1.0-0-dev
./build.sh wine-<version>      # e.g. wine-11.23
sudo FORCE=1 ./install.sh
WINEPREFIX=$HOME/.wine ./install.sh --prefix
```

Installing again after an upgrade is safe: the installer notices that Wine's own files
have changed and takes a new backup of them, so `--uninstall` never brings back files of the old version.

WineHQ's packages also have 32-bit Unix libraries (`lib/wine/i386-unix`), which 32-bit prefixes
(`WINEARCH=win32`) use. `build.sh` builds the 32-bit `wineusb.so` for them if a 32-bit toolchain is installed:

```bash
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install gcc-multilib libusb-1.0-0-dev:i386
```

Without it, the installer leaves the 32-bit drivers alone. 32-bit programs such as HiSuite
still work in the usual 64-bit prefix; only 32-bit prefixes get no WinUSB.

## Tests

`tests/run-tests.sh` runs end-to-end tests without hardware. A stand-in
`libusb-1.0.so.0` emulates a phone in fastboot mode (`18d1:d00d`) or a
composite Huawei phone (`12d1:1052`, HDB on `MI_01`). Windows test clients
(32- and 64-bit) then go through `winusb.dll` → `winusb.sys` → `wineusb.sys` → libusb.

```bash
sudo apt install gcc gcc-mingw-w64 libusb-1.0-0-dev
sudo ./install.sh
./tests/run-tests.sh
```

The tests cover:

- device discovery through `SetupDi*` and the interface GUID
- descriptors, string descriptors and vendor control transfers
- fastboot commands and a 3 MiB download that is checked byte for byte
- pipe timeouts for synchronous and overlapped transfers, `AbortPipe`, `CancelIoEx`, and recovery afterwards
- failed transfers returning an error, and no data, instead of hanging
- stalls, with and without `AUTO_CLEAR_STALL`
- reads smaller than a packet, and the read policies
- several reads pending on one pipe
- a device handle that is bound to an I/O completion port
- alternate settings and the device speed
- unplugging the device while transfers are in flight, and while they are being cancelled
- composite devices: only the interface the driver is bound to is exposed
- a 32-bit prefix, if Wine has 32-bit Unix libraries and `gcc-multilib` is installed

The emulated libusb also checks that the USB stack follows libusb's rules,
for example that no transfer is left behind when a device is closed.

On stock Wine 11.22 the same setup fails with
`Failed to load driver L"WinUSB", status 0xc0000142`.

**Status:** tested with emulated devices only. Reports from real phones and
applications are welcome. Please include a `WINEDEBUG=+winusb,+wineusb,+plugplay` log.

## License

LGPL-2.1-or-later, the same as Wine (see `LICENSE`).
