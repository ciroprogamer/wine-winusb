/*
 * WinUSB regression tests: each scenario covers behaviour that was wrong at
 * some point (timeouts of overlapped transfers, hot-unplug, read policies,
 * completion ports, alternate settings, ...).
 *
 *   regress.exe <scenario>
 *
 * Some scenarios need the emulated device in a particular mode; see
 * tests/run-tests.sh.
 */
#include "common.h"

static WINUSB_INTERFACE_HANDLE h;
static HANDLE file;
static UCHAR in = 0x81, out = 0x01;

static BOOL command(const char *cmd)
{
    ULONG len;
    return WinUsb_WritePipe(h, out, (UCHAR *)cmd, strlen(cmd), &len, NULL) && len == strlen(cmd);
}

/* synchronous read of one reply, returned as a string */
static BOOL read_reply(char *reply)
{
    ULONG len = 0;
    BOOL ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, &len, NULL);
    reply[ret ? len : 0] = 0;
    return ret;
}

static void check_device_works(const char *what)
{
    char reply[65];
    BOOL ret = command("getvar:version") && read_reply(reply);
    CHECK(ret && !strcmp(reply, "OKAY0.4"), "%s (%s, error %lu)", what, ret ? reply : "", ret ? 0 : GetLastError());
}

static void set_flag(UCHAR pipe, ULONG policy, UCHAR value)
{
    if (!WinUsb_SetPipePolicy(h, pipe, policy, sizeof(value), &value))
        CHECK(0, "set pipe policy %#lx (error %lu)", policy, GetLastError());
}

static void set_timeout(UCHAR pipe, ULONG timeout)
{
    if (!WinUsb_SetPipePolicy(h, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout))
        CHECK(0, "set PIPE_TRANSFER_TIMEOUT (error %lu)", GetLastError());
}

/* Waits for an overlapped request and returns its error code (0 on success). */
static DWORD wait_result(OVERLAPPED *ov, DWORD timeout, ULONG *len)
{
    ULONG dummy;

    if (WaitForSingleObject(ov->hEvent, timeout) != WAIT_OBJECT_0)
    {
        /* don't leave it pending, the buffers are about to go away */
        CancelIoEx(file, ov);
        WinUsb_GetOverlappedResult(h, ov, &dummy, TRUE);
        return WAIT_TIMEOUT;
    }
    if (!len) len = &dummy;
    return WinUsb_GetOverlappedResult(h, ov, len, FALSE) ? 0 : GetLastError();
}

static void test_timeouts(void)
{
    OVERLAPPED ov = {0};
    char reply[65];
    DWORD start, err;
    ULONG len;
    BOOL ret;

    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    set_timeout(in, 300);
    command("silence");
    start = GetTickCount();
    ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, NULL, &ov);
    CHECK(!ret && GetLastError() == ERROR_IO_PENDING, "overlapped read with a pipe timeout is pending");
    err = wait_result(&ov, 3000, &len);
    CHECK(err == ERROR_SEM_TIMEOUT, "overlapped read times out with ERROR_SEM_TIMEOUT (error %lu after %lu ms)",
            err, GetTickCount() - start);

    start = GetTickCount();
    ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, &len, NULL);
    CHECK(!ret && GetLastError() == ERROR_SEM_TIMEOUT && GetTickCount() - start < 2000,
            "synchronous read times out (error %lu after %lu ms)", GetLastError(), GetTickCount() - start);

    set_timeout(in, 0);
    check_device_works("device works after the timeouts");
    CloseHandle(ov.hEvent);
}

/* needs FAKEUSB_CANCEL_COMPLETES: the transfer completes just as it is cancelled */
static void test_timeout_race(void)
{
    char reply[65];
    ULONG len = 0;
    BOOL ret;

    set_timeout(in, 300);
    command("silence");
    ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, &len, NULL);
    CHECK(ret && len == 4 && !memcmp(reply, "LATE", 4),
            "a read that completes while it times out returns its data (ret %d, len %lu, error %lu)",
            ret, len, ret ? 0 : GetLastError());
}

