/*
 * Nikon Z 6III -- the rear monitor's touch panel controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The driver is 0x40cc1aa0-0x40cc3300; its readings go to the touch input
 * service TPIS (0x40d2bf70.., objects TPISFLG/TPISMTX/TPISCYC1/2, which
 * turns points into TouchOn/Slide/Flick/Pinch/...). Board wiring, from
 * the driver's init sub_40CC20A8 and the runtime tables:
 *
 *   I2C          handle sub_40C00460(0) = 0x00000001 -> bank 0 (0x0e005000);
 *                slave 0x55 (normal mode; 0x41 is a flash-update mode the
 *                service menu uses, not modelled)
 *   interrupt    resource 0x2d, pin descriptor 0xa02809300 = {0x2d, port
 *                3, bit 14}: EXT_INT_14, installed mode 0 with config word
 *                0 -- the edge detector's falling edge (nikon-z63-extint.h)
 *                -- callback 0x40cc21c8, which sets flag bit 0 of 0xa030582e8
 *   power        resource 0x8c, {0x8c, port 9, bit 15}: an output, 1 = on
 *                (the task sets it at start-up; sub_40CC27A0 and the power
 *                hook sub_40CC21F8 clear it; sub_40CC2660 sets it)
 *
 * The protocol (register addresses are one byte; a read is "write the
 * register, repeated START, read"; message lengths from sub_40CA709C):
 *
 *   0x10, read 2     {S0 records, S1 flags}. The first interrupt after
 *                    power-on is taken only with S1 bit 7 set
 *                    (0x40cc1e64); after it the driver writes 02/01, the
 *                    four configuration pairs and a block, and is
 *                    configured. On later interrupts S1 bit 4 drops the
 *                    event and bit 3 means no finger (0x40cc1d8c/90).
 *   0x11, read 7     one record per read, up to S0 of them and at most two
 *                    (0x40cc1da4..e34): R0 bit 7 down, R0 & 0x3f the id,
 *                    x = R1 | R2 << 8, y = R3 | R4 << 8; R5/R6 unread.
 *   0x21, read 3     the firmware version, after "F6 21" (sub_40CC2944)
 *   0x01, 0x02       written 1 then 1 to start scanning ("02 01", "01 01"
 *                    after the boot status, and in sub_40CC2660), 0 and 0
 *                    to stop it (sub_40CC27A0). An interrupt while it is
 *                    stopped is taken as a new boot interrupt.
 *   anything else    written and kept
 *
 * Points are in the screen's own 1024x682 (TPIS sub_40D2D4D8 keeps
 * 0 <= x <= 1024, 0 <= y <= 682, the window at 0x42b74178), origin top
 * left. A report with no finger is the release, delivered once.
 *
 * What is modelled -- DECLARED where the part is not known: the chip is
 * not identified, so this answers the registers above and no more. 10 ms
 * after power comes on it raises the boot interrupt (S1 bit 7), and
 * repeats the edge every 100 ms until 0x10 has been read, so a line not
 * yet installed does not lose it. The interrupt line idles high and goes
 * low when there is something to read, released when it has been read
 * (0x10 with no finger, or the last record). Register 0x10 answers the
 * panel as it is at that read -- the fingers down now -- and the records
 * that follow are that snapshot. While a finger is down the line is
 * lowered again 16 ms after each report, but never while a report is
 * still unread: the driver services one edge per report (sub_40CC1BA8
 * runs well below 62.5 reports a second here), and an edge stacked on an
 * unread one made it read 0x10 a second time, find no record, and pass
 * TPIS a release in the middle of a drag -- which the menus then took as
 * a tap. A lift reports the finger's last position first, if it was not
 * yet reported (TPIS's release carries the last point it saw), then the
 * release, once. S1 bit 7 stays set
 * after the boot (a "ready" flag nothing else tests). Touches are made
 * only while registers 1 and 2 both hold 1 (scanning). The version reads
 * 1.00.00. Fingers come from QEMU's multi-touch input (input-send-event
 * "mtt", as QEMU's own windows and z63ui send them: begin/update/end per
 * slot, x and y 0..0x7fff across the picture, then a sync) or from the
 * machine property "touch": "x:y" for one, "x:y+x:y" for two (a pinch),
 * "" or "up" to lift them; settable at run time with qom-set. A touch
 * while the controller is not scanning is dropped and named once.
 */

