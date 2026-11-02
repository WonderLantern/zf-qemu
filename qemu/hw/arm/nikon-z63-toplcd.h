/*
 * Nikon Z 6III -- the top LCD (the control panel on the right shoulder),
 * as the body MCU drives it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * What the panel shows is decided on the EXPEED, which sends it to the MCU
 * as state, not pixels: family 8 kind 4, 192 bytes, with family 8 kind 1
 * (20 bytes of flags) beside it. The MCU draws that itself: its display
 * task (sub_8032870) keeps the 192 bytes plus two (0x2001A9FA, the last
 * ones at +194), renders a 2048-byte frame from them (sub_8030E0C, into
 * +388) and hands the frame to sub_8032554, which compares it line by line
 * with the frame last sent (0x2001BC94) and sends only the lines that
 * changed. The bytes it sends are the write protocol of a Sharp memory-in-
 * pixel LCD:
 *
 *   0x80                      mode: M0 (data update), MSB first on the wire
 *   addr, 16 bytes, 0x00      a line: gate address, 128 pixels, dummy
 *   ...                       up to 16 lines a packet
 *   0x00                      the trailing dummy
 *
 * and at power-on (sub_8032058) 0x20 0x00 -- M2, all clear. The frame is
 * 128 lines of 16 bytes and the address of line i is table 0x8058A10[i]:
 * 0x01, 0xfe, 0x7e, ... -- 128, 127, 126, ... with the bits reversed, as
 * the panel takes its address LSB first while this SPI sends MSB first.
 * So the panel is 128 x 128, and the MCU's row 0 is the panel's line 128.
 *
 * The bytes go out on SPI2 by DMA (HAL_SPI_Transmit_DMA = sub_804D580,
 * called from sub_8032704; DMA1 stream 4). The packet is framed by the
 * chip select, which on this panel is active high (SCS): the begin hook
 * sub_8032350 raises it and the end hook sub_8032360, run from the
 * stream's TC, drops it. DISP is the panel's display-on input, raised by
 * sub_8031FA8 once the clear has gone out. Both are pins the MCU looks up
 * through its pin tables (sub_80321C8, ids 4 and 72); the board says which
 * GPIOs they are (nikon-z63.c, Z63_TOPLCD_SCS / _DISP).
 *
 * One more line, and it is not the panel's. At power-on sub_8032058 pulses
 * PD11 high for 1 ms (id 59), sends the clear, and arms PD1 (id 49) as an
 * input with a pull-up, interrupting on a rising edge (sub_8032690:
 * HAL_GPIO_Init(GPIOD, pin 1, IT_RISING, PULLUP)). That edge (sub_8032908,
 * event 0x80) is waited out for 1 ms and then sets 0x2001BB84 (sub_8032264),
 * and until it is set the packets sub_8032554 builds are queued but never
 * sent -- after which the display task, its queue full, draws nothing more.
 * A memory LCD has no output, so what drives PD1 is not known; a pull-up
 * and a rising edge read like an open-drain "ready" or power-good from the
 * panel's supply. DECLARED: PD11's pulse pulls PD1 low, and it is let go
 * Z63_TOPLCD_READY_NS after the pulse ends.
 *
 * Modelled: SCS, DISP, the three modes and the panel's memory; what the
 * panel shows is published to -machine toplcd-live=<file>, in the same
 * format as scanout-live (nikon-z63-scanout.h): 128 x 128, 0x00ffffff for
 * a white pixel (a 1 bit, which is what all clear writes) and 0 for black,
 * in the panel's own order -- line 1 first, the first pixel shifted in at
 * the left. How the panel sits in the body is the viewer's business.
 * Not modelled: VCOM (M1 or EXTCOMIN; a DC-balanced liquid crystal has no
 * picture to show for it), timing limits, and the reflective look.
 */

#ifndef HW_ARM_NIKON_Z63_TOPLCD_H
#define HW_ARM_NIKON_Z63_TOPLCD_H
#include "hw/arm/nikon-zf-toplcd.h"

#define Z63_TOPLCD_W        128
#define Z63_TOPLCD_H        128
#define Z63_TOPLCD_STRIDE   (Z63_TOPLCD_W / 8)
#define Z63_TOPLCD_READY_NS (5 * SCALE_MS)      /* declared, see above */

