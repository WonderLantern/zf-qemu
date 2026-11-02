/*
 * Nikon Z 6III -- a USB host on the other end of the camera's USB port,
 * and a USB/IP server in front of it (-machine usbip=).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The camera is a USB device (nikon-z63-dwc3.h); a device needs a host.
 * This is that host, reduced to what a host controller does for one
 * device on one port:
 *
 *   - when the camera comes on the bus (the driver's run bit) it resets
 *     the port 20 ms later, as a hub does on a connect -- the reset is
 *     what makes the controller post Reset and ConnectDone;
 *   - it runs transfers: a control transfer as its three stages (SETUP,
 *     the data stage IN or OUT, the status stage the other way), a bulk
 *     transfer as tokens until it is full or a packet comes back short
 *     (or, OUT, until every byte has gone); a NAK is retried when the
 *     controller says an endpoint was armed, and every millisecond
 *     otherwise; a STALL ends the transfer;
 *   - after each reset it reads the device descriptor, sets address 1 and
 *     reads the configuration descriptor, for the USB/IP device list and
 *     the log.
 *
 * USB/IP (the protocol of Linux's usbip-host / vhci-hcd, version 1.1.1)
 * is the outside: OP_REQ_DEVLIST, OP_REQ_IMPORT of bus id "1-1", then
 * CMD_SUBMIT / RET_SUBMIT and CMD_UNLINK / RET_UNLINK, one client at a
 * time. A Linux host should attach it with `usbip attach -r <host> -b
 * 1-1` (TCP, usbip=tcp:<addr>:3240) and gphoto2 talk PTP to it as to the
 * real one -- not tried; what is tried is z63-tools/ptp.py, a client for
 * hosts without vhci-hcd (macOS): descriptors, PTP info/ls/get, a remote
 * release.
 * The transfers are the client's own: this side configures nothing and
 * speaks no class protocol.
 *
 * -machine usbip=<path> | unix:<path> | tcp:<host>:<port> | <host>:<port>
 */

#ifndef HW_ARM_NIKON_Z63_USBHOST_H
#define HW_ARM_NIKON_Z63_USBHOST_H

#include "qemu/sockets.h"
#include "qemu/main-loop.h"
#include "qapi/qapi-types-sockets.h"

#define Z63_USBHOST_PIPES       32      /* (ep & 0xf) | in << 4 */
#define Z63_USBHOST_RETRY_NS    (1 * SCALE_MS)
#define Z63_USBHOST_RESET_NS    (20 * SCALE_MS)
#define Z63_USBHOST_BULK_MAXP   512     /* high speed bulk */
#define Z63_USBHOST_MAX_LEN     (16 * MiB)

#define Z63_USBIP_VERSION       0x0111
#define Z63_USBIP_REQ_IMPORT    0x8003
#define Z63_USBIP_REP_IMPORT    0x0003
#define Z63_USBIP_REQ_DEVLIST   0x8005
#define Z63_USBIP_REP_DEVLIST   0x0005
#define Z63_USBIP_CMD_SUBMIT    1
#define Z63_USBIP_CMD_UNLINK    2
#define Z63_USBIP_RET_SUBMIT    3
#define Z63_USBIP_RET_UNLINK    4
#define Z63_USBIP_DEVICE_SIZE   312
#define Z63_USBIP_BUSID         "1-1"

typedef struct Z63UsbHost Z63UsbHost;
typedef struct Z63UsbXfer Z63UsbXfer;

typedef void (*Z63UsbDone)(Z63UsbHost *h, Z63UsbXfer *x);

enum { Z63X_SETUP, Z63X_DATA, Z63X_STATUS, Z63X_BULK };

struct Z63UsbXfer {
    QTAILQ_ENTRY(Z63UsbXfer) link;
    uint8_t ep;                 /* address: number | 0x80 for IN */
    bool control;
    uint8_t setup[8];
    bool in;                    /* data direction */
    uint8_t *data;
    uint32_t len, actual;
    int phase;
    int status;                 /* 0, or a negative errno (USB/IP's) */
    Z63UsbDone done;
    uint32_t seqnum;            /* USB/IP; 0 for the host's own */
    bool orphan;                /* its client is gone or unlinked it */
};

