/*
 * Fake libusb-1.0 emulating an Android phone in fastboot mode (18d1:d00d).
 * Used to test Wine's USB stack end to end without real hardware.
 *
 * Implements the subset of libusb used by Wine's wineusb.so.
 *
 * Environment:
 *   FAKEUSB_LOG              log file
 *   FAKEUSB_COMPOSITE        composite Huawei phone (12d1:1052), HDB on MI_01
 *   FAKEUSB_ALT              interface 0 has a second alternate setting with
 *                            endpoints 0x82/0x02 instead of 0x81/0x01
 *   FAKEUSB_SPEED            low, full, high (default) or super
 *   FAKEUSB_FAIL_SUBMIT      submitting the 4 byte write "fail" fails
 *   FAKEUSB_UNPLUG_LATE      on "unplug", report the removal first and
 *                            finish the pending transfers 300 ms later
 *   FAKEUSB_CANCEL_COMPLETES a cancelled read completes with data instead,
 *                            as if it had finished just before the cancel
 *
 * Lines starting with "FAKEUSB ERROR" in the log mean that the USB stack
 * broke a libusb rule; the test script treats them as failures.
 */
#include <libusb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EP_IN  0x81
#define EP_OUT 0x01
#define MAX_PACKET 512

struct libusb_device { int dummy; };
struct libusb_device_handle { struct libusb_device *dev; };

static struct libusb_device fake_dev;
static struct libusb_device_handle fake_handle = { &fake_dev };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int interrupted;

static libusb_hotplug_callback_fn hotplug_cb;
static void *hotplug_data;

static int unplug_requested;    /* the "unplug" command was received */
static int gone;                /* the device has been unplugged */
static int late_completion;     /* pending transfers still have to fail */
static struct timespec late_time;

static int claimed;             /* bitmask of interfaces claimed through libusb */
static int host_alt;            /* alternate setting the host side knows about */
static int halted;              /* the IN endpoint is stalled */

static FILE *logf;
static void flog(const char *fmt, ...)
{
    va_list args;
    if (!logf)
    {
        const char *path = getenv("FAKEUSB_LOG");
        logf = fopen(path ? path : "/tmp/fakeusb.log", "a");
        if (!logf) return;
        setvbuf(logf, NULL, _IOLBF, 0);
    }
    va_start(args, fmt);
    vfprintf(logf, fmt, args);
    va_end(args);
}

/* completed transfers waiting for their callback */
#define QSIZE 1024
static struct libusb_transfer *done_queue[QSIZE];
static int done_count;

/* pending IN transfers */
static struct libusb_transfer *pending_in[QSIZE];
static int pending_count;

/* responses from the device */
struct response { unsigned char data[64]; int len; };
static struct response responses[QSIZE];
static int response_count;

static unsigned long download_left, download_total, download_sum;
static unsigned long total_bytes_out;

static void queue_done(struct libusb_transfer *t, enum libusb_transfer_status status, int actual)
{
    t->status = status;
    t->actual_length = actual;
    done_queue[done_count++] = t;
    pthread_cond_broadcast(&cond);
}

static void add_response(const char *fmt, ...)
{
    va_list args;
    struct response *r = &responses[response_count++];
    va_start(args, fmt);
    r->len = vsnprintf((char *)r->data, sizeof(r->data), fmt, args);
    va_end(args);
    flog("device response: %.*s\n", r->len, r->data);
}

static void match_in(void)
{
    while (pending_count && response_count)
    {
        struct libusb_transfer *t = pending_in[0];
        struct response r = responses[0];

        memmove(pending_in, pending_in + 1, --pending_count * sizeof(*pending_in));
        memmove(responses, responses + 1, --response_count * sizeof(*responses));
        if (r.len > t->length)
        {
            /* like a real host controller: the packet does not fit in the
             * transfer, which fails, and the data is lost */
            flog("device: %d byte packet overflows %d byte transfer\n", r.len, t->length);
            queue_done(t, LIBUSB_TRANSFER_OVERFLOW, 0);
            continue;
        }
        memcpy(t->buffer, r.data, r.len);
        queue_done(t, LIBUSB_TRANSFER_COMPLETED, r.len);
    }
}

