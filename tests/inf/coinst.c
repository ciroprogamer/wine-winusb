#include <windows.h>
#include <setupapi.h>
DWORD CALLBACK CoDeviceInstall(DI_FUNCTION f, HDEVINFO s, PSP_DEVINFO_DATA d, void *ctx) { return NO_ERROR; }
DWORD CALLBACK WdfCoInstaller(DI_FUNCTION f, HDEVINFO s, PSP_DEVINFO_DATA d, void *ctx) { return NO_ERROR; }