typedef struct Z63UsbPipe {
    QTAILQ_HEAD(, Z63UsbXfer) q;
    bool waiting;               /* the head was NAKed */
} Z63UsbPipe;

typedef struct Z63UsbIpConn {
    int fd;
    uint8_t hdr[48];
    unsigned have, need;
    uint8_t *body;
    unsigned body_len;
    int state;
    GByteArray *out;
} Z63UsbIpConn;

enum { Z63IP_OP, Z63IP_BUSID, Z63IP_URB, Z63IP_DATA };

struct Z63UsbHost {
    Z63Dwc3 *ctl;               /* the camera's controller */
    bool ready;                 /* reset done: tokens may go */
    QEMUTimer *reset_timer;
    QEMUTimer *retry_timer;
    QEMUBH *kick_bh;
    Z63UsbPipe pipe[Z63_USBHOST_PIPES];
    /* what the device said about itself after the last reset */
    uint8_t dev_desc[18];
    uint8_t cfg_desc[1024];
    unsigned cfg_len;
    bool desc_valid;
    /* USB/IP */
    char *addr;
    int listen_fd;
    Z63UsbIpConn *conn;
    uint64_t submits;
};

static unsigned z63_usbhost_pipe_of(uint8_t ep)
{
    return (ep & 0x0f) | ((ep & 0x80) && (ep & 0x0f) ? 0x10 : 0);
}

/* ---------------------------------------------------------------- */
/* Transfers                                                         */
/* ---------------------------------------------------------------- */

static int z63_usbhost_token(Z63UsbHost *h, Z63UsbXfer *x, int pid,
                             void *buf, uint32_t len, uint32_t *moved)
{
    return z63_dwc3_token(h->ctl, pid, x->ep & 0x0f, buf, len, moved);
}

/* One step of the head transfer: true when it moved (or ended), false
 * when the device NAKed and the pipe has to wait. */
static bool z63_usbhost_step(Z63UsbHost *h, Z63UsbXfer *x, bool *finished)
{
    uint32_t moved = 0;
    int st;

    *finished = false;
    if (!h->ctl->on_bus || !h->ready) {
        x->status = -ESHUTDOWN;
        *finished = true;
        return true;
    }
    switch (x->phase) {
    case Z63X_SETUP:
        st = z63_usbhost_token(h, x, Z63_TOK_SETUP, x->setup, 8, &moved);
        if (st == Z63_TOK_OK) {
            x->phase = x->len ? Z63X_DATA : Z63X_STATUS;
        }
        break;
    case Z63X_DATA:
        st = z63_usbhost_token(h, x, x->in ? Z63_TOK_IN : Z63_TOK_OUT,
                               x->data + x->actual, x->len - x->actual,
                               &moved);
        if (st == Z63_TOK_OK) {
            x->actual += moved;
            if (x->in || x->actual >= x->len || !moved) {
                x->phase = Z63X_STATUS;
            }
        }
        break;
    case Z63X_STATUS:
        st = z63_usbhost_token(h, x, x->in && x->len ? Z63_TOK_OUT
                                                     : Z63_TOK_IN,
                               NULL, 0, &moved);
        if (st == Z63_TOK_OK) {
            x->status = 0;
            *finished = true;
        }
        break;
    default:    /* bulk */
        st = z63_usbhost_token(h, x, x->in ? Z63_TOK_IN : Z63_TOK_OUT,
                               x->data + x->actual, x->len - x->actual,
                               &moved);
        if (st == Z63_TOK_OK) {
            if (!x->in && !moved && x->len) {
                st = Z63_TOK_NAK;           /* nothing taken: wait */
                break;
            }
            x->actual += moved;
            if (x->in ? (moved == 0 || moved % Z63_USBHOST_BULK_MAXP ||
                         x->actual >= x->len)
                      : x->actual >= x->len) {
                x->status = 0;
                *finished = true;
            }
        }
        break;
    }
    if (st == Z63_TOK_NAK) {
        return false;
    }
    if (st != Z63_TOK_OK) {
        x->status = -EPIPE;
        *finished = true;
    }
    return true;
}

