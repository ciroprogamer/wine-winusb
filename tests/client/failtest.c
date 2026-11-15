#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
static const GUID g = {0xf72fe0d4,0xcbcb,0x407d,{0x88,0x14,0x9e,0xd6,0x73,0xd0,0xdd,0x6b}};
int main(void)
{
    SP_DEVICE_INTERFACE_DATA i = {sizeof(i)}; char b[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_A *d = (void *)b;
    HDEVINFO s; HANDLE f; WINUSB_INTERFACE_HANDLE h; ULONG len; char r[65]; DWORD t; BOOL ok; int n;
    for (n = 0; n < 100; n++) { s = SetupDiGetClassDevsA(&g, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
        if (SetupDiEnumDeviceInterfaces(s, NULL, &g, 0, &i)) break; SetupDiDestroyDeviceInfoList(s); Sleep(100); }
    d->cbSize = sizeof(*d); SetupDiGetDeviceInterfaceDetailA(s, &i, d, sizeof(b), NULL, NULL);
    f = CreateFileA(d->DevicePath, GENERIC_READ|GENERIC_WRITE, 3, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (!WinUsb_Initialize(f, &h)) { printf("FAIL: init %lu\n", GetLastError()); return 1; }
    t = GetTickCount(); len = 0xdead;
    ok = WinUsb_WritePipe(h, 0x01, (UCHAR *)"fail", 4, &len, NULL);
    printf("%s: failed submission returns error instead of hanging (ret %d, error %lu, %lu ms)\n",
           !ok && GetTickCount() - t < 2000 ? "PASS" : "FAIL", ok, GetLastError(), GetTickCount() - t);
    printf("%s: no bytes are reported as transferred (%lu)\n", !len ? "PASS" : "FAIL", len);
    WinUsb_WritePipe(h, 0x01, (UCHAR *)"getvar:version", 14, &len, NULL);
    ok = WinUsb_ReadPipe(h, 0x81, (UCHAR *)r, 64, &len, NULL); r[ok ? len : 0] = 0;
    printf("%s: device usable afterwards (%s)\n", ok && !strcmp(r, "OKAY0.4") ? "PASS" : "FAIL", r);
    return 0;
}
