/*
 * Nikon Z 6III -- programmed-I/O behaviour of the serial-bus controllers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The twelve controllers at 0x0e700000 (nikon-z63-spibus.h) have been a
 * register file: every offset the drivers name is storage, anything else
 * stops. That was enough while nothing waited on a transfer. The external
 * MCU does: it is wired to instances 0 and 1, the firmware runs both in
 * PIO mode, and the MCU, not the EXPEED, supplies the clock. So the
 * FIFOs, the status word and the interrupt causes become behaviour here;
 * every other offset is still the same storage and the same tripwire.
 *
 * The behaviour follows this image's driver, which is the same driver
 * ../z9-qemu (WonderLantern, GPL-2.0-or-later) traced on the Z 9 -- its
 * serial model is the reference and is credited for it, and each register
 * was re-read here rather than taken over:
 *
 *   start (sub_40C147F0, transfer type 1, 0x40c14adc..0x40c14cdc)
 *     +0x3c &= ~1 and wait for bit 0 to read back clear (disable)
 *     +0x0c &= ~0x400 (PIO; the DMA entries set it), +0x28 = 0x1ff
 *     +0x4c low byte = RX threshold, +0x10 |= 8, +0x20 = 0x86 or 2
 *     +0x3c |= 1 (enable), +0x64 |= 1 (start, unless +0x0c bit 5)
 *   fill (sub_40C1538C)   16-bit words to +0x00 while +0x14 bit 4 is clear
 *   drain (sub_40C15598)  words from +0x04 while +0x14 bit 5 is clear
 *   ISR (sub_40C15134 for instance 0, INTID 0xd3 = SPI 179):
 *                         reads +0x24, writes it to +0x28, masks with
 *                         +0x20; bit 1 TX, bit 2 RX, bit 7 error
 *   stop (sub_40C14FB8 / 0x40c14d68): +0x20 = 0, +0x64 &= ~1, +0x3c &= ~1
 *
 * So, as in z9's model: +0x14 reads 0x20 when the RX FIFO is empty, 0x10
 * when the TX FIFO is full and 0x04 when it is empty; +0x24 latches cause
 * bit 0 (TX empty), 1 (TX at or below the +0x4c byte-1 threshold) and 2 (RX
 * at or above the byte-0 threshold, at least one word) on a false->true
 * change, and +0x28 clears them; the line is SPI 179 + n, high while a
 * latched cause is unmasked in +0x20.
 *
 * DECLARED CHOICES. +0x48 (FIFO depth, read once per instance at boot by
 * the loop at 40c14420 into the driver's config) answers 64, the depth z9
 * provisioned -- not a measurement; it used to read 0, which left the
 * drivers' thresholds at 0. Words are moved only by an attached peer
 * (z63_sio_clock); an instance without one keeps its TX words, which is
 * what an unclocked slave does -- and says so once per instance, because
 * a task waiting on such a transfer is otherwise invisible.
 *
 * DMA mode (+0x0c bit 10 at enable, sub_40C147F0's transfer types 2 and
 * 4) runs through the same FIFOs: at enable the instance looks up the
 * armed HDMAC pair whose fixed ends are its FIFOs (nikon-z63-hdmac.h), then
 * the TX FIFO is refilled from memory and the RX FIFO drained to memory as
 * the peer clocks, each channel's count and completion kept by the HDMAC.
 * That is ../z9-qemu's serial_enable / serial_refill / its RX path. With
 * no peer the words wait, as in PIO; with no pair that fits, the enable is
 * reported once with the reason and the transfer waits.
 *
 * Write-only sinks
 * ----------------
 * Some instances drive a device that only listens. Instance 9 is the rear
 * panel's: TFTEVFSR (task 11) sends it one 9-bit word at a time in
 * interrupt PIO (transfer type 1, +0x10 word size 9, +0x20 = 0x84), the
 * first one 0x036 -- MIPI DCS set_address_mode, with the D/C bit (bit 8)
 * clear for a command -- and waits for the word that a full-duplex shift
 * brings back, which it then discards. Unclocked, that task waited for
 * ever. A sink clocks such an instance: every word out is logged (the first
 * Z63_SIO_SINK_LOG of them) and a zero word comes back in its place.
 *
 * This is ../z9-qemu's WritePeer (tools/service_serial8.py, channels 8 and
 * 9 on the Z 9: "dummy RX only to complete a physical full-duplex
 * exchange"), moved into the board. DECLARED CHOICES: the zero RX word,
 * and the time a word takes (Z63_SIO_SINK_NS). Nothing here claims the
 * panel accepted anything; a driver that read the RX word as data would be
 * reading an invented zero, which is why sinks are only attached where the
 * request has been checked to be write-only (-machine sio-sink=, default
 * "9:8" -- 8 is the image sensor's control port, see nikon-z63.c -- several
 * as "9:8:7"; an empty list attaches none).
 */