static void handle_command(const unsigned char *data, int len)
{
    char cmd[65];

    if (len > 64) len = 64;
    memcpy(cmd, data, len);
    cmd[len] = 0;
    flog("device command: %s\n", cmd);

    if (!strcmp(cmd, "getvar:version")) add_response("OKAY0.4");
    else if (!strcmp(cmd, "getvar:product")) add_response("OKAYwine-fake-phone");
    else if (!strcmp(cmd, "getvar:serialno")) add_response("OKAYFAKE0123");
    else if (!strcmp(cmd, "getvar:max-download-size")) add_response("OKAY0x10000000");
    else if (!strncmp(cmd, "download:", 9))
    {
        download_total = download_left = strtoul(cmd + 9, NULL, 16);
        download_sum = 0;
        add_response("DATA%08lx", download_left);
    }
    else if (!strcmp(cmd, "silence")) { /* no response: used to test read timeouts */ }
    else if (!strcmp(cmd, "big"))
    {
        /* a 64 byte packet with a known pattern, for reads smaller than a packet */
        struct response *r = &responses[response_count++];
        int i;
        for (i = 0; i < 64; i++) r->data[i] = 'A' + i % 26;
        r->len = 64;
        flog("device response: 64 byte pattern\n");
    }
    else if (!strcmp(cmd, "stall"))
    {
        /* the IN endpoint halts until the host clears it */
        halted = 1;
        while (pending_count)
        {
            struct libusb_transfer *t = pending_in[0];
            memmove(pending_in, pending_in + 1, --pending_count * sizeof(*pending_in));
            queue_done(t, LIBUSB_TRANSFER_STALL, 0);
        }
    }
    else if (!strcmp(cmd, "unplug")) unplug_requested = 1;
    else if (!strcmp(cmd, "reboot")) add_response("OKAY");
    else add_response("FAILunknown command");
}

static void handle_out(struct libusb_transfer *t)
{
    total_bytes_out += t->length;
    if (download_left)
    {
        unsigned long n = (unsigned long)t->length < download_left ? (unsigned long)t->length : download_left;
        int i;
        for (i = 0; i < (int)n; i++) download_sum += t->buffer[i];
        download_left -= n;
        flog("device download: got %d bytes, %lu left\n", t->length, download_left);
        if (!download_left) { flog("device download complete: %lu bytes, checksum %lu\n", download_total, download_sum); add_response("OKAY"); }
    }
    else if (t->length)
        handle_command(t->buffer, t->length);
    else
        flog("device: zero length packet\n");
    queue_done(t, LIBUSB_TRANSFER_COMPLETED, t->length);
    match_in();
}

static const unsigned char device_desc[18] =
{
    18, LIBUSB_DT_DEVICE, 0x00, 0x02, 0, 0, 0, 64,
    0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1
};

static const unsigned char config_desc[32] =
{
    9, LIBUSB_DT_CONFIG, 32, 0, 1, 1, 0, 0x80, 250,
    9, LIBUSB_DT_INTERFACE, 0, 0, 2, 0xff, 0x42, 0x03, 0,
    7, LIBUSB_DT_ENDPOINT, EP_IN, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
    7, LIBUSB_DT_ENDPOINT, EP_OUT, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
};

/* Huawei normal mode: interface 0 = MTP-like, interface 1 = HDB/ADB (ff/42/01) */
static const unsigned char composite_device_desc[18] =
{
    18, LIBUSB_DT_DEVICE, 0x00, 0x02, 0, 0, 0, 64,
    0xd1, 0x12, 0x52, 0x10, 0x99, 0x02, 1, 2, 3, 1
};