static void test_stall(void)
{
    OVERLAPPED ov = {0};
    char reply[65];
    DWORD err;
    BOOL ret;

    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    command("stall");
    ret = read_reply(reply);
    CHECK(!ret && GetLastError() == ERROR_GEN_FAILURE, "stalled read fails (error %lu)", GetLastError());
    command("getvar:version");
    ret = read_reply(reply);
    CHECK(!ret, "the pipe stays halted without AUTO_CLEAR_STALL");
    ret = WinUsb_ResetPipe(h, in);
    CHECK(ret, "WinUsb_ResetPipe");
    ret = read_reply(reply);
    CHECK(ret && !strcmp(reply, "OKAY0.4"), "read works after the reset: %s", reply);

    set_flag(in, AUTO_CLEAR_STALL, 1);
    command("stall");
    SetLastError(0xdeadbeef);
    ret = read_reply(reply);
    CHECK(!ret && GetLastError() == ERROR_GEN_FAILURE,
            "stalled read with AUTO_CLEAR_STALL still reports the failure (error %lu)", GetLastError());
    check_device_works("the stall was cleared automatically");

    command("stall");
    ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, NULL, &ov);
    err = ret ? 0 : GetLastError();
    if (err == ERROR_IO_PENDING) err = wait_result(&ov, 3000, NULL);
    CHECK(err == ERROR_GEN_FAILURE, "stalled overlapped read fails (error %lu)", err);
    check_device_works("the stall was cleared automatically for the overlapped read");

    set_flag(in, AUTO_CLEAR_STALL, 0);
    CloseHandle(ov.hEvent);
}

/* The handle is bound to a completion port before WinUSB sees it, like
 * libusb does. */
static void test_iocp(const char *path)
{
    OVERLAPPED ov = {0}, *pov = NULL;
    WINUSB_SETUP_PACKET setup;
    ULONG_PTR key = 0;
    UCHAR buf[64];
    char reply[65];
    DWORD count = 0;
    HANDLE port;
    ULONG len;
    BOOL ret;

    file = open_device(path);
    port = CreateIoCompletionPort(file, NULL, 0x1234, 0);
    CHECK(port != NULL, "device handle bound to a completion port (error %lu)", port ? 0 : GetLastError());
    ret = WinUsb_Initialize(file, &h);
    CHECK(ret, "WinUsb_Initialize on that handle (error %lu)", ret ? 0 : GetLastError());
    if (!ret) return;

    setup.RequestType = 0xc0; setup.Request = 1; setup.Value = 0; setup.Index = 0; setup.Length = 4;
    ret = WinUsb_ControlTransfer(h, setup, buf, 4, &len, NULL);
    CHECK(ret && len == 4, "synchronous control transfer");
    check_device_works("synchronous write and read");

    ret = GetQueuedCompletionStatus(port, &count, &key, &pov, 300);
    CHECK(!ret && !pov, "synchronous calls post nothing to the application's completion port (overlapped %p)", pov);
    while (pov && GetQueuedCompletionStatus(port, &count, &key, &pov, 100)) /* drain */;

    ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply, 64, NULL, &ov);
    CHECK(!ret && GetLastError() == ERROR_IO_PENDING, "overlapped read pending");
    command("getvar:version");
    pov = NULL;
    ret = GetQueuedCompletionStatus(port, &count, &key, &pov, 3000);
    CHECK(ret && pov == &ov && key == 0x1234 && count == 7,
            "the overlapped read is reported through the port (ret %d, %lu bytes)", ret, count);
    CloseHandle(port);
}