static void z63_usbhost_run(Z63UsbHost *h, Z63UsbPipe *pipe)
{
    Z63UsbXfer *x;

    pipe->waiting = false;
    while ((x = QTAILQ_FIRST(&pipe->q)) != NULL) {
        bool finished;

        if (!z63_usbhost_step(h, x, &finished)) {
            pipe->waiting = true;
            if (!timer_pending(h->retry_timer)) {
                timer_mod(h->retry_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          Z63_USBHOST_RETRY_NS);
            }
            return;
        }
        if (finished) {
            QTAILQ_REMOVE(&pipe->q, x, link);
            x->done(h, x);
        }
    }
}

static void z63_usbhost_kick(void *opaque)
{
    Z63UsbHost *h = opaque;

    for (unsigned i = 0; i < Z63_USBHOST_PIPES; i++) {
        if (h->pipe[i].waiting) {
            z63_usbhost_run(h, &h->pipe[i]);
        }
    }
}

static void z63_usbhost_submit(Z63UsbHost *h, Z63UsbXfer *x)
{
    Z63UsbPipe *pipe = &h->pipe[z63_usbhost_pipe_of(x->ep)];
    bool idle = QTAILQ_EMPTY(&pipe->q);

    x->phase = x->control ? Z63X_SETUP : Z63X_BULK;
    QTAILQ_INSERT_TAIL(&pipe->q, x, link);
    if (idle) {
        z63_usbhost_run(h, pipe);
    }
}

/* Every queued transfer ends with `status`, its owner told. */
static void z63_usbhost_fail_all(Z63UsbHost *h, int status)
{
    for (unsigned i = 0; i < Z63_USBHOST_PIPES; i++) {
        Z63UsbXfer *x;

        h->pipe[i].waiting = false;
        while ((x = QTAILQ_FIRST(&h->pipe[i].q)) != NULL) {
            QTAILQ_REMOVE(&h->pipe[i].q, x, link);
            x->status = status;
            x->done(h, x);
        }
    }
}

/* The controller armed an endpoint (nikon-z63-dwc3.h): try again, from
 * the main loop rather than from inside its register write. */
static void z63_usbhost_ep_armed(void *opaque, unsigned phys)
{
    Z63UsbHost *h = opaque;

    qemu_bh_schedule(h->kick_bh);
}

/* ---------------------------------------------------------------- */
/* The host's own reads after a reset                                */
/* ---------------------------------------------------------------- */

static void z63_usbhost_control(Z63UsbHost *h, uint8_t type, uint8_t req,
                                uint16_t value, uint16_t index,
                                uint16_t length, Z63UsbDone done)
{
    Z63UsbXfer *x = g_new0(Z63UsbXfer, 1);

    x->ep = (type & 0x80) ? 0x80 : 0;
    x->control = true;
    x->in = type & 0x80;
    x->setup[0] = type;
    x->setup[1] = req;
    stw_le_p(x->setup + 2, value);
    stw_le_p(x->setup + 4, index);
    stw_le_p(x->setup + 6, length);
    x->len = length;
    x->data = g_malloc0(MAX(length, 1));
    x->done = done;
    z63_usbhost_submit(h, x);
}

static void z63_usbhost_free(Z63UsbXfer *x)
{
    g_free(x->data);
    g_free(x);
}

static void z63_usbhost_got_config(Z63UsbHost *h, Z63UsbXfer *x)
{
    if (!x->status && x->actual >= 9) {
        h->cfg_len = MIN(x->actual, sizeof(h->cfg_desc));
        memcpy(h->cfg_desc, x->data, h->cfg_len);
        h->desc_valid = true;
        error_report("Z63_USB: the camera enumerated -- %04x:%04x, class "
                     "%u, %u configuration(s), %u interface(s), %u bytes of "
                     "configuration%s", lduw_le_p(h->dev_desc + 8),
                     lduw_le_p(h->dev_desc + 10), h->dev_desc[4],
                     h->dev_desc[17], h->cfg_desc[4], h->cfg_len,
                     h->addr ? "; USB/IP bus id " Z63_USBIP_BUSID : "");
    } else {
        error_report("Z63_USB: configuration descriptor read failed (%d)",
                     x->status);
    }
    z63_usbhost_free(x);
}