static const unsigned char composite_config_desc[55] =
{
    9, LIBUSB_DT_CONFIG, 55, 0, 2, 1, 0, 0x80, 250,
    9, LIBUSB_DT_INTERFACE, 0, 0, 2, 0x06, 0x01, 0x01, 0,
    7, LIBUSB_DT_ENDPOINT, 0x82, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
    7, LIBUSB_DT_ENDPOINT, 0x02, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
    9, LIBUSB_DT_INTERFACE, 1, 0, 2, 0xff, 0x42, 0x01, 0,
    7, LIBUSB_DT_ENDPOINT, EP_IN, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
    7, LIBUSB_DT_ENDPOINT, EP_OUT, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
};

/* fastboot phone whose interface has a second alternate setting that uses
 * different endpoints */
static const unsigned char alt_config_desc[55] =
{
    9, LIBUSB_DT_CONFIG, 55, 0, 1, 1, 0, 0x80, 250,
    9, LIBUSB_DT_INTERFACE, 0, 0, 2, 0xff, 0x42, 0x03, 0,
    7, LIBUSB_DT_ENDPOINT, EP_IN, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
    7, LIBUSB_DT_ENDPOINT, EP_OUT, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
    9, LIBUSB_DT_INTERFACE, 0, 1, 2, 0xff, 0x42, 0x03, 0,
    7, LIBUSB_DT_ENDPOINT, 0x82, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
    7, LIBUSB_DT_ENDPOINT, 0x02, LIBUSB_TRANSFER_TYPE_BULK, MAX_PACKET & 0xff, MAX_PACKET >> 8, 0,
};

static int composite(void) { return getenv("FAKEUSB_COMPOSITE") != NULL; }
static int alt_mode(void) { return getenv("FAKEUSB_ALT") != NULL; }
#define DEV_DESC (composite() ? composite_device_desc : device_desc)
#define CFG_DESC (composite() ? composite_config_desc : alt_mode() ? alt_config_desc : config_desc)
#define CFG_LEN  (composite() || alt_mode() ? 55 : 32)

/* the endpoints of the alternate setting the host has selected */
static int ep_in(void) { return alt_mode() && host_alt ? 0x82 : EP_IN; }
static int ep_out(void) { return alt_mode() && host_alt ? 0x02 : EP_OUT; }

static int string_desc(int index, unsigned char *buf, int max)
{
    const char *str;
    int i, len;

    if (index == 0)
    {
        unsigned char langs[4] = {4, LIBUSB_DT_STRING, 0x09, 0x04};
        len = max < 4 ? max : 4;
        memcpy(buf, langs, len);
        return len;
    }
    str = index == 1 ? "Wine Test" : index == 2 ? "Fake Fastboot Phone" : index == 3 ? "FAKE0123" : NULL;
    if (!str) return -1;
    len = 2 + 2 * strlen(str);
    if (len > max) len = max;
    buf[0] = 2 + 2 * strlen(str);
    buf[1] = LIBUSB_DT_STRING;
    for (i = 2; i < len; i++) buf[i] = (i % 2) ? 0 : str[(i - 2) / 2];
    return len;
}

static void handle_control(struct libusb_transfer *t)
{
    struct libusb_control_setup *setup = (struct libusb_control_setup *)t->buffer;
    unsigned char *data = t->buffer + LIBUSB_CONTROL_SETUP_SIZE;
    int wlength = libusb_le16_to_cpu(setup->wLength);
    int len = -1;

    flog("device control: %02x %02x %04x %04x %04x\n", setup->bmRequestType, setup->bRequest,
            libusb_le16_to_cpu(setup->wValue), libusb_le16_to_cpu(setup->wIndex), wlength);

    if (setup->bmRequestType == 0x80 && setup->bRequest == LIBUSB_REQUEST_GET_DESCRIPTOR)
    {
        int type = libusb_le16_to_cpu(setup->wValue) >> 8, index = libusb_le16_to_cpu(setup->wValue) & 0xff;
        if (type == LIBUSB_DT_DEVICE) { len = wlength < 18 ? wlength : 18; memcpy(data, DEV_DESC, len); }
        else if (type == LIBUSB_DT_CONFIG) { len = wlength < CFG_LEN ? wlength : CFG_LEN; memcpy(data, CFG_DESC, len); }
        else if (type == LIBUSB_DT_STRING) len = string_desc(index, data, wlength);
    }
    else if (setup->bmRequestType == 0x01 && setup->bRequest == LIBUSB_REQUEST_SET_INTERFACE && alt_mode())
    {
        /* The device accepts the request, but a raw SET_INTERFACE goes
         * past the host's USB core, which keeps using the endpoints of
         * the old setting; only libusb_set_interface_alt_setting()
         * updates it. */
        flog("device: raw SET_INTERFACE %d, the host side is not updated\n", libusb_le16_to_cpu(setup->wValue));
        len = 0;
    }
    else if ((setup->bmRequestType & 0x60) == 0x40) /* vendor */
    {
        if (setup->bmRequestType & 0x80)
        {
            len = wlength < 4 ? wlength : 4;
            memcpy(data, "WINE", len);
        }
        else len = wlength;
    }

    if (len < 0) queue_done(t, LIBUSB_TRANSFER_STALL, 0);
    else queue_done(t, LIBUSB_TRANSFER_COMPLETED, len); /* like libusb: excludes the setup packet */
}