static void test_partial_reads(void)
{
    static const char pattern[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKL";
    UCHAR buf[512];
    ULONG len;
    BOOL ret;

    /* the device sends 64 bytes, the application asks for less */
    command("big");
    len = 0;
    ret = WinUsb_ReadPipe(h, in, buf, 16, &len, NULL);
    CHECK(ret && len == 16 && !memcmp(buf, pattern, 16), "read of 16 bytes out of a 64 byte packet (ret %d, len %lu, error %lu)",
            ret, len, ret ? 0 : GetLastError());
    len = 0;
    ret = WinUsb_ReadPipe(h, in, buf, 100, &len, NULL);
    CHECK(ret && len == 48 && !memcmp(buf, pattern + 16, 48), "the next read returns the rest of the packet (ret %d, len %lu)", ret, len);
    check_device_works("device works after partial reads");

    command("big");
    ret = WinUsb_ReadPipe(h, in, buf, 16, &len, NULL);
    ret = ret && WinUsb_FlushPipe(h, in);
    CHECK(ret, "WinUsb_FlushPipe");
    check_device_works("WinUsb_FlushPipe discards the rest of the packet");

    set_flag(in, AUTO_FLUSH, 1);
    command("big");
    len = 0;
    ret = WinUsb_ReadPipe(h, in, buf, 16, &len, NULL);
    CHECK(ret && len == 16, "partial read with AUTO_FLUSH (ret %d, len %lu)", ret, len);
    check_device_works("AUTO_FLUSH discards the rest of the packet");
    set_flag(in, AUTO_FLUSH, 0);

    set_flag(in, ALLOW_PARTIAL_READS, 0);
    command("big");
    ret = WinUsb_ReadPipe(h, in, buf, 16, &len, NULL);
    CHECK(!ret, "too much data fails the read without ALLOW_PARTIAL_READS (error %lu)", ret ? 0 : GetLastError());
    check_device_works("device works after the failed read");
    set_flag(in, ALLOW_PARTIAL_READS, 1);

    command("big");
    command("big");
    len = 0;
    ret = WinUsb_ReadPipe(h, in, buf, 128, &len, NULL);
    CHECK(ret && len == 64, "a short packet completes the read (ret %d, len %lu)", ret, len);
    ret = WinUsb_ReadPipe(h, in, buf, 128, &len, NULL);
    CHECK(ret && len == 64, "second packet (ret %d, len %lu)", ret, len);

    set_flag(in, IGNORE_SHORT_PACKETS, 1);
    command("big");
    command("big");
    len = 0;
    ret = WinUsb_ReadPipe(h, in, buf, 128, &len, NULL);
    CHECK(ret && len == 128 && !memcmp(buf, pattern, 64) && !memcmp(buf + 64, pattern, 64),
            "IGNORE_SHORT_PACKETS reads until the buffer is full (ret %d, len %lu)", ret, len);
    set_flag(in, IGNORE_SHORT_PACKETS, 0);

    set_flag(in, RAW_IO, 1);
    ret = WinUsb_ReadPipe(h, in, buf, 100, &len, NULL);
    CHECK(!ret && GetLastError() == ERROR_INVALID_PARAMETER,
            "RAW_IO rejects a read that is not a multiple of the packet size (ret %d, error %lu)", ret, ret ? 0 : GetLastError());
    command("getvar:version");
    len = 0;
    ret = WinUsb_ReadPipe(h, in, buf, 512, &len, NULL);
    CHECK(ret && len == 7 && !memcmp(buf, "OKAY0.4", 7), "RAW_IO read (ret %d, len %lu)", ret, len);
    set_flag(in, RAW_IO, 0);

    len = 1;
    ret = WinUsb_ReadPipe(h, in, buf, 0, &len, NULL);
    CHECK(ret && !len, "zero length read completes at once (ret %d, len %lu)", ret, len);
}

static void test_read_queue(void)
{
    OVERLAPPED ov[3] = {{0}};
    char reply[3][65];
    DWORD err, start;
    ULONG len;
    BOOL ret;
    int i;

    for (i = 0; i < 3; i++) ov[i].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* replies arrive in the order the reads were issued */
    for (i = 0; i < 2; i++)
    {
        ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply[i], 64, NULL, &ov[i]);
        CHECK(!ret && GetLastError() == ERROR_IO_PENDING, "overlapped read %d pending", i);
    }
    command("getvar:version");
    command("getvar:product");
    err = wait_result(&ov[0], 3000, &len);
    reply[0][err ? 0 : len] = 0;
    CHECK(!err && !strcmp(reply[0], "OKAY0.4"), "first read gets the first reply: %s", reply[0]);
    err = wait_result(&ov[1], 3000, &len);
    reply[1][err ? 0 : len] = 0;
    CHECK(!err && !strcmp(reply[1], "OKAYwine-fake-phone"), "second read gets the second reply: %s", reply[1]);

    /* cancelling a read that waits behind another one */
    for (i = 0; i < 2; i++)
    {
        ResetEvent(ov[i].hEvent);
        WinUsb_ReadPipe(h, in, (UCHAR *)reply[i], 64, NULL, &ov[i]);
    }
    ret = CancelIoEx(file, &ov[1]);
    CHECK(ret, "CancelIoEx on the second read");
    err = wait_result(&ov[1], 3000, NULL);
    CHECK(err == ERROR_OPERATION_ABORTED, "the second read is cancelled (error %lu)", err);
    CHECK(WaitForSingleObject(ov[0].hEvent, 200) == WAIT_TIMEOUT, "the first read is still pending");
    command("getvar:version");
    err = wait_result(&ov[0], 3000, &len);
    reply[0][err ? 0 : len] = 0;
    CHECK(!err && !strcmp(reply[0], "OKAY0.4"), "the first read completes normally: %s", reply[0]);

    /* WinUsb_AbortPipe aborts the waiting reads too */
    for (i = 0; i < 3; i++)
    {
        ResetEvent(ov[i].hEvent);
        WinUsb_ReadPipe(h, in, (UCHAR *)reply[i], 64, NULL, &ov[i]);
    }
    ret = WinUsb_AbortPipe(h, in);
    CHECK(ret, "WinUsb_AbortPipe with three reads pending");
    for (i = 0; i < 3; i++)
    {
        err = wait_result(&ov[i], 3000, NULL);
        CHECK(err == ERROR_OPERATION_ABORTED, "read %d is aborted (error %lu)", i, err);
    }

    /* each read gets its own timeout */
    set_timeout(in, 300);
    command("silence");
    start = GetTickCount();
    for (i = 0; i < 2; i++)
    {
        ResetEvent(ov[i].hEvent);
        WinUsb_ReadPipe(h, in, (UCHAR *)reply[i], 64, NULL, &ov[i]);
    }
    for (i = 0; i < 2; i++)
    {
        err = wait_result(&ov[i], 3000, NULL);
        CHECK(err == ERROR_SEM_TIMEOUT, "queued read %d times out (error %lu after %lu ms)", i, err, GetTickCount() - start);
    }
    set_timeout(in, 0);

    check_device_works("device works afterwards");
    for (i = 0; i < 3; i++) CloseHandle(ov[i].hEvent);
}