#ifndef HW_ARM_NIKON_Z63_SIO_H
#define HW_ARM_NIKON_Z63_SIO_H

#define Z63_SIO_FIFO    64
#define Z63_SIO_DMA_REPORTS 8    /* DMA enables named per instance */

typedef struct Z63SioPio {
    bool active;
    uint16_t tx[Z63_SIO_FIFO], rx[Z63_SIO_FIFO];
    unsigned tx_head, tx_queued, rx_head, rx_queued;
    uint32_t conditions, pending;
    /* DMA mode: the linked HDMAC pair and how far it has got. */
    bool dma;
    unsigned txch, rxch, units, sent, received;
    hwaddr src, dst;
} Z63SioPio;

typedef struct Z63Sio {
    MemoryRegion mr;
    Z63RegWindow win;                   /* storage and tripwire */
    Z63SioPio pio[Z63_SPI_INST];
    qemu_irq irq[Z63_SPI_INST];
    unsigned dma_reported[Z63_SPI_INST];  /* DMA enables reported so far */
    unsigned dma_refused[Z63_SPI_INST];   /* ... and refusals */
    Z63Hdmac *hdmac;                    /* for DMA mode */
    bool idle_reported[Z63_SPI_INST];
    /* The peer that clocks an instance, if any: told when words wait. */
    void (*kick[Z63_SPI_INST])(void *opaque, unsigned inst);
    void *kick_opaque[Z63_SPI_INST];
    /* Write-only sinks (above). */
    QEMUTimer *sink_timer[Z63_SPI_INST];
    unsigned sink_words[Z63_SPI_INST];
} Z63Sio;

#define Z63_SIO_SINK_NS     2000    /* per word, a choice */
#define Z63_SIO_SINK_LOG    64

static uint32_t *z63_sio_reg(Z63Sio *s, unsigned inst, unsigned off)
{
    return &s->win.words[(inst * Z63_SPI_STRIDE + off) / 4];
}

static void z63_sio_update(Z63Sio *s, unsigned inst)
{
    Z63SioPio *p = &s->pio[inst];
    uint32_t thr = *z63_sio_reg(s, inst, 0x4c);
    unsigned rx_thr = thr & 0xff, tx_thr = (thr >> 8) & 0xff;
    uint32_t c = 0;

    if (p->active) {
        c = (!p->tx_queued ? 1 : 0) |
            (p->tx_queued <= tx_thr ? 2 : 0) |
            (p->rx_queued && p->rx_queued >= MAX(1u, rx_thr) ? 4 : 0);
    }
    p->pending |= c & ~p->conditions;
    p->conditions = c;
    qemu_set_irq(s->irq[inst], p->active &&
                 (p->pending & *z63_sio_reg(s, inst, 0x20)));
}

static void z63_sio_kick(Z63Sio *s, unsigned inst);

/* DMA mode: RX words to memory, TX words from memory, counts to the
 * HDMAC channels. */
static void z63_sio_dma_pump(Z63Sio *s, unsigned inst)
{
    Z63SioPio *p = &s->pio[inst];
    unsigned received = p->received, sent = p->sent;

    while (p->rx_queued && p->received < p->units) {
        uint8_t b[2] = { p->rx[p->rx_head], p->rx[p->rx_head] >> 8 };

        cpu_physical_memory_write(p->dst + 2 * p->received, b, 2);
        p->rx_head = (p->rx_head + 1) % Z63_SIO_FIFO;
        p->rx_queued--;
        p->received++;
    }
    while (p->tx_queued < Z63_SIO_FIFO && p->sent < p->units) {
        uint8_t b[2];

        cpu_physical_memory_read(p->src + 2 * p->sent, b, 2);
        p->tx[(p->tx_head + p->tx_queued) % Z63_SIO_FIFO] = b[0] | (b[1] << 8);
        p->tx_queued++;
        p->sent++;
    }
    if (p->sent != sent) {
        z63_hdmac_progress(s->hdmac, p->txch, p->units - p->sent);
    }
    if (p->received != received) {
        z63_hdmac_progress(s->hdmac, p->rxch, p->units - p->received);
    }
}

/* Called by a peer: exchange up to `max` words. `fn` sees each word the
 * EXPEED shifted out and returns the word shifted in. Returns the count. */