#ifndef HW_ARM_NIKON_Z63_TOUCH_H
#define HW_ARM_NIKON_Z63_TOUCH_H

#include "ui/input.h"

#define Z63_TOUCH_BANK          0
#define Z63_TOUCH_ADDRESS       0x55
#define Z63_TOUCH_INT_LINE      14      /* EXT_INT_14, GPIO port 3 bit 14 */
#define Z63_TOUCH_POWER_PORT    9       /* GPIO port 9 bit 15 */
#define Z63_TOUCH_POWER_BIT     15
#define Z63_TOUCH_WIDTH         1024
#define Z63_TOUCH_HEIGHT        682
#define Z63_TOUCH_FINGERS       2
#define Z63_TOUCH_BOOT_NS       (10 * SCALE_MS)
#define Z63_TOUCH_RETRY_NS      (100 * SCALE_MS)
#define Z63_TOUCH_FRAME_NS      (16 * SCALE_MS)

typedef struct Z63Touch {
    Z63ExtInt *extint;
    QEMUTimer *timer;
    /* I2C */
    bool selected, receive, pointer_set;
    uint8_t pointer;
    uint8_t regs[256];
    uint8_t rx[8];
    unsigned rx_len, rx_pos;
    /* the panel */
    bool powered;
    bool boot_pending;          /* S1 bit 7 not yet read */
    bool booted;
    bool int_low;
    unsigned fingers;           /* down now */
    uint16_t x[Z63_TOUCH_FINGERS], y[Z63_TOUCH_FINGERS];
    bool release_pending;       /* one report with no finger owed */
    bool final_pending;         /* the lifted finger's last point, unreported */
    uint16_t final_x, final_y;
    unsigned report_n;          /* fingers in the report being read */
    unsigned records_left;
    uint16_t rx_x[Z63_TOUCH_FINGERS], rx_y[Z63_TOUCH_FINGERS];
    uint16_t last_x, last_y;    /* finger 0 as last reported */
    uint64_t reports, boot_edges, dropped;
    bool drop_named;            /* this touch's drop already reported */
    /* QEMU multi-touch input, per slot until the sync */
    QemuInputHandlerState *input;
    bool mt_down[Z63_TOUCH_FINGERS];
    uint16_t mt_x[Z63_TOUCH_FINGERS], mt_y[Z63_TOUCH_FINGERS];
} Z63Touch;

static bool z63_touch_scanning(Z63Touch *t)
{
    return t->booted && (t->regs[0x01] & 1) && (t->regs[0x02] & 1);
}

static void z63_touch_int(Z63Touch *t, bool low)
{
    t->int_low = low;
    z63_extint_set(t->extint, Z63_TOUCH_INT_LINE, !low);
}

/* Something to report: lower the line, unless a report is still unread
 * (its read will answer the panel as it is by then). */
static void z63_touch_report(Z63Touch *t)
{
    if (!t->int_low) {
        t->reports++;
        z63_touch_int(t, true);
    }
}

/* Register 0x10 read: the snapshot the records are read from. */
static void z63_touch_snapshot(Z63Touch *t)
{
    if (t->fingers) {
        t->report_n = t->fingers;
        for (unsigned i = 0; i < t->fingers; i++) {
            t->rx_x[i] = t->x[i];
            t->rx_y[i] = t->y[i];
        }
    } else if (t->final_pending) {
        t->final_pending = false;
        t->report_n = 1;
        t->rx_x[0] = t->final_x;
        t->rx_y[0] = t->final_y;
    } else {
        t->release_pending = false;
        t->report_n = 0;
    }
    if (t->report_n) {
        t->last_x = t->rx_x[0];
        t->last_y = t->rx_y[0];
    }
    t->records_left = t->report_n;
}

static void z63_touch_tick(void *opaque)
{
    Z63Touch *t = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!t->powered) {
        return;
    }
    if (t->boot_pending) {
        t->boot_edges++;
        if (t->int_low) {
            z63_touch_int(t, false);
        }
        z63_touch_int(t, true);
        timer_mod(t->timer, now + Z63_TOUCH_RETRY_NS);
        return;
    }
    if (!z63_touch_scanning(t)) {
        return;
    }
    if (t->fingers || t->final_pending || t->release_pending) {
        z63_touch_report(t);
        timer_mod(t->timer, now + Z63_TOUCH_FRAME_NS);
    }
}

