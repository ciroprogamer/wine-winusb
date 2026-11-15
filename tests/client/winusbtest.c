/*
 * WinUSB end-to-end test: finds an Android/fastboot device through its
 * device interface (like AdbWinUsbApi / HiSuite do) and talks fastboot to it.
 */
#include "common.h"

static BOOL fb_command(WINUSB_INTERFACE_HANDLE h, UCHAR out, UCHAR in, const char *cmd, char *reply)
{
    ULONG len;
    if (!WinUsb_WritePipe(h, out, (UCHAR *)cmd, strlen(cmd), &len, NULL) || len != strlen(cmd))
    {
        printf("  write '%s' failed, error %lu, len %lu\n", cmd, GetLastError(), len);
        return FALSE;
    }
    if (!WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, &len, NULL))
    {
        printf("  read after '%s' failed, error %lu\n", cmd, GetLastError());
        return FALSE;
    }
    reply[len] = 0;
    printf("  > %s\n  < %s\n", cmd, reply);
    return TRUE;
}

int main(void)
{
    WINUSB_INTERFACE_HANDLE h;
    USB_INTERFACE_DESCRIPTOR iface_desc;
    USB_DEVICE_DESCRIPTOR dev_desc;
    WINUSB_PIPE_INFORMATION pipe;
    WINUSB_SETUP_PACKET setup;
    UCHAR in = 0, out = 0, speed, buf[256];
    char path[512], reply[65];
    ULONG len, timeout, i;
    OVERLAPPED ov = {0};
    HANDLE file;
    DWORD start;
    BOOL ret;

    printf("== WinUSB end-to-end test (%d-bit) ==\n", (int)sizeof(void *) * 8);

    ret = wait_for_device(path, sizeof(path));
    CHECK(ret, "device interface {F72FE0D4-...} found via SetupDi: %s", ret ? path : "(none)");
    if (!ret) return 1;

    file = open_device(path);
    CHECK(file != INVALID_HANDLE_VALUE, "CreateFile on device path (error %lu)", GetLastError());
    if (file == INVALID_HANDLE_VALUE) return 1;

    ret = WinUsb_Initialize(file, &h);
    CHECK(ret, "WinUsb_Initialize (error %lu)", ret ? 0 : GetLastError());
    if (!ret) return 1;

    ret = WinUsb_GetDescriptor(h, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, (UCHAR *)&dev_desc, sizeof(dev_desc), &len);
    CHECK(ret && len == 18 && dev_desc.idVendor == 0x18d1 && dev_desc.idProduct == 0xd00d,
            "device descriptor %04x:%04x", dev_desc.idVendor, dev_desc.idProduct);

    ret = WinUsb_QueryInterfaceSettings(h, 0, &iface_desc);
    CHECK(ret && iface_desc.bInterfaceClass == 0xff && iface_desc.bInterfaceSubClass == 0x42
            && iface_desc.bInterfaceProtocol == 0x03 && iface_desc.bNumEndpoints == 2,
            "interface class %02x/%02x/%02x, %u endpoints", iface_desc.bInterfaceClass,
            iface_desc.bInterfaceSubClass, iface_desc.bInterfaceProtocol, iface_desc.bNumEndpoints);

    for (i = 0; WinUsb_QueryPipe(h, 0, i, &pipe); i++)
    {
        printf("  pipe %#x type %d maxpacket %u\n", pipe.PipeId, pipe.PipeType, pipe.MaximumPacketSize);
        if (pipe.PipeType == UsbdPipeTypeBulk)
        {
            if (pipe.PipeId & 0x80) in = pipe.PipeId; else out = pipe.PipeId;
        }
    }
    CHECK(in == 0x81 && out == 0x01 && GetLastError() == ERROR_NO_MORE_ITEMS, "bulk pipes found (in %#x, out %#x)", in, out);

    ret = WinUsb_GetDescriptor(h, USB_STRING_DESCRIPTOR_TYPE, dev_desc.iSerialNumber, 0x0409, buf, sizeof(buf), &len);
    if (ret)
    {
        char serial[64];
        for (i = 0; i < (len - 2) / 2 && i < 63; i++) serial[i] = buf[2 + 2 * i];
        serial[i] = 0;
        CHECK(!strcmp(serial, "FAKE0123"), "serial string descriptor via control transfer: %s", serial);
    }
    else CHECK(0, "serial string descriptor (error %lu)", GetLastError());

    len = 1;
    ret = WinUsb_QueryDeviceInformation(h, DEVICE_SPEED, &len, &speed);
    CHECK(ret && speed == HighSpeed, "device speed %u", speed);

    setup.RequestType = 0xc0; setup.Request = 1; setup.Value = 0; setup.Index = 0; setup.Length = 4;
    ret = WinUsb_ControlTransfer(h, setup, buf, 4, &len, NULL);
    CHECK(ret && len == 4 && !memcmp(buf, "WINE", 4), "vendor IN control transfer");

    printf("-- fastboot protocol --\n");
    ret = fb_command(h, out, in, "getvar:version", reply);
    CHECK(ret && !strcmp(reply, "OKAY0.4"), "getvar:version");
    ret = fb_command(h, out, in, "getvar:product", reply);
    CHECK(ret && !strcmp(reply, "OKAYwine-fake-phone"), "getvar:product");

    /* large download, crosses the driver's 1 MiB chunking */
    {
        ULONG size = 3 * 1024 * 1024 + 123;
        UCHAR *data = malloc(size);
        char cmd[32];
        unsigned long sum = 0;

        for (i = 0; i < size; i++) { data[i] = (UCHAR)(i * 7); sum += data[i]; }
        sprintf(cmd, "download:%08lx", size);
        ret = fb_command(h, out, in, cmd, reply);
        CHECK(ret && !strncmp(reply, "DATA", 4), "download command accepted");
        start = GetTickCount();
        ret = WinUsb_WritePipe(h, out, data, size, &len, NULL);
        CHECK(ret && len == size, "wrote %lu/%lu bytes payload in %lu ms (checksum %lu)", len, size, GetTickCount() - start, sum);
        ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, &len, NULL);
        reply[ret ? len : 0] = 0;
        CHECK(ret && !strcmp(reply, "OKAY"), "download completed: %s", reply);
        free(data);
    }

    printf("-- timeouts and cancellation --\n");
    timeout = 500;
    ret = WinUsb_SetPipePolicy(h, in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    CHECK(ret, "set PIPE_TRANSFER_TIMEOUT");
    WinUsb_WritePipe(h, out, (UCHAR *)"silence", 7, &len, NULL);
    start = GetTickCount();
    ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, &len, NULL);
    CHECK(!ret && GetLastError() == ERROR_SEM_TIMEOUT, "read times out (error %lu after %lu ms)", GetLastError(), GetTickCount() - start);
    timeout = 0;
    WinUsb_SetPipePolicy(h, in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

    ret = fb_command(h, out, in, "getvar:serialno", reply);
    CHECK(ret && !strcmp(reply, "OKAYFAKE0123"), "device still works after timed-out read (cancelled transfer cleaned up)");

    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, NULL, &ov);
    CHECK(!ret && GetLastError() == ERROR_IO_PENDING, "overlapped read pending");
    ret = WinUsb_AbortPipe(h, in);
    CHECK(ret, "WinUsb_AbortPipe");
    ret = WinUsb_GetOverlappedResult(h, &ov, &len, TRUE);
    CHECK(!ret && GetLastError() == ERROR_OPERATION_ABORTED, "aborted read completes with ERROR_OPERATION_ABORTED (error %lu)", GetLastError());

    /* overlapped read first, then the command (how adb/fastboot clients pipeline) */
    ResetEvent(ov.hEvent);
    ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, NULL, &ov);
    CHECK(!ret && GetLastError() == ERROR_IO_PENDING, "overlapped read pending (2)");
    ret = WinUsb_WritePipe(h, out, (UCHAR *)"getvar:version", 14, &len, NULL);
    CHECK(ret && len == 14, "write while read is pending");
    ret = WinUsb_GetOverlappedResult(h, &ov, &len, TRUE);
    reply[ret ? len : 0] = 0;
    CHECK(ret && !strcmp(reply, "OKAY0.4"), "overlapped read result: %s", reply);

    ResetEvent(ov.hEvent);
    ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, NULL, &ov);
    ret = CancelIoEx(file, &ov);
    CHECK(ret, "CancelIoEx on pending read");
    ret = GetOverlappedResult(file, &ov, &len, TRUE);
    CHECK(!ret && GetLastError() == ERROR_OPERATION_ABORTED, "CancelIoEx'd read completes (error %lu)", GetLastError());

    ret = fb_command(h, out, in, "getvar:version", reply);
    CHECK(ret && !strcmp(reply, "OKAY0.4"), "device still works after cancellations");

    ret = WinUsb_ResetPipe(h, in);
    CHECK(ret, "WinUsb_ResetPipe");

    ret = WinUsb_ReadPipe(h, 0x82, buf, 4, &len, NULL);
    CHECK(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "invalid pipe rejected");

    ret = WinUsb_Free(h);
    CHECK(ret, "WinUsb_Free");
    CloseHandle(file);

    printf("== %d failure(s) ==\n", failures);
    return failures ? 1 : 0;
}