/* needs FAKEUSB_ALT */
static void test_alt_setting(void)
{
    USB_INTERFACE_DESCRIPTOR desc;
    WINUSB_PIPE_INFORMATION pipe;
    UCHAR alt = 0xff, alt_in = 0, alt_out = 0;
    ULONG i;
    BOOL ret;

    ret = WinUsb_QueryInterfaceSettings(h, 1, &desc);
    CHECK(ret && desc.bAlternateSetting == 1 && desc.bNumEndpoints == 2, "alternate setting 1 found");
    for (i = 0; WinUsb_QueryPipe(h, 1, i, &pipe); i++)
    {
        if (pipe.PipeId & 0x80) alt_in = pipe.PipeId; else alt_out = pipe.PipeId;
    }
    CHECK(alt_in == 0x82 && alt_out == 0x02, "its pipes are %#x and %#x", alt_in, alt_out);

    ret = WinUsb_SetCurrentAlternateSetting(h, 1);
    CHECK(ret, "WinUsb_SetCurrentAlternateSetting(1) (error %lu)", ret ? 0 : GetLastError());
    ret = WinUsb_GetCurrentAlternateSetting(h, &alt);
    CHECK(ret && alt == 1, "current alternate setting is %u", alt);

    in = alt_in; out = alt_out;
    check_device_works("I/O on the pipes of alternate setting 1");

    ret = WinUsb_SetCurrentAlternateSetting(h, 0);
    CHECK(ret, "WinUsb_SetCurrentAlternateSetting(0) (error %lu)", ret ? 0 : GetLastError());
    in = 0x81; out = 0x01;
    check_device_works("I/O on the pipes of alternate setting 0");

    ret = WinUsb_SetCurrentAlternateSetting(h, 5);
    CHECK(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "invalid alternate setting rejected");
}