typedef struct Z63TopLcd {
    bool scs, disp;
    bool zf_segment;
    ZfSegmentPanel segment;
    enum {
        Z63_TOPLCD_MODE,            /* the first byte after SCS rises */
        Z63_TOPLCD_ADDR,
        Z63_TOPLCD_DATA,
        Z63_TOPLCD_DUMMY,
        Z63_TOPLCD_IDLE,            /* the rest of the packet is dummy */
    } state;
    unsigned line, at;
    uint8_t row[Z63_TOPLCD_STRIDE];
    uint8_t mem[Z63_TOPLCD_H][Z63_TOPLCD_STRIDE];   /* 1 = white */
    bool dirty;
    Z63Stm32Gpio *ready_gpio;       /* the ready line, an MCU input */
    unsigned ready_pin;
    QEMUTimer *ready_timer;
    Z63LiveHeader *live;
    uint64_t packets, lines, clears;
    unsigned named;                 /* oddities reported so far */
    unsigned named_cap;             /* cap for named; board property
                                     * zf-toplcd-named-cap (default
                                     * Z63_TOPLCD_NAMED). Raising it logs
                                     * every dropped byte -- the capture
                                     * side of the Zf top-LCD protocol
                                     * decode (zf-toplcd-scs-gap note). */
} Z63TopLcd;

#define Z63_TOPLCD_NAMED    8