static void z63_usbhost_got_config9(Z63UsbHost *h, Z63UsbXfer *x)
{
    if (!x->status && x->actual >= 4) {
        uint16_t total = lduw_le_p(x->data + 2);

        z63_usbhost_control(h, 0x80, 6, 0x0200, 0,
                            MIN(total, sizeof(h->cfg_desc)),
                            z63_usbhost_got_config);
    } else {
        error_report("Z63_USB: configuration descriptor read failed (%d)",
                     x->status);
    }
    z63_usbhost_free(x);
}

static void z63_usbhost_set_address(Z63UsbHost *h, Z63UsbXfer *x)
{
    if (!x->status) {
        z63_usbhost_control(h, 0x80, 6, 0x0200, 0, 9,
                            z63_usbhost_got_config9);
    } else {
        error_report("Z63_USB: SET_ADDRESS failed (%d)", x->status);
    }
    z63_usbhost_free(x);
}

static void z63_usbhost_got_device(Z63UsbHost *h, Z63UsbXfer *x)
{
    if (!x->status && x->actual >= 18) {
        memcpy(h->dev_desc, x->data, 18);
        z63_usbhost_control(h, 0x00, 5, 1, 0, 0, z63_usbhost_set_address);
    } else {
        error_report("Z63_USB: device descriptor read failed (%d, %u "
                     "bytes)", x->status, x->actual);
    }
    z63_usbhost_free(x);
}

static void z63_usbhost_reset_tick(void *opaque)
{
    Z63UsbHost *h = opaque;

    if (!h->ctl->on_bus) {
        return;
    }
    z63_dwc3_bus_reset(h->ctl);
    h->ready = true;
    h->desc_valid = false;
    error_report("Z63_USB: port reset -- reading the camera's descriptors");
    z63_usbhost_control(h, 0x80, 6, 0x0100, 0, 18, z63_usbhost_got_device);
}

/* ---------------------------------------------------------------- */
/* On and off the bus (the controller's run bit)                     */
/* ---------------------------------------------------------------- */

static void z63_usbhost_attached(void *opaque)
{
    Z63UsbHost *h = opaque;

    h->ready = false;
    timer_mod(h->reset_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              Z63_USBHOST_RESET_NS);
}

static void z63_usbhost_detached(void *opaque)
{
    Z63UsbHost *h = opaque;

    h->ready = false;
    h->desc_valid = false;
    timer_del(h->reset_timer);
    z63_usbhost_fail_all(h, -ESHUTDOWN);
}

/* ---------------------------------------------------------------- */
/* USB/IP                                                            */
/* ---------------------------------------------------------------- */

static void z63_usbip_close(Z63UsbHost *h);

static void z63_usbip_flush(void *opaque);

static void z63_usbip_send(Z63UsbHost *h, const void *buf, size_t len)
{
    Z63UsbIpConn *c = h->conn;

    if (!c) {
        return;
    }
    g_byte_array_append(c->out, buf, len);
    z63_usbip_flush(h);
}

static void z63_usbip_read(void *opaque);

static void z63_usbip_flush(void *opaque)
{
    Z63UsbHost *h = opaque;
    Z63UsbIpConn *c = h->conn;

    while (c && c->out->len) {
        ssize_t n = send(c->fd, c->out->data, c->out->len, 0);

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (n <= 0) {
            z63_usbip_close(h);
            return;
        }
        g_byte_array_remove_range(c->out, 0, n);
    }
    if (c) {
        qemu_set_fd_handler(c->fd, z63_usbip_read,
                            c->out->len ? z63_usbip_flush : NULL, h);
    }
}