/* needs FAKEUSB_SPEED=full: the device says USB 2.00 but runs at full speed */
static void test_speed(void)
{
    UCHAR speed = 0;
    ULONG len = 1;
    BOOL ret;

    ret = WinUsb_QueryDeviceInformation(h, DEVICE_SPEED, &len, &speed);
    CHECK(ret && speed == FullSpeed, "a full speed device is reported as such (speed %u)", speed);
}

static void test_unplug(void)
{
    OVERLAPPED ov[3] = {{0}};
    char reply[3][65], path[512];
    int i, drivers = count_processes("winedevice.exe");
    DWORD err, start;
    ULONG len;
    BOOL ret;

    for (i = 0; i < 3; i++)
    {
        ov[i].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        ret = WinUsb_ReadPipe(h, in, (UCHAR *)reply[i], 64, NULL, &ov[i]);
        CHECK(!ret && GetLastError() == ERROR_IO_PENDING, "overlapped read %d pending", i);
    }

    ret = command("unplug");
    CHECK(ret, "device told to unplug itself");

    for (i = 0; i < 3; i++)
    {
        err = wait_result(&ov[i], 5000, NULL);
        CHECK(err && err != WAIT_TIMEOUT, "pending read %d fails when the device goes away (error %lu)", i, err);
    }

    start = GetTickCount();
    ret = WinUsb_WritePipe(h, out, (UCHAR *)"getvar:version", 14, &len, NULL);
    CHECK(!ret && GetTickCount() - start < 2000, "write to the unplugged device fails at once (error %lu after %lu ms)",
            ret ? 0 : GetLastError(), GetTickCount() - start);

    for (i = 0; i < 50 && find_devices(path, sizeof(path)); i++) Sleep(100);
    CHECK(i < 50, "the device interface disappears");

    ret = WinUsb_Free(h);
    CHECK(ret, "WinUsb_Free");
    CloseHandle(file);
    for (i = 0; i < 3; i++) CloseHandle(ov[i].hEvent);

    /* give transfers that finish late a chance to do damage */
    Sleep(1000);
    CHECK(count_processes("winedevice.exe") == drivers, "the driver host processes are still running (%d of %d)",
            count_processes("winedevice.exe"), drivers);
}

/* needs FAKEUSB_UNPLUG_LATE: the removal is reported while many transfers
 * are still in flight, and the application cancels them at the same time */
enum { STORM_READS = 400, STORM_THREADS = 8 };
static OVERLAPPED storm_ov[STORM_READS];

static DWORD WINAPI storm_cancel_thread(void *arg)
{
    int i;

    for (i = (int)(INT_PTR)arg; i < STORM_READS; i += STORM_THREADS)
        CancelIoEx(file, &storm_ov[i]);
    return 0;
}