static unsigned z63_sio_clock(Z63Sio *s, unsigned inst, unsigned max,
                              uint16_t (*fn)(void *, unsigned, uint16_t),
                              void *opaque)
{
    Z63SioPio *p = &s->pio[inst];
    unsigned n = 0;

    while (p->active && n < max && p->tx_queued &&
           p->rx_queued < Z63_SIO_FIFO) {
        uint16_t out = p->tx[p->tx_head];

        p->tx_head = (p->tx_head + 1) % Z63_SIO_FIFO;
        p->tx_queued--;
        p->rx[(p->rx_head + p->rx_queued) % Z63_SIO_FIFO] = fn(opaque, inst, out);
        p->rx_queued++;
        n++;
    }
    if (n && p->dma) {
        z63_sio_dma_pump(s, inst);
    }
    if (n) {
        z63_sio_update(s, inst);
    }
    if (n && p->dma) {
        z63_sio_kick(s, inst);          /* the rest of the transfer */
    }
    return n;
}

static void z63_sio_kick(Z63Sio *s, unsigned inst)
{
    if (!s->pio[inst].active || !s->pio[inst].tx_queued ||
        s->pio[inst].rx_queued >= Z63_SIO_FIFO) {
        return;
    }
    if (s->kick[inst]) {
        s->kick[inst](s->kick_opaque[inst], inst);
    } else if (!s->idle_reported[inst]) {
        s->idle_reported[inst] = true;
        error_report("Z63_SIO: instance %u has TX words and no peer to clock "
                     "them; the transfer waits (first word 0x%x, pc=0x%"
                     PRIx64 "); see nikon-z63-sio.h", inst,
                     s->pio[inst].tx[s->pio[inst].tx_head], z63_current_pc());
    }
}

static uint64_t z63_sio_read(void *opaque, hwaddr addr, unsigned size)
{
    Z63Sio *s = opaque;
    unsigned inst = addr / Z63_SPI_STRIDE, off = addr % Z63_SPI_STRIDE;
    Z63SioPio *p = &s->pio[inst];

    if (!z63_regwindow_has(&s->win, addr, size)) {
        return z63_regwindow_read(&s->win, addr, size);
    }
    switch (off) {
    case 0x04:
        if (p->active && p->rx_queued) {
            uint16_t v = p->rx[p->rx_head];

            p->rx_head = (p->rx_head + 1) % Z63_SIO_FIFO;
            p->rx_queued--;
            z63_sio_update(s, inst);
            z63_sio_kick(s, inst);
            return v;
        }
        break;
    case 0x14:
        if (p->active) {
            return (!p->rx_queued ? 0x20 : 0) |
                   (p->tx_queued == Z63_SIO_FIFO ? 0x10 : 0) |
                   (!p->tx_queued ? 0x04 : 0);
        }
        break;
    case 0x24:
        return p->pending;
    case 0x48:
        return Z63_SIO_FIFO;
    }
    return z63_regwindow_read(&s->win, addr, size);
}

static void z63_sio_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    Z63Sio *s = opaque;
    unsigned inst = addr / Z63_SPI_STRIDE, off = addr % Z63_SPI_STRIDE;
    Z63SioPio *p = &s->pio[inst];
    uint32_t old;

    if (!z63_regwindow_has(&s->win, addr, size)) {
        z63_regwindow_write(&s->win, addr, value, size);
        return;
    }
    old = *z63_sio_reg(s, inst, off);
    z63_regwindow_write(&s->win, addr, value, size);

    switch (off) {
    case 0x00:
        if (p->active && p->tx_queued < Z63_SIO_FIFO) {
            p->tx[(p->tx_head + p->tx_queued) % Z63_SIO_FIFO] = value;
            p->tx_queued++;
            z63_sio_update(s, inst);
            z63_sio_kick(s, inst);
        }
        break;
    case 0x20:
    case 0x4c:
        z63_sio_update(s, inst);
        break;
    case 0x28:
        p->pending &= ~(uint32_t)value;
        z63_sio_update(s, inst);
        break;
    case 0x3c:
        if ((value & 1) && !(old & 1)) {
            if (*z63_sio_reg(s, inst, 0x0c) & 0x400) {
                Z63SioPio next = { .active = true, .dma = true };
                const char *why = s->hdmac ? NULL : "no HDMAC";

                if (!why) {
                    why = z63_hdmac_pair(s->hdmac,
                                         Z63_SPI_BASE + inst * Z63_SPI_STRIDE,
                                         &next.txch, &next.rxch, &next.units,
                                         &next.src, &next.dst);
                }
                if (why) {
                    if (s->dma_refused[inst] < Z63_SIO_DMA_REPORTS) {
                        s->dma_refused[inst]++;
                        error_report("Z63_SIO: instance %u enabled in DMA "
                                     "mode, but %s -- the transfer waits "
                                     "(pc=0x%" PRIx64 "); see "
                                     "nikon-z63-sio.h", inst, why,
                                     z63_current_pc());
                    }
                } else {
                    *p = next;
                    z63_sio_dma_pump(s, inst);
                    if (s->dma_reported[inst] < Z63_SIO_DMA_REPORTS) {
                        s->dma_reported[inst]++;
                        error_report("Z63_SIO: instance %u in DMA mode: "
                                     "%u words, HDMAC channels %u/%u, "
                                     "buffers 0x%" HWADDR_PRIx " / 0x%"
                                     HWADDR_PRIx, inst, next.units,
                                     next.txch, next.rxch, next.src,
                                     next.dst);
                    }
                }
            } else {
                memset(p, 0, sizeof(*p));
                p->active = true;
            }
        } else if (!(value & 1) && (old & 1)) {
            if (p->dma && p->received < p->units) {
                /* Cancelled: no completion, the channels stop (z9's
                 * serial_abort). */
                z63_hdmac_ch(s->hdmac, p->txch)[0] &= ~Z63_HDMAC_A_EB;
                z63_hdmac_ch(s->hdmac, p->rxch)[0] &= ~Z63_HDMAC_A_EB;
            }
            memset(p, 0, sizeof(*p));
        }
        z63_sio_update(s, inst);
        z63_sio_kick(s, inst);
        break;
    }
}

