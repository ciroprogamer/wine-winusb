/*
 * Helpers shared by the WinUSB test clients.
 */
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

/* interface GUID registered by Google's / Huawei's android_winusb.inf */
static const GUID android_guid = {0xf72fe0d4, 0xcbcb, 0x407d, {0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b}};

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("PASS: " __VA_ARGS__); else { failures++; printf("FAIL: " __VA_ARGS__); } printf("\n"); } while (0)

/* Returns the number of present device interfaces, and the path of the first one. */
static inline int find_devices(char *path, DWORD size)
{
    SP_DEVICE_INTERFACE_DATA iface = {sizeof(iface)};
    SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail;
    HDEVINFO set;
    char buffer[1024];
    int count = 0;

    set = SetupDiGetClassDevsA(&android_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;

    while (SetupDiEnumDeviceInterfaces(set, NULL, &android_guid, count, &iface))
    {
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)buffer;
        detail->cbSize = sizeof(*detail);
        if (!count && path && SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, sizeof(buffer), NULL, NULL))
            lstrcpynA(path, detail->DevicePath, size);
        count++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

/* The device shows up a moment after the USB driver has started. */
static inline BOOL wait_for_device(char *path, DWORD size)
{
    int i;

    for (i = 0; i < 100; i++)
    {
        if (find_devices(path, size)) return TRUE;
        Sleep(100);
    }
    return FALSE;
}

static inline HANDLE open_device(const char *path)
{
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
}

/* Number of running processes with the given image name; used to notice a
 * crash of winedevice.exe, the process that hosts the drivers. */
static inline int count_processes(const char *name)
{
    PROCESSENTRY32 entry = {sizeof(entry)};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    int count = 0;

    if (snapshot == INVALID_HANDLE_VALUE) return -1;
    if (Process32First(snapshot, &entry))
    {
        do
        {
            if (!lstrcmpiA(entry.szExeFile, name)) count++;
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}