static void test_unplug_storm(void)
{
    static UCHAR buffers[STORM_READS][512];
    HANDLE threads[STORM_THREADS];
    char path[512];
    int i, done = 0, drivers = count_processes("winedevice.exe");
    DWORD start;
    BOOL ret;

    set_flag(in, RAW_IO, 1); /* all reads are in flight at the same time */
    for (i = 0; i < STORM_READS; i++)
    {
        storm_ov[i].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        ret = WinUsb_ReadPipe(h, in, buffers[i], sizeof(buffers[i]), NULL, &storm_ov[i]);
        if (ret || GetLastError() != ERROR_IO_PENDING) break;
    }
    CHECK(i == STORM_READS, "%d overlapped reads pending", i);

    ret = command("unplug");
    CHECK(ret, "device told to unplug itself");
    for (i = 0; i < STORM_THREADS; i++)
        threads[i] = CreateThread(NULL, 0, storm_cancel_thread, (void *)(INT_PTR)i, 0, NULL);

    start = GetTickCount();
    for (i = 0; i < STORM_READS; i++)
    {
        DWORD elapsed = GetTickCount() - start;
        if (WaitForSingleObject(storm_ov[i].hEvent, elapsed < 15000 ? 15000 - elapsed : 0) == WAIT_OBJECT_0) done++;
    }
    CHECK(done == STORM_READS, "all reads complete while the device is removed and they are cancelled (%d of %d)",
            done, STORM_READS);
    if (done == STORM_READS) WaitForMultipleObjects(STORM_THREADS, threads, TRUE, 5000);

    for (i = 0; i < 50 && find_devices(path, sizeof(path)); i++) Sleep(100);
    CHECK(i < 50, "the device interface disappears");

    Sleep(1000);
    CHECK(count_processes("winedevice.exe") == drivers, "the driver host processes are still running (%d of %d)",
            count_processes("winedevice.exe"), drivers);
}

/* needs FAKEUSB_COMPOSITE */
static void test_composite(const char *path, int count)
{
    WINUSB_INTERFACE_HANDLE assoc = NULL;
    USB_INTERFACE_DESCRIPTOR desc;
    BOOL ret;

    CHECK(count == 1, "one device interface for the composite device (found %d)", count);
    CHECK(strstr(path, "mi_01") != NULL, "it belongs to interface 1: %s", path);

    ret = WinUsb_QueryInterfaceSettings(h, 0, &desc);
    CHECK(ret && desc.bInterfaceNumber == 1 && desc.bInterfaceClass == 0xff && desc.bInterfaceSubClass == 0x42
            && desc.bInterfaceProtocol == 0x01, "the WinUSB handle is for interface %u, class %02x/%02x/%02x",
            desc.bInterfaceNumber, desc.bInterfaceClass, desc.bInterfaceSubClass, desc.bInterfaceProtocol);

    ret = WinUsb_GetAssociatedInterface(h, 0, &assoc);
    CHECK(!ret && GetLastError() == ERROR_NO_MORE_ITEMS, "the other interface is not exposed (ret %d, error %lu)",
            ret, ret ? 0 : GetLastError());
    if (ret) WinUsb_Free(assoc);

    check_device_works("I/O on the interface");
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "";
    char path[512];
    int count;
    BOOL ret;

    printf("== WinUSB regression test: %s (%d-bit) ==\n", scenario, (int)sizeof(void *) * 8);

    ret = wait_for_device(path, sizeof(path));
    CHECK(ret, "device interface found: %s", ret ? path : "(none)");
    if (!ret) return 1;
    count = find_devices(NULL, 0);

    if (!strcmp(scenario, "iocp"))
    {
        test_iocp(path);
        goto done;
    }

    file = open_device(path);
    ret = file != INVALID_HANDLE_VALUE && WinUsb_Initialize(file, &h);
    CHECK(ret, "device opened (error %lu)", ret ? 0 : GetLastError());
    if (!ret) return 1;

    if (!strcmp(scenario, "timeouts")) test_timeouts();
    else if (!strcmp(scenario, "timeout-race")) test_timeout_race();
    else if (!strcmp(scenario, "stall")) test_stall();
    else if (!strcmp(scenario, "partial")) test_partial_reads();
    else if (!strcmp(scenario, "queue")) test_read_queue();
    else if (!strcmp(scenario, "altsetting")) test_alt_setting();
    else if (!strcmp(scenario, "speed")) test_speed();
    else if (!strcmp(scenario, "unplug")) test_unplug();
    else if (!strcmp(scenario, "unplug-storm")) test_unplug_storm();
    else if (!strcmp(scenario, "composite")) test_composite(path, count);
    else CHECK(0, "unknown scenario '%s'", scenario);

done:
    printf("== %d failure(s) ==\n", failures);
    return failures ? 1 : 0;
}