/* GPIO port 9 bit 15, as the firmware drives it. */
static void z63_touch_power(Z63Touch *t, bool on)
{
    if (on == t->powered) {
        return;
    }
    t->powered = on;
    t->booted = false;
    t->boot_pending = on;
    t->records_left = 0;
    t->report_n = 0;
    t->final_pending = t->release_pending = false;
    t->regs[0x01] = t->regs[0x02] = 0;
    z63_touch_int(t, false);
    if (on) {
        timer_mod(t->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            Z63_TOUCH_BOOT_NS);
    } else {
        timer_del(t->timer);
    }
    error_report("Z63_TOUCH: panel power %s", on ? "on" : "off");
}

/* The fingers on the panel are now these n. */
static void z63_touch_apply(Z63Touch *t, unsigned n, const uint16_t *x,
                            const uint16_t *y)
{
    if (n && !z63_touch_scanning(t)) {
        t->dropped++;
        if (!t->drop_named) {
            t->drop_named = true;
            error_report("Z63_TOUCH: touch at %u:%u dropped -- the "
                         "controller is %s", x[0], y[0], !t->powered ? "off" :
                         !t->booted ? "starting" : "not scanning");
        }
        return;
    }
    if (!n) {
        t->drop_named = false;
        if (t->fingers) {
            t->release_pending = true;
            t->final_x = t->x[0];
            t->final_y = t->y[0];
            t->final_pending = t->x[0] != t->last_x || t->y[0] != t->last_y;
        }
    }
    t->fingers = n;
    memcpy(t->x, x, sizeof(x[0]) * n);
    memcpy(t->y, y, sizeof(y[0]) * n);
    if (z63_touch_scanning(t) && !timer_pending(t->timer)) {
        z63_touch_tick(t);
    }
}

/* "x:y", "x:y+x:y", "" or "up". */
static bool z63_touch_set(Z63Touch *t, const char *text, Error **errp)
{
    g_auto(GStrv) fingers = NULL;
    unsigned n = 0;
    uint16_t x[Z63_TOUCH_FINGERS], y[Z63_TOUCH_FINGERS];

    if (text && *text && strcmp(text, "up")) {
        fingers = g_strsplit(text, "+", -1);
        for (n = 0; fingers[n]; n++) {
            unsigned long fx, fy;
            char *end;

            if (n == Z63_TOUCH_FINGERS) {
                error_setg(errp, "touch: at most %u fingers",
                           Z63_TOUCH_FINGERS);
                return false;
            }
            fx = strtoul(fingers[n], &end, 0);
            if (*end != ':') {
                goto bad;
            }
            fy = strtoul(end + 1, &end, 0);
            if (*end || fx > Z63_TOUCH_WIDTH || fy > Z63_TOUCH_HEIGHT) {
                goto bad;
            }
            x[n] = fx;
            y[n] = fy;
        }
    }
    z63_touch_apply(t, n, x, y);
    return true;

bad:
    error_setg(errp, "touch wants x:y (0..%u, 0..%u), x:y+x:y, or up",
               Z63_TOUCH_WIDTH, Z63_TOUCH_HEIGHT);
    return false;
}

/* QEMU's multi-touch input: contacts per slot, applied at the sync. One
 * machine, so one handler; it finds its panel through this pointer. */
static Z63Touch *z63_touch_input_owner;

static void z63_touch_input_event(DeviceState *dev, QemuConsole *src,
                                  InputEvent *evt)
{
    Z63Touch *t = z63_touch_input_owner;
    InputMultiTouchEvent *mtt;

    if (!t || evt->type != INPUT_EVENT_KIND_MTT) {
        return;
    }
    mtt = evt->u.mtt.data;
    if (mtt->slot < 0 || mtt->slot >= Z63_TOUCH_FINGERS) {
        return;
    }
    switch (mtt->type) {
    case INPUT_MULTI_TOUCH_TYPE_BEGIN:
    case INPUT_MULTI_TOUCH_TYPE_UPDATE:
        t->mt_down[mtt->slot] = true;
        break;
    case INPUT_MULTI_TOUCH_TYPE_END:
    case INPUT_MULTI_TOUCH_TYPE_CANCEL:
        t->mt_down[mtt->slot] = false;
        break;
    case INPUT_MULTI_TOUCH_TYPE_DATA:
        if (mtt->axis == INPUT_AXIS_X) {
            t->mt_x[mtt->slot] = qemu_input_scale_axis(mtt->value,
                INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX, 0, Z63_TOUCH_WIDTH);
        } else if (mtt->axis == INPUT_AXIS_Y) {
            t->mt_y[mtt->slot] = qemu_input_scale_axis(mtt->value,
                INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX, 0, Z63_TOUCH_HEIGHT);
        }
        break;
    default:
        break;
    }
}

