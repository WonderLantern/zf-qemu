/*
 * Nikon Z 6III -- the HDMAC: the DMA that feeds the serial buses.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 0x0e100000..0x0e12ffff was mapped as plain storage, EMIO-E1, with a
 * note that what the three banks are was not established. ../z9-qemu
 * (WonderLantern, GPL-2.0-or-later; peripheral24_word, pdma_*,
 * serial_enable in qemu/nikon-z9-lab.c) identifies them on the Z 9 as three
 * Milbeaut HDMACs of eight channels -- a global word at +0x00 of each bank
 * (bit 31 enables the controller) and a channel every 0x20 from +0x100 --
 * which the serial driver pairs with a bus's FIFO for DMA-mode transfers.
 * It is the reference for this file; each piece was re-read here:
 *
 *   The serial channel table at 0xa02813708 (232 bytes a channel) names a
 *   TX and an RX HDMAC device for instances 3, 4 and 7 (+0x10 and +0x20):
 *   0x0e100100/120, 0x0e100140/160, 0x0e100180/1a0. BVRPRO (task 41, the
 *   body VR unit) transfers on instance 7 in DMA mode -- sub_4115AFFC ->
 *   sub_40CA60E4 -> two sub_40C168BC (arm the channels: word 0 bit 31, the
 *   word-1 interrupt enables) and sub_40C147F0 (+0x0c bit 10, enable) --
 *   and then waits on an event the channel interrupt sets.
 *   Read back from a running machine after one such transfer:
 *
 *     ch 4 (TX)  A 0x9200004b  B 0x25180000  +8 0x404  src 0x43d488c0
 *                dst 0x0e700700 (instance 7's TX FIFO)
 *     ch 5 (RX)  A 0x9100004b  B 0x26180000  +8 0x404  src 0x0e700704
 *                (its RX FIFO)  dst 0x4bd45940
 *
 *   which is z9's reading exactly: A = enable (31), request select
 *   (28..24), transfer count minus one (15..0); B = mode 2, width 16 bits,
 *   fixed destination (TX, 0x25000000) or fixed source (RX, 0x26000000),
 *   error and completion interrupt enables (20, 19), stop status (18..16).
 *   Buffers are bus addresses through NoC group 0 (nikon-z63-noc.h): reads
 *   on even windows from 0x40000000, writes on odd ones from 0x48000000,
 *   both onto DRAM 0xa00000000 here -- 0x43d488c0 is 0xa03d488c0, the
 *   request's TX buffer, and 0x4bd45940 is 0xa03d45940, its RX buffer.
 *
 * What moves the words is the serial model (nikon-z63-sio.h): when an
 * instance is enabled in DMA mode it looks here for the armed pair whose
 * fixed ends are its FIFOs, then refills its TX FIFO from memory and
 * drains its RX FIFO to memory as its peer clocks. This file keeps the
 * channels' count and status, and their interrupts: completion clears
 * enable, sets stop status 5 and raises the line while completion
 * interrupts are enabled (z9: "SS=5 is success; the ISR clears SS with
 * RMW"). A guest write can clear stop status but never set it.
 *
 * DECLARED CHOICES: only the shape above is linked (one transfer, 16-bit
 * units, fixed FIFO ends, no block or beat counts); anything else is
 * refused with its values, and the transfer then waits as it did before.
 * Every other offset in the three banks is still storage, as EMIO-E1 was.
 */

#ifndef HW_ARM_NIKON_Z63_HDMAC_H
#define HW_ARM_NIKON_Z63_HDMAC_H

#define Z63_HDMAC_BASE       0x0e100000ULL
#define Z63_HDMAC_SIZE       0x30000
#define Z63_HDMAC_CHANS      24
#define Z63_HDMAC_NOC_GROUP  0

#define Z63_HDMAC_A_EB       (1u << 31)
#define Z63_HDMAC_B_CI       (1u << 19)
#define Z63_HDMAC_B_SS       (7u << 16)

typedef struct Z63Hdmac {
    MemoryRegion mr;
    uint32_t *words;                    /* the board's storage for the band */
    qemu_irq irq[Z63_HDMAC_CHANS];
    Z63Noc *noc;
} Z63Hdmac;

static uint32_t *z63_hdmac_ch(Z63Hdmac *h, unsigned c)
{
    return &h->words[((c / 8) * 0x10000 + 0x100 + (c % 8) * 0x20) / 4];
}

static bool z63_hdmac_reg(hwaddr addr, unsigned *c, unsigned *word)
{
    unsigned off = addr & 0xffff;

    if (addr >= Z63_HDMAC_SIZE || off < 0x100 || off >= 0x200) {
        return false;
    }
    *c = (addr >> 16) * 8 + (off - 0x100) / 0x20;
    *word = (off & 0x1f) / 4;
    return true;
}

static void z63_hdmac_irq(Z63Hdmac *h, unsigned c)
{
    uint32_t b = z63_hdmac_ch(h, c)[1];

    qemu_set_irq(h->irq[c], (b & Z63_HDMAC_B_CI) &&
                            ((b & Z63_HDMAC_B_SS) >> 16) == 5);
}

/* `remaining` units are left: the count field shows remaining - 1, and at
 * zero the channel finishes. */