/* The usbip_usb_device record: 312 bytes, big-endian. */
static void z63_usbip_device(Z63UsbHost *h, uint8_t *d)
{
    memset(d, 0, Z63_USBIP_DEVICE_SIZE);
    pstrcpy((char *)d, 256, "/sys/devices/platform/nikon-z63/usb1/1-1");
    pstrcpy((char *)d + 256, 32, Z63_USBIP_BUSID);
    stl_be_p(d + 288, 1);                       /* busnum */
    stl_be_p(d + 292, 2);                       /* devnum */
    stl_be_p(d + 296, 3);                       /* speed: high */
    stw_be_p(d + 300, lduw_le_p(h->dev_desc + 8));
    stw_be_p(d + 302, lduw_le_p(h->dev_desc + 10));
    stw_be_p(d + 304, lduw_le_p(h->dev_desc + 12));
    d[306] = h->dev_desc[4];
    d[307] = h->dev_desc[5];
    d[308] = h->dev_desc[6];
    d[309] = 0;                                 /* configuration value */
    d[310] = h->dev_desc[17];
    d[311] = h->cfg_desc[4];
}

static void z63_usbip_devlist(Z63UsbHost *h)
{
    g_autoptr(GByteArray) r = g_byte_array_new();
    uint8_t hdr[12], dev[Z63_USBIP_DEVICE_SIZE];

    stw_be_p(hdr, Z63_USBIP_VERSION);
    stw_be_p(hdr + 2, Z63_USBIP_REP_DEVLIST);
    stl_be_p(hdr + 4, 0);
    stl_be_p(hdr + 8, h->desc_valid ? 1 : 0);
    g_byte_array_append(r, hdr, sizeof(hdr));
    if (h->desc_valid) {
        z63_usbip_device(h, dev);
        g_byte_array_append(r, dev, sizeof(dev));
        /* each interface, alternate setting 0: class, subclass, protocol */
        for (unsigned o = 0; o + 9 <= h->cfg_len && h->cfg_desc[o];
             o += h->cfg_desc[o]) {
            if (h->cfg_desc[o + 1] == 4 && h->cfg_desc[o + 3] == 0) {
                uint8_t i[4] = { h->cfg_desc[o + 5], h->cfg_desc[o + 6],
                                 h->cfg_desc[o + 7], 0 };

                g_byte_array_append(r, i, 4);
            }
        }
    }
    z63_usbip_send(h, r->data, r->len);
}

static void z63_usbip_import(Z63UsbHost *h, const char *busid)
{
    uint8_t r[8 + Z63_USBIP_DEVICE_SIZE];
    bool ok = h->desc_valid && !strncmp(busid, Z63_USBIP_BUSID, 32);

    stw_be_p(r, Z63_USBIP_VERSION);
    stw_be_p(r + 2, Z63_USBIP_REP_IMPORT);
    stl_be_p(r + 4, ok ? 0 : 1);
    if (ok) {
        z63_usbip_device(h, r + 8);
        h->conn->state = Z63IP_URB;
        error_report("Z63_USB: USB/IP client attached the camera");
    } else {
        error_report("Z63_USB: USB/IP import of '%.32s' refused -- %s",
                     busid, h->desc_valid ? "no such bus id"
                                          : "the camera is not on the bus");
    }
    z63_usbip_send(h, r, ok ? sizeof(r) : 8);
}

static void z63_usbip_ret_submit(Z63UsbHost *h, Z63UsbXfer *x)
{
    if (!x->orphan && h->conn && h->conn->state >= Z63IP_URB) {
        uint8_t r[48] = { 0 };
        bool in = x->control ? x->in : (x->ep & 0x80);
        uint32_t data = in && !x->status ? x->actual : 0;

        stl_be_p(r, Z63_USBIP_RET_SUBMIT);
        stl_be_p(r + 4, x->seqnum);
        stl_be_p(r + 20, (uint32_t)x->status);
        stl_be_p(r + 24, x->status ? 0 : x->actual);
        g_byte_array_append(h->conn->out, r, sizeof(r));
        if (data) {
            g_byte_array_append(h->conn->out, x->data, data);
        }
        z63_usbip_flush(h);
    }
    z63_usbhost_free(x);
}

static void z63_usbip_submit(Z63UsbHost *h, const uint8_t *hdr,
                             uint8_t *data, uint32_t len)
{
    Z63UsbXfer *x = g_new0(Z63UsbXfer, 1);
    uint32_t dir = ldl_be_p(hdr + 12), ep = ldl_be_p(hdr + 16);

    x->seqnum = ldl_be_p(hdr + 4);
    x->control = (ep & 0x0f) == 0;
    x->len = len;
    x->data = data ? data : g_malloc0(MAX(len, 1));
    if (x->control) {
        memcpy(x->setup, hdr + 40, 8);
        x->in = x->setup[0] & 0x80;
        x->ep = 0;
    } else {
        x->in = dir == 1;
        x->ep = (ep & 0x0f) | (x->in ? 0x80 : 0);
    }
    x->done = z63_usbip_ret_submit;
    h->submits++;
    z63_usbhost_submit(h, x);
}