static void z63_touch_input_sync(DeviceState *dev)
{
    Z63Touch *t = z63_touch_input_owner;
    uint16_t x[Z63_TOUCH_FINGERS], y[Z63_TOUCH_FINGERS];
    unsigned n = 0;

    if (!t) {
        return;
    }
    for (unsigned i = 0; i < Z63_TOUCH_FINGERS; i++) {
        if (t->mt_down[i]) {
            x[n] = t->mt_x[i];
            y[n] = t->mt_y[i];
            n++;
        }
    }
    z63_touch_apply(t, n, x, y);
}

static const QemuInputHandler z63_touch_input = {
    .name  = "Nikon Z 6III touch panel",
    .mask  = INPUT_EVENT_MASK_MTT,
    .event = z63_touch_input_event,
    .sync  = z63_touch_input_sync,
};

/* The I2C endpoint (bank 0, slave 0x55). */
static bool z63_touch_start(Z63Touch *t, bool receive)
{
    t->selected = true;
    t->receive = receive;
    if (!receive) {
        t->pointer_set = false;
        return true;
    }
    t->rx_pos = 0;
    t->rx_len = 0;
    memset(t->rx, 0, sizeof(t->rx));
    switch (t->pointer) {
    case 0x10:
        if (t->boot_pending) {
            t->report_n = t->records_left = 0;
        } else {
            z63_touch_snapshot(t);
        }
        t->rx[0] = t->report_n;
        t->rx[1] = (t->boot_pending || t->booted) ? 0x80 : 0;
        t->rx_len = 2;
        if (t->boot_pending) {
            t->boot_pending = false;
            t->booted = true;
            timer_del(t->timer);
            error_report("Z63_TOUCH: boot status read -- the controller "
                         "is ready");
        }
        if (!t->report_n) {
            z63_touch_int(t, false);
        }
        break;
    case 0x11:
        if (t->records_left) {
            unsigned i = t->report_n - t->records_left;

            t->rx[0] = 0x80 | i;
            t->rx[1] = t->rx_x[i];
            t->rx[2] = t->rx_x[i] >> 8;
            t->rx[3] = t->rx_y[i];
            t->rx[4] = t->rx_y[i] >> 8;
            if (--t->records_left == 0) {
                t->report_n = 0;
                z63_touch_int(t, false);
            }
        }
        t->rx_len = 7;
        break;
    default:
        memcpy(t->rx, &t->regs[t->pointer],
               MIN(sizeof(t->rx), 256 - t->pointer));
        t->rx_len = sizeof(t->rx);
        break;
    }
    return true;
}

static bool z63_touch_write(Z63Touch *t, uint8_t byte)
{
    bool was = z63_touch_scanning(t);

    if (!t->pointer_set) {
        t->pointer = byte;
        t->pointer_set = true;
        return true;
    }
    t->regs[t->pointer++] = byte;
    if (was != z63_touch_scanning(t)) {
        error_report("Z63_TOUCH: scanning %s", was ? "stopped" : "started");
        if (was) {
            timer_del(t->timer);
            t->records_left = t->report_n = 0;
            z63_touch_int(t, false);
        } else if (t->fingers) {
            z63_touch_tick(t);
        }
    }
    return true;
}

static bool z63_touch_read(Z63Touch *t, uint8_t *byte)
{
    *byte = t->rx_pos < t->rx_len ? t->rx[t->rx_pos] : 0;
    t->rx_pos++;
    return true;
}

static void z63_touch_init(Z63Touch *t, Z63ExtInt *extint)
{
    t->extint = extint;
    t->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_touch_tick, t);
    t->regs[0x21] = 0x01;       /* version 1.00.00, DECLARED */
    z63_touch_int(t, false);    /* the line idles high */
    z63_touch_input_owner = t;
    t->input = qemu_input_handler_register(NULL, &z63_touch_input);
    qemu_input_handler_activate(t->input);
}

#endif /* HW_ARM_NIKON_Z63_TOUCH_H */
