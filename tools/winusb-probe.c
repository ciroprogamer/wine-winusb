/*
 * winusb-probe: lists Android/Huawei WinUSB devices visible to Windows
 * programs under Wine, and checks that they can be opened and used.
 *
 * Read-only: in fastboot mode it only sends "getvar" queries.
 */
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <string.h>

/* interface GUID registered by Google's / Huawei's android_winusb.inf */
static const GUID android_guid = {0xf72fe0d4, 0xcbcb, 0x407d, {0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b}};

static void get_string(WINUSB_INTERFACE_HANDLE h, UCHAR index, char *out, int size)
{
    UCHAR buf[256];
    ULONG len, i;

    out[0] = 0;
    if (!index || !WinUsb_GetDescriptor(h, USB_STRING_DESCRIPTOR_TYPE, index, 0x0409, buf, sizeof(buf), &len) || len < 2)
        return;
    for (i = 0; i < (len - 2) / 2 && i < (ULONG)size - 1; i++)
        out[i] = buf[2 + 2 * i] ? buf[2 + 2 * i] : '?';
    out[i] = 0;
}

static void fastboot_getvar(WINUSB_INTERFACE_HANDLE h, UCHAR out, UCHAR in, const char *var)
{
    char cmd[64], reply[65];
    ULONG len, timeout = 3000;

    WinUsb_SetPipePolicy(h, in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    WinUsb_SetPipePolicy(h, out, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    snprintf(cmd, sizeof(cmd), "getvar:%s", var);
    if (!WinUsb_WritePipe(h, out, (UCHAR *)cmd, strlen(cmd), &len, NULL))
    {
        printf("      %-10s write failed, error %lu\n", var, GetLastError());
        return;
    }
    if (!WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, &len, NULL))
    {
        printf("      %-10s read failed, error %lu\n", var, GetLastError());
        return;
    }
    reply[len] = 0;
    printf("      %-10s -> %s\n", var, reply);
}

int main(int argc, char **argv)
{
    SP_DEVICE_INTERFACE_DATA iface = {sizeof(iface)};
    BOOL roundtrip = argc > 1 && !strcmp(argv[1], "--roundtrip");
    int found = 0, opened = 0, worked = 0, tries;
    DWORD index;
    HDEVINFO set;

    printf("winusb-probe: looking for Android/Huawei USB interfaces {F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}\n");

    for (tries = 0; tries < 50; tries++)
    {
        set = SetupDiGetClassDevsA(&android_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &android_guid, 0, &iface))
            break;
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
        set = INVALID_HANDLE_VALUE;
        Sleep(100);
    }
    if (set == INVALID_HANDLE_VALUE)
    {
        printf("\nNo device found.\n"
               "  - is the phone plugged in, and does `lsusb` show it?\n"
               "  - was the WinUSB driver installed for it (HiSuite driver / android_winusb.inf)?\n"
               "  - run with WINEDEBUG=+winusb,+wineusb,+plugplay for details\n");
        return 2;
    }

    for (index = 0; SetupDiEnumDeviceInterfaces(set, NULL, &android_guid, index, &iface); index++)
    {
        char buffer[1024], manufacturer[128], product[128], serial[128];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)buffer;
        USB_DEVICE_DESCRIPTOR dev;
        USB_INTERFACE_DESCRIPTOR desc;
        WINUSB_PIPE_INFORMATION pipe;
        WINUSB_INTERFACE_HANDLE h;
        UCHAR in = 0, out = 0, speed = 0;
        ULONG len, i;
        HANDLE file;

        found++;
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, sizeof(buffer), NULL, NULL))
            continue;
        printf("\n[%lu] %s\n", index, detail->DevicePath);

        file = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            printf("    CreateFile failed, error %lu\n", GetLastError());
            continue;
        }
        if (!WinUsb_Initialize(file, &h))
        {
            printf("    WinUsb_Initialize failed, error %lu (is winusb.sys loaded?)\n", GetLastError());
            CloseHandle(file);
            continue;
        }
        opened++;

        WinUsb_GetDescriptor(h, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, (UCHAR *)&dev, sizeof(dev), &len);
        get_string(h, dev.iManufacturer, manufacturer, sizeof(manufacturer));
        get_string(h, dev.iProduct, product, sizeof(product));
        get_string(h, dev.iSerialNumber, serial, sizeof(serial));
        len = 1;
        WinUsb_QueryDeviceInformation(h, DEVICE_SPEED, &len, &speed);
        printf("    device    %04x:%04x  \"%s\" \"%s\"  serial %s  speed %s\n", dev.idVendor, dev.idProduct,
                manufacturer, product, serial, speed == HighSpeed ? "high" : speed == FullSpeed ? "full" : "?");

        if (WinUsb_QueryInterfaceSettings(h, 0, &desc))
            printf("    interface %u  class %02x/%02x/%02x  %s\n", desc.bInterfaceNumber, desc.bInterfaceClass,
                    desc.bInterfaceSubClass, desc.bInterfaceProtocol,
                    desc.bInterfaceClass == 0xff && desc.bInterfaceSubClass == 0x42
                        ? (desc.bInterfaceProtocol == 0x03 ? "(fastboot)" : "(adb/hdb)") : "");

        for (i = 0; WinUsb_QueryPipe(h, 0, i, &pipe); i++)
        {
            printf("    pipe      %#04x  %s  max packet %u\n", pipe.PipeId,
                    pipe.PipeType == UsbdPipeTypeBulk ? "bulk" : pipe.PipeType == UsbdPipeTypeInterrupt ? "interrupt" : "other",
                    pipe.MaximumPacketSize);
            if (pipe.PipeType == UsbdPipeTypeBulk)
            {
                if (pipe.PipeId & 0x80) in = pipe.PipeId; else out = pipe.PipeId;
            }
        }

        if (in && out && desc.bInterfaceClass == 0xff && desc.bInterfaceSubClass == 0x42 && desc.bInterfaceProtocol == 0x03)
        {
            printf("    fastboot queries:\n");
            fastboot_getvar(h, out, in, "product");
            fastboot_getvar(h, out, in, "version");
            worked++;
        }
        else if (in && out && roundtrip)
        {
            char reply[65];
            if (WinUsb_WritePipe(h, out, (UCHAR *)"getvar:version", 14, &len, NULL)
                    && WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, &len, NULL))
            {
                reply[len] = 0;
                printf("    round trip -> %s\n", reply);
                worked++;
            }
            else printf("    round trip failed, error %lu\n", GetLastError());
        }

        WinUsb_Free(h);
        CloseHandle(file);
    }
    SetupDiDestroyDeviceInfoList(set);

    printf("\n%d interface(s) found, %d opened with WinUSB%s\n", found, opened,
            worked ? ", I/O working" : "");
    return opened ? 0 : 1;
}