static uint16_t z63_sio_sink_word(void *opaque, unsigned inst, uint16_t out)
{
    Z63Sio *s = opaque;
    unsigned n = s->sink_words[inst]++;

    if (n < Z63_SIO_SINK_LOG) {
        error_report("Z63_SIO: sink %u word %u: 0x%03x", inst, n, out);
    } else if (n == Z63_SIO_SINK_LOG) {
        error_report("Z63_SIO: sink %u: further words not logged", inst);
    }
    return 0;
}

static void z63_sio_sink_clock(void *opaque)
{
    Z63Sio *s = opaque;

    for (unsigned i = 0; i < Z63_SPI_INST; i++) {
        if (s->sink_timer[i] && !timer_pending(s->sink_timer[i]) &&
            s->pio[i].tx_queued) {
            z63_sio_clock(s, i, Z63_SIO_FIFO, z63_sio_sink_word, s);
        }
    }
}

static void z63_sio_sink_kick(void *opaque, unsigned inst)
{
    Z63Sio *s = opaque;
    unsigned words = s->pio[inst].tx_queued;

    if (!timer_pending(s->sink_timer[inst])) {
        timer_mod(s->sink_timer[inst], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)words * Z63_SIO_SINK_NS);
    }
}

/* Attach write-only sinks to the instances named in `list` ("9", "8:9" --
 * a colon, since -machine splits on commas; empty for none). */
static void z63_sio_sinks(Z63Sio *s, const char *list)
{
    g_auto(GStrv) parts = g_strsplit_set(list ? list : "", ",:", -1);

    for (char **p = parts; *p; p++) {
        unsigned long inst;

        if (!**p) {
            continue;
        }
        if (qemu_strtoul(*p, NULL, 0, &inst) || inst >= Z63_SPI_INST ||
            s->kick[inst]) {
            error_report("Z63_SIO: sio-sink: '%s' is not a free serial "
                         "instance", *p);
            exit(1);
        }
        s->sink_timer[inst] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           z63_sio_sink_clock, s);
        s->kick[inst] = z63_sio_sink_kick;
        s->kick_opaque[inst] = s;
        error_report("Z63_SIO: write-only sink on serial instance %lu -- "
                     "words out are logged, zero words come back; see "
                     "nikon-z63-sio.h (-machine sio-sink=)", inst);
    }
}

static const MemoryRegionOps z63_sio_ops = {
    .read = z63_sio_read,
    .write = z63_sio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void z63_sio_init(Object *owner, MemoryRegion *sysmem,
                         DeviceState *gic, Z63Sio *s)
{
    Z63RegWindow *w = &s->win;
    unsigned size = (unsigned)Z63_SPI_INST * Z63_SPI_STRIDE;

    /* The storage half, exactly as z63_regwindow_init_strided builds it,
     * but behind this file's ops instead of the plain ones. */
    w->name = "SPI-BUS";
    w->base = Z63_SPI_BASE;
    w->size = size;
    w->stride = Z63_SPI_STRIDE;
    w->words = g_new0(uint32_t, size / 4);
    w->modelled = bitmap_new(Z63_SPI_STRIDE / 4);
    for (size_t i = 0; i < ARRAY_SIZE(z63_spibus_offsets); i++) {
        set_bit(z63_spibus_offsets[i] / 4, w->modelled);
    }
    for (unsigned i = 0; i < Z63_SPI_INST; i++) {
        s->irq[i] = qdev_get_gpio_in(gic, Z63_GIC_SPI(Z63_SPI_SPI_0 + i));
    }
    memory_region_init_io(&s->mr, owner, &z63_sio_ops, s, "SPI-BUS", size);
    memory_region_add_subregion(sysmem, Z63_SPI_BASE, &s->mr);
}

#endif /* HW_ARM_NIKON_Z63_SIO_H */