/* API */

int LIBUSB_CALL libusb_init(libusb_context **ctx) { flog("libusb_init\n"); if (ctx) *ctx = NULL; return 0; }
void LIBUSB_CALL libusb_exit(libusb_context *ctx) { flog("libusb_exit\n"); }

const char * LIBUSB_CALL libusb_strerror(int code)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "fake error %d", code);
    return buf;
}

int LIBUSB_CALL libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags,
        int vendor_id, int product_id, int dev_class, libusb_hotplug_callback_fn cb_fn,
        void *user_data, libusb_hotplug_callback_handle *handle)
{
    flog("hotplug register, flags %#x\n", flags);
    hotplug_cb = cb_fn;
    hotplug_data = user_data;
    if (handle) *handle = 1;
    if (flags & LIBUSB_HOTPLUG_ENUMERATE)
        cb_fn(ctx, &fake_dev, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, user_data);
    return 0;
}

void LIBUSB_CALL libusb_hotplug_deregister_callback(libusb_context *ctx, libusb_hotplug_callback_handle handle) {}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
    memcpy(desc, DEV_DESC, sizeof(device_desc));
    desc->bcdUSB = 0x0200;
    desc->idVendor = composite() ? 0x12d1 : 0x18d1;
    desc->idProduct = composite() ? 0x1052 : 0xd00d;
    desc->bcdDevice = composite() ? 0x0299 : 0x0100;
    return 0;
}

int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *dev, struct libusb_config_descriptor **config)
{
    int n = composite() ? 2 : 1, i;
    struct libusb_config_descriptor *c = calloc(1, sizeof(*c));
    struct libusb_interface *iface = calloc(n, sizeof(*iface));
    struct libusb_interface_descriptor *alt = calloc(n, sizeof(*alt));

    c->bLength = 9; c->bDescriptorType = LIBUSB_DT_CONFIG; c->wTotalLength = CFG_LEN;
    c->bNumInterfaces = n; c->bConfigurationValue = 1;
    for (i = 0; i < n; i++)
    {
        alt[i].bLength = 9; alt[i].bDescriptorType = LIBUSB_DT_INTERFACE; alt[i].bNumEndpoints = 2;
        alt[i].bInterfaceNumber = i;
        alt[i].bInterfaceClass = 0xff; alt[i].bInterfaceSubClass = 0x42; alt[i].bInterfaceProtocol = composite() ? 0x01 : 0x03;
        if (composite() && i == 0) { alt[i].bInterfaceClass = 0x06; alt[i].bInterfaceSubClass = 0x01; alt[i].bInterfaceProtocol = 0x01; }
        iface[i].altsetting = &alt[i]; iface[i].num_altsetting = 1;
    }
    c->interface = iface;
    *config = c;
    return 0;
}

void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor *config)
{
    if (!config) return;
    free((void *)config->interface[0].altsetting);
    free((void *)config->interface);
    free(config);
}