static void z63_hdmac_progress(Z63Hdmac *h, unsigned c, unsigned remaining)
{
    uint32_t *r = z63_hdmac_ch(h, c);

    r[0] = (r[0] & ~0xffffu) | ((remaining - 1) & 0xffff);
    if (!remaining) {
        r[0] &= ~Z63_HDMAC_A_EB;
        r[1] = (r[1] & ~Z63_HDMAC_B_SS) | (5u << 16);
    }
    z63_hdmac_irq(h, c);
}

/* A buffer bus address to DRAM, through group 0. */
static bool z63_hdmac_bus(Z63Hdmac *h, uint32_t bus, uint64_t len, bool write,
                          hwaddr *pa)
{
    return z63_noc_translate(h->noc, Z63_HDMAC_NOC_GROUP, write, bus, len, pa) &&
           *pa >= Z63_DRAM_BASE && *pa + len <= Z63_DRAM_BASE + Z63_DRAM_SIZE;
}

/*
 * The armed TX/RX pair for a serial FIFO at bus address `fifo`: TX writes
 * the fixed address fifo, RX reads the fixed address fifo + 4. On success
 * fills the channels, the unit count and both DRAM addresses; otherwise
 * returns why not.
 */
static const char *z63_hdmac_pair(Z63Hdmac *h, uint32_t fifo, unsigned *tx,
                                  unsigned *rx, unsigned *units,
                                  hwaddr *src, hwaddr *dst)
{
    int t = -1, r = -1;
    uint32_t *a, *b;

    for (unsigned c = 0; c < Z63_HDMAC_CHANS; c++) {
        uint32_t *w = z63_hdmac_ch(h, c);

        if (!(w[0] & Z63_HDMAC_A_EB)) {
            continue;
        }
        if (w[6] == fifo) {
            if (t >= 0) {
                return "two armed channels write the TX FIFO";
            }
            t = c;
        }
        if (w[4] == fifo + 4) {
            if (r >= 0) {
                return "two armed channels read the RX FIFO";
            }
            r = c;
        }
    }
    if (t < 0 || r < 0) {
        return "no armed TX/RX channel pair on the FIFOs";
    }
    a = z63_hdmac_ch(h, t);
    b = z63_hdmac_ch(h, r);
    if ((a[0] & 0x40ff0000) || (b[0] & 0x40ff0000) ||
        (a[1] & ~0x001f0000u) != 0x25000000 ||
        (b[1] & ~0x001f0000u) != 0x26000000 ||
        (a[2] & ~0x404u) || (b[2] & ~0x404u) || a[3] || b[3] ||
        (a[0] & 0xffff) != (b[0] & 0xffff) ||
        !(h->words[(t / 8) * 0x10000 / 4] & 0x80000000u) ||
        !(h->words[(r / 8) * 0x10000 / 4] & 0x80000000u)) {
        return "a channel pair outside the one shape modelled";
    }
    *units = (a[0] & 0xffff) + 1;
    if (!z63_hdmac_bus(h, a[4], *units * 2, false, src) ||
        !z63_hdmac_bus(h, b[6], *units * 2, true, dst)) {
        static char why[96];

        snprintf(why, sizeof(why), "a buffer NoC group 0 does not put in "
                 "DRAM (source bus 0x%08x, destination bus 0x%08x)", a[4],
                 b[6]);
        return why;
    }
    *tx = t;
    *rx = r;
    return NULL;
}

static uint64_t z63_hdmac_read(void *opaque, hwaddr addr, unsigned size)
{
    Z63Hdmac *h = opaque;

    if (size != 4 || (addr & 3)) {
        z63_stop_read("HDMAC", Z63_HDMAC_BASE + addr, size);
        return 0;
    }
    return h->words[addr / 4];
}

static void z63_hdmac_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    Z63Hdmac *h = opaque;
    unsigned c, word;

    if (size != 4 || (addr & 3)) {
        z63_stop_write("HDMAC", Z63_HDMAC_BASE + addr, size, value);
        return;
    }
    if (z63_hdmac_reg(addr, &c, &word) && word == 1) {
        /* Stop status is the device's: a write can only clear it. */
        value = (value & ~Z63_HDMAC_B_SS) |
                (h->words[addr / 4] & value & Z63_HDMAC_B_SS);
    }
    h->words[addr / 4] = value;
    if (z63_hdmac_reg(addr, &c, &word) && word <= 1) {
        z63_hdmac_irq(h, c);
    }
}

static const MemoryRegionOps z63_hdmac_ops = {
    .read = z63_hdmac_read,
    .write = z63_hdmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static void z63_hdmac_init(Object *owner, MemoryRegion *sysmem,
                           DeviceState *gic, Z63Hdmac *h, uint32_t *words,
                           Z63Noc *noc)
{
    h->words = words;
    h->noc = noc;
    for (unsigned c = 0; c < Z63_HDMAC_CHANS; c++) {
        h->irq[c] = qdev_get_gpio_in(gic, Z63_GIC_SPI(Z63_SPI_HDMAC0_0 + c));
    }
    memory_region_init_io(&h->mr, owner, &z63_hdmac_ops, h, "HDMAC",
                          Z63_HDMAC_SIZE);
    memory_region_add_subregion(sysmem, Z63_HDMAC_BASE, &h->mr);
}

#endif /* HW_ARM_NIKON_Z63_HDMAC_H */