static void z63_usbip_unlink(Z63UsbHost *h, const uint8_t *hdr)
{
    uint32_t seq = ldl_be_p(hdr + 4), victim = ldl_be_p(hdr + 20);
    uint8_t r[48] = { 0 };
    int status = 0;

    for (unsigned i = 0; i < Z63_USBHOST_PIPES && !status; i++) {
        Z63UsbXfer *x, *head = QTAILQ_FIRST(&h->pipe[i].q);

        QTAILQ_FOREACH(x, &h->pipe[i].q, link) {
            if (x->seqnum != victim || x->orphan) {
                continue;
            }
            status = -ECONNRESET;
            if (x != head || x->phase == Z63X_SETUP ||
                (x->phase == Z63X_BULK && !x->actual)) {
                /* not started on the wire: just take it away, and let
                 * whatever is behind it go */
                bool was_head = x == head;

                QTAILQ_REMOVE(&h->pipe[i].q, x, link);
                    z63_usbhost_free(x);
                if (was_head) {
                    h->pipe[i].waiting = !QTAILQ_EMPTY(&h->pipe[i].q);
                    qemu_bh_schedule(h->kick_bh);
                }
            } else {
                x->orphan = true;   /* finishes, unanswered */
            }
            break;
        }
    }
    stl_be_p(r, Z63_USBIP_RET_UNLINK);
    stl_be_p(r + 4, seq);
    stl_be_p(r + 20, (uint32_t)status);
    z63_usbip_send(h, r, sizeof(r));
}

static void z63_usbip_close(Z63UsbHost *h)
{
    Z63UsbIpConn *c = h->conn;

    if (!c) {
        return;
    }
    qemu_set_fd_handler(c->fd, NULL, NULL, NULL);
    close(c->fd);
    g_byte_array_free(c->out, true);
    g_free(c->body);
    g_free(c);
    h->conn = NULL;
    for (unsigned i = 0; i < Z63_USBHOST_PIPES; i++) {
        Z63UsbXfer *x;

        QTAILQ_FOREACH(x, &h->pipe[i].q, link) {
            if (x->seqnum) {
                x->orphan = true;
            }
        }
    }
    error_report("Z63_USB: USB/IP client gone");
}

/* Bytes in: an 8-byte op header, a 32-byte bus id, then 48-byte URB
 * headers each followed by its OUT data. */
static void z63_usbip_read(void *opaque)
{
    Z63UsbHost *h = opaque;
    Z63UsbIpConn *c = h->conn;

    while (c) {
        uint8_t *dst = c->state == Z63IP_DATA ? c->body + c->have
                                              : c->hdr + c->have;
        ssize_t n = recv(c->fd, dst, c->need - c->have, 0);

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (n <= 0) {
            z63_usbip_close(h);
            return;
        }
        c->have += n;
        if (c->have < c->need) {
            continue;
        }
        c->have = 0;
        switch (c->state) {
        case Z63IP_OP: {
            uint16_t code = lduw_be_p(c->hdr + 2);

            if (code == Z63_USBIP_REQ_DEVLIST) {
                z63_usbip_devlist(h);
                c->need = 8;
            } else if (code == Z63_USBIP_REQ_IMPORT) {
                c->state = Z63IP_BUSID;
                c->need = 32;
            } else {
                error_report("Z63_USB: USB/IP op 0x%04x not understood",
                             code);
                z63_usbip_close(h);
                return;
            }
            break;
        }
        case Z63IP_BUSID:
            c->need = 48;
            c->state = Z63IP_OP;
            z63_usbip_import(h, (const char *)c->hdr);
            if (h->conn && c->state != Z63IP_URB) {
                z63_usbip_close(h);
                return;
            }
            break;
        case Z63IP_URB: {
            uint32_t cmd = ldl_be_p(c->hdr);

            c->need = 48;
            if (cmd == Z63_USBIP_CMD_SUBMIT) {
                uint32_t dir = ldl_be_p(c->hdr + 12);
                int32_t len = (int32_t)ldl_be_p(c->hdr + 24);

                if (len < 0 || len > Z63_USBHOST_MAX_LEN) {
                    error_report("Z63_USB: USB/IP transfer of %d bytes "
                                 "refused", len);
                    z63_usbip_close(h);
                    return;
                }
                if (dir == 0 && len > 0) {
                    c->body = g_malloc(len);
                    c->body_len = len;
                    c->need = len;
                    c->state = Z63IP_DATA;
                } else {
                    z63_usbip_submit(h, c->hdr, NULL, len);
                }
            } else if (cmd == Z63_USBIP_CMD_UNLINK) {
                z63_usbip_unlink(h, c->hdr);
            } else {
                error_report("Z63_USB: USB/IP command %u not understood",
                             cmd);
                z63_usbip_close(h);
                return;
            }
            break;
        }
        case Z63IP_DATA: {
            uint8_t *body = c->body;

            c->body = NULL;
            c->state = Z63IP_URB;
            c->need = 48;
            z63_usbip_submit(h, c->hdr, body, c->body_len);
            break;
        }
        }
        c = h->conn;
    }
}