int LIBUSB_CALL libusb_get_device_speed(libusb_device *dev)
{
    const char *speed = getenv("FAKEUSB_SPEED");

    if (!speed) return LIBUSB_SPEED_HIGH;
    if (!strcmp(speed, "low")) return LIBUSB_SPEED_LOW;
    if (!strcmp(speed, "full")) return LIBUSB_SPEED_FULL;
    if (!strcmp(speed, "super")) return LIBUSB_SPEED_SUPER;
    return LIBUSB_SPEED_HIGH;
}

uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *dev) { return 1; }
uint8_t LIBUSB_CALL libusb_get_port_number(libusb_device *dev) { return 4; }
libusb_device * LIBUSB_CALL libusb_get_device(libusb_device_handle *h) { return h->dev; }

int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **handle)
{
    flog("libusb_open\n");
    *handle = &fake_handle;
    return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *handle)
{
    pthread_mutex_lock(&lock);
    if (pending_count || done_count)
        flog("FAKEUSB ERROR: libusb_close with %d transfer(s) still in flight\n", pending_count + done_count);
    pthread_mutex_unlock(&lock);
    flog("libusb_close\n");
}

int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *handle, unsigned char endpoint)
{
    flog("clear halt %#x\n", endpoint);
    pthread_mutex_lock(&lock);
    if (endpoint & 0x80) halted = 0;
    pthread_mutex_unlock(&lock);
    return 0;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *handle, int iface)
{
    flog("claim interface %d\n", iface);
    pthread_mutex_lock(&lock);
    claimed |= 1 << iface;
    pthread_mutex_unlock(&lock);
    return 0;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle, int iface)
{
    pthread_mutex_lock(&lock);
    claimed &= ~(1 << iface);
    pthread_mutex_unlock(&lock);
    return 0;
}

int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *handle, int iface, int alt)
{
    int ret = 0;

    pthread_mutex_lock(&lock);
    if (gone) ret = LIBUSB_ERROR_NO_DEVICE;
    else if (!(claimed & (1 << iface))) ret = LIBUSB_ERROR_NOT_FOUND; /* like libusb */
    else if (iface != 0 || alt < 0 || alt > (alt_mode() ? 1 : 0)) ret = LIBUSB_ERROR_NOT_FOUND;
    else host_alt = alt;
    pthread_mutex_unlock(&lock);
    flog("set interface %d alt setting %d: %d\n", iface, alt, ret);
    return ret;
}

