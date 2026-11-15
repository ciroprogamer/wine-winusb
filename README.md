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
| `winusb.dll` (replaces the stub) | Full WinUSB API: `Initialize`, `QueryInterfaceSettings`, `QueryPipe(Ex)`, `GetDescriptor`, `ControlTransfer`, `Read/WritePipe` (sync and overlapped), `Abort/Reset/FlushPipe`, pipe and power policies (including `PIPE_TRANSFER_TIMEOUT`), `GetOverlappedResult`, alternate settings and the descriptor parsers. Built for 32-bit (HiSuite) and 64-bit. |
| `wineusb.sys` + `wineusb.so` (patched) | Per-request cancellation (`CancelIoEx`, timeouts, `AbortPipe` now only aborts the given pipe), generic control transfers, and transfers that libusb refuses now fail cleanly instead of hanging forever. |

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
- pipe timeouts, `AbortPipe`, `CancelIoEx`, and recovery afterwards
- failed transfers returning an error instead of hanging
- composite devices: only the interface the driver is bound to is exposed

On stock Wine 11.22 the same setup fails with
`Failed to load driver L"WinUSB", status 0xc0000142`.

**Status:** tested with emulated devices only. Reports from real phones and
applications are welcome. Please include a `WINEDEBUG=+winusb,+wineusb,+plugplay` log.

## License

LGPL-2.1-or-later, the same as Wine (see `LICENSE`).