static void z63_usbip_accept(void *opaque)
{
    Z63UsbHost *h = opaque;
    int fd = qemu_accept(h->listen_fd, NULL, NULL);
    Z63UsbIpConn *c;

    if (fd < 0) {
        return;
    }
    if (h->conn) {
        /* one client: the camera's endpoints have one owner */
        close(fd);
        return;
    }
    qemu_set_blocking(fd, false, NULL);
    c = g_new0(Z63UsbIpConn, 1);
    c->fd = fd;
    c->need = 8;
    c->state = Z63IP_OP;
    c->out = g_byte_array_new();
    h->conn = c;
    qemu_set_fd_handler(fd, z63_usbip_read, NULL, h);
}

static void z63_usbip_listen(Z63UsbHost *h, Error **errp)
{
    const char *a = h->addr;
    g_autofree char *spec = NULL;
    SocketAddress *sa;

    if (g_str_has_prefix(a, "tcp:")) {
        a += 4;
    }
    spec = a[0] == '/' || a[0] == '.' ? g_strdup_printf("unix:%s", a)
                                      : g_strdup(a);
    sa = socket_parse(spec, errp);
    if (!sa) {
        return;
    }
    if (sa->type == SOCKET_ADDRESS_TYPE_UNIX) {
        unlink(sa->u.q_unix.path);
    }
    h->listen_fd = socket_listen(sa, 1, errp);
    qapi_free_SocketAddress(sa);
    if (h->listen_fd < 0) {
        return;
    }
    qemu_set_fd_handler(h->listen_fd, z63_usbip_accept, NULL, h);
    error_report("Z63_USB: USB/IP server at %s (bus id " Z63_USBIP_BUSID
                 ")", h->addr);
}

/* ---------------------------------------------------------------- */
/* The host                                                          */
/* ---------------------------------------------------------------- */

static void z63_usbhost_init(Z63UsbHost *h, Z63Dwc3 *ctl, const char *addr,
                             Error **errp)
{
    h->ctl = ctl;
    for (unsigned i = 0; i < Z63_USBHOST_PIPES; i++) {
        QTAILQ_INIT(&h->pipe[i].q);
    }
    h->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  z63_usbhost_reset_tick, h);
    h->retry_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_usbhost_kick, h);
    h->kick_bh = qemu_bh_new(z63_usbhost_kick, h);
    h->listen_fd = -1;
    ctl->listener.attached = z63_usbhost_attached;
    ctl->listener.detached = z63_usbhost_detached;
    ctl->listener.ep_armed = z63_usbhost_ep_armed;
    ctl->listener.opaque = h;
    if (addr && *addr) {
        h->addr = g_strdup(addr);
        z63_usbip_listen(h, errp);
    }
}

#endif /* HW_ARM_NIKON_Z63_USBHOST_H */