struct libusb_transfer * LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    return calloc(1, sizeof(struct libusb_transfer) + iso_packets * sizeof(struct libusb_iso_packet_descriptor));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *t)
{
    if (!t) return;
    if ((t->flags & LIBUSB_TRANSFER_FREE_BUFFER) && t->buffer) free(t->buffer);
    free(t);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *t)
{
    pthread_mutex_lock(&lock);
    if (gone)
    {
        pthread_mutex_unlock(&lock);
        return LIBUSB_ERROR_NO_DEVICE;
    }
    if (getenv("FAKEUSB_FAIL_SUBMIT") && t->type == LIBUSB_TRANSFER_TYPE_BULK && t->endpoint == EP_OUT
            && t->length == 4 && !memcmp(t->buffer, "fail", 4))
    {
        pthread_mutex_unlock(&lock);
        flog("submit: simulated failure\n");
        return LIBUSB_ERROR_IO;
    }
    switch (t->type)
    {
        case LIBUSB_TRANSFER_TYPE_CONTROL:
            handle_control(t);
            break;
        case LIBUSB_TRANSFER_TYPE_BULK:
            if (t->endpoint == ep_in())
            {
                flog("submit IN %d\n", t->length);
                if (halted)
                {
                    queue_done(t, LIBUSB_TRANSFER_STALL, 0);
                    break;
                }
                pending_in[pending_count++] = t;
                match_in();
            }
            else if (t->endpoint == ep_out())
            {
                flog("submit OUT %d\n", t->length);
                handle_out(t);
            }
            else
            {
                /* usbfs: the endpoint does not exist in the current setting */
                flog("submit to unknown endpoint %#x\n", t->endpoint);
                pthread_mutex_unlock(&lock);
                return LIBUSB_ERROR_NOT_FOUND;
            }
            break;
        default:
            pthread_mutex_unlock(&lock);
            return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *t)
{
    int i, ret = LIBUSB_ERROR_NOT_FOUND;

    pthread_mutex_lock(&lock);
    for (i = 0; i < pending_count; i++)
    {
        if (pending_in[i] == t)
        {
            if (gone)
            {
                /* the transfer fails with NO_DEVICE once the disconnect is handled */
                ret = LIBUSB_ERROR_NO_DEVICE;
                break;
            }
            memmove(pending_in + i, pending_in + i + 1, (--pending_count - i) * sizeof(*pending_in));
            if (getenv("FAKEUSB_CANCEL_COMPLETES") && t->length >= 4)
            {
                memcpy(t->buffer, "LATE", 4);
                queue_done(t, LIBUSB_TRANSFER_COMPLETED, 4);
            }
            else queue_done(t, LIBUSB_TRANSFER_CANCELLED, 0);
            ret = 0;
            break;
        }
    }
    flog("cancel transfer %p: %d\n", t, ret);
    pthread_mutex_unlock(&lock);
    return ret;
}

/* lock must be held */
static void fail_pending_transfers(void)
{
    while (pending_count)
    {
        struct libusb_transfer *t = pending_in[0];
        memmove(pending_in, pending_in + 1, --pending_count * sizeof(*pending_in));
        queue_done(t, LIBUSB_TRANSFER_NO_DEVICE, 0);
    }
}

/* lock must be held; it is released around the callbacks */
static void run_callbacks(void)
{
    while (done_count)
    {
        struct libusb_transfer *t = done_queue[0];
        memmove(done_queue, done_queue + 1, --done_count * sizeof(*done_queue));
        pthread_mutex_unlock(&lock);
        t->callback(t);
        if (t->flags & LIBUSB_TRANSFER_FREE_TRANSFER)
            libusb_free_transfer(t);
        pthread_mutex_lock(&lock);
    }
}

static int time_reached(const struct timespec *when)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > when->tv_sec || (now.tv_sec == when->tv_sec && now.tv_nsec >= when->tv_nsec);
}

int LIBUSB_CALL libusb_handle_events(libusb_context *ctx)
{
    struct timespec ts;

    pthread_mutex_lock(&lock);
    if (!done_count && !interrupted && !unplug_requested)
    {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (late_completion ? 20 : 200) * 1000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&cond, &lock, &ts);
    }
    interrupted = 0;
    run_callbacks();

    if (unplug_requested && !gone)
    {
        gone = 1;
        if (getenv("FAKEUSB_UNPLUG_LATE"))
        {
            flog("device unplugged, %d transfer(s) stay pending for now\n", pending_count);
            late_completion = 1;
            clock_gettime(CLOCK_REALTIME, &late_time);
            late_time.tv_nsec += 300 * 1000000;
            if (late_time.tv_nsec >= 1000000000) { late_time.tv_sec++; late_time.tv_nsec -= 1000000000; }
        }
        else
        {
            flog("device unplugged, failing %d pending transfer(s)\n", pending_count);
            fail_pending_transfers();
            run_callbacks();
        }
        pthread_mutex_unlock(&lock);
        if (hotplug_cb) hotplug_cb(ctx, &fake_dev, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, hotplug_data);
        pthread_mutex_lock(&lock);
    }

    if (late_completion && time_reached(&late_time))
    {
        flog("failing %d transfer(s) of the unplugged device\n", pending_count);
        late_completion = 0;
        fail_pending_transfers();
        run_callbacks();
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

void LIBUSB_CALL libusb_interrupt_event_handler(libusb_context *ctx)
{
    pthread_mutex_lock(&lock);
    interrupted = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
}

__attribute__((destructor)) static void report(void)
{
    flog("device stats: %lu bytes received over bulk OUT, last download checksum %lu of %lu bytes\n",
            total_bytes_out, download_sum, download_total);
}