static void z63_toplcd_odd(Z63TopLcd *t, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

static void z63_toplcd_odd(Z63TopLcd *t, const char *fmt, ...)
{
    va_list ap;
    g_autofree char *msg = NULL;

    if (t->named >= t->named_cap) {
        return;
    }
    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    error_report("Z63_TOPLCD: %s%s", msg, ++t->named == t->named_cap ?
                 " (further oddities not named)" : "");
}

/* What the panel shows, to the live file, if it changed. */
static void zf_toplcd_publish(Z63TopLcd *t)
{
    Z63LiveHeader *l = t->live;
    if (!l || !t->dirty) { return; }
    t->dirty = false;
    uint8_t digits[2]; bool flags[2];
    zf_segment_masks(&t->segment,digits,flags);
    uint32_t *pixels = (uint32_t *)((uint8_t *)l + Z63_LIVE_HEADER);
    z63_live_begin(l);
    l->width = ZF_TOPLCD_W; l->height = ZF_TOPLCD_H;
    for (unsigned y=0;y<ZF_TOPLCD_H;y++) {
        for (unsigned x=0;x<ZF_TOPLCD_W;x++) {
            *pixels++ = zf_segment_pixel(digits,flags,t->disp,x,y);
        }
    }
    pstrcpy(l->note,sizeof(l->note),t->disp ? "" : "DISP low");
    l->frames = t->segment.frames;
    z63_live_end(l);
}

static void z63_toplcd_publish(Z63TopLcd *t)
{
    if (t->zf_segment) { zf_toplcd_publish(t); return; }
    Z63LiveHeader *l = t->live;
    uint32_t *px;

    if (!l || !t->dirty) {
        return;
    }
    t->dirty = false;
    px = (uint32_t *)((uint8_t *)l + Z63_LIVE_HEADER);
    z63_live_begin(l);
    l->width = Z63_TOPLCD_W;
    l->height = Z63_TOPLCD_H;
    for (unsigned y = 0; y < Z63_TOPLCD_H; y++) {
        for (unsigned x = 0; x < Z63_TOPLCD_W; x++) {
            bool white = (t->mem[y][x / 8] >> (7 - x % 8)) & 1;

            *px++ = white ? 0x00ffffff : 0;
        }
    }
    if (t->disp) {
        l->note[0] = 0;
    } else {
        pstrcpy(l->note, sizeof(l->note), "DISP is low: the panel is "
                "blank");
    }
    l->frames = t->packets;
    z63_live_end(l);
}

/* A byte shifted in. Only while SCS is high does the panel listen. */
static void z63_toplcd_byte(void *opaque, unsigned inst, uint16_t word)
{
    Z63TopLcd *t = opaque;
    uint8_t b = word;
    if (t->zf_segment) {
        zf_segment_byte(&t->segment,b);
        t->dirty = true;
        return;
    }

    if (!t->scs) {
        z63_toplcd_odd(t, "0x%02x arrived with SCS low and was dropped",
                       b);
        return;
    }
    switch (t->state) {
    case Z63_TOPLCD_MODE:
        t->packets++;
        if (b & 0x80) {                                 /* M0 */
            t->state = Z63_TOPLCD_ADDR;
        } else {
            if (b & 0x20) {                             /* M2 */
                memset(t->mem, 0xff, sizeof(t->mem));
                t->clears++;
                t->dirty = true;
            }
            t->state = Z63_TOPLCD_IDLE;
        }
        break;
    case Z63_TOPLCD_ADDR:
        /* The address goes LSB first: reversed in an MSB-first byte. */
        t->line = revbit8(b);
        if (t->line == 0) {                             /* the trailer */
            t->state = Z63_TOPLCD_IDLE;
        } else if (t->line > Z63_TOPLCD_H) {
            z63_toplcd_odd(t, "gate line %u (byte 0x%02x) is past the "
                           "panel's %u", t->line, b, Z63_TOPLCD_H);
            t->state = Z63_TOPLCD_IDLE;
        } else {
            t->at = 0;
            t->state = Z63_TOPLCD_DATA;
        }
        break;
    case Z63_TOPLCD_DATA:
        t->row[t->at++] = b;
        if (t->at == Z63_TOPLCD_STRIDE) {
            if (memcmp(t->mem[t->line - 1], t->row, sizeof(t->row))) {
                memcpy(t->mem[t->line - 1], t->row, sizeof(t->row));
                t->dirty = true;
            }
            t->lines++;
            t->state = Z63_TOPLCD_DUMMY;
        }
        break;
    case Z63_TOPLCD_DUMMY:
        t->state = Z63_TOPLCD_ADDR;
        break;
    case Z63_TOPLCD_IDLE:
        break;
    }
}

static void z63_toplcd_scs(Z63TopLcd *t, bool level)
{
    if (t->zf_segment) {
        zf_segment_cs(&t->segment,level);
        t->scs = level; t->dirty = true;
        zf_toplcd_publish(t); return;
    }
    if (level == t->scs) {
        return;
    }
    t->scs = level;
    if (level) {
        t->state = Z63_TOPLCD_MODE;
        return;
    }
    if (t->state == Z63_TOPLCD_DATA) {
        z63_toplcd_odd(t, "SCS fell %u bytes into line %u", t->at, t->line);
    }
    /* The end of a packet: what the panel now shows. */
    if (t->packets == 1) {
        error_report("Z63_TOPLCD: the first packet (%" PRIu64 " clear, "
                     "%" PRIu64 " lines)", t->clears, t->lines);
    }
    z63_toplcd_publish(t);
}

static void z63_toplcd_disp(Z63TopLcd *t, bool level)
{
    if (t->zf_segment) {
        t->disp = level; t->dirty = true;
        zf_toplcd_publish(t); return;
    }
    if (level == t->disp) {
        return;
    }
    t->disp = level;
    error_report("Z63_TOPLCD: DISP %s after %" PRIu64 " packets",
                 level ? "high: the panel shows its memory"
                       : "low: the panel is blank", t->packets);
    t->dirty = true;
    z63_toplcd_publish(t);
}

/* PD11, the start pulse: the ready line goes low with it and comes back
 * up a little after it (declared, see above). */
static void z63_toplcd_start(Z63TopLcd *t, bool level)
{
    if (t->zf_segment) { return; } /* PD11/PD1 ready shim belongs to Z63. */
    if (level) {
        timer_del(t->ready_timer);
        z63_stm32_gpio_drive(t->ready_gpio, t->ready_pin, false);
    } else {
        timer_mod(t->ready_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  Z63_TOPLCD_READY_NS);
    }
}

static void z63_toplcd_ready(void *opaque)
{
    Z63TopLcd *t = opaque;

    error_report("Z63_TOPLCD: ready line P%c%u released (declared: %u ms "
                 "after the start pulse)", 'A' + t->ready_gpio->port,
                 t->ready_pin, (unsigned)(Z63_TOPLCD_READY_NS / SCALE_MS));
    z63_stm32_gpio_drive(t->ready_gpio, t->ready_pin, true);
}

/* The panel at power-on: memory undefined (shown white; the MCU clears it
 * before raising DISP), SCS and DISP low, the ready line left to its
 * pull-up. */
static void z63_toplcd_init(Z63TopLcd *t, const char *live,
                            Z63Stm32Gpio *ready, unsigned ready_pin)
{
    if (t->zf_segment) {
        memset(&t->segment,0,sizeof(t->segment));
        t->segment.cs_high = t->scs; /* MCU GPIO ODR initially low. */
        if (live && *live) {
            t->live = z63_live_open(live,ZF_TOPLCD_W*ZF_TOPLCD_H*4,"toplcd-live");
            t->dirty = true; zf_toplcd_publish(t);
        }
        return;
    }
    memset(t->mem, 0xff, sizeof(t->mem));
    t->ready_gpio = ready;
    t->ready_pin = ready_pin;
    t->ready_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_toplcd_ready, t);
    if (live && *live) {
        t->live = z63_live_open(live, Z63_TOPLCD_W * Z63_TOPLCD_H * 4,
                                "toplcd-live");
        t->dirty = true;
        z63_toplcd_publish(t);
    }
}

#endif
