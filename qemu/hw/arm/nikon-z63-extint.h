/*
 * Nikon Z 6III -- the external-interrupt controller at 0x0c86e000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The board's AUX window (nikon-z63-aux.h) has covered these two 0x1000
 * banks as plain storage. Its first seven words are not a register file but
 * the edge detector behind EXT_INT_0..17 (SPI 23..40), and an edge detector
 * is behaviour, so per nikon-z63-regwindow.h it gets its own ops, overlaid
 * on +0x00..+0x1c of each bank. The rest of the window stays AUX storage.
 *
 * What the firmware says, function by function (this image, not vr2070):
 *
 *   sub_40C15DFC   installs a line: +0x14 bit = 1 for mode 1, 0 for mode 0,
 *                  and for mode 2 the opposite of the pin's present level;
 *                  +0x18 bit cleared when the config's second word is 1,
 *                  set otherwise. Stores the callback and mode.
 *   sub_40C16080   +0x00 bit set = masked, clear = unmasked.
 *   sub_40C15F80   enable: +0x10 |= bit (clears a stale edge), then unmask.
 *   sub_40C16194.. the per-line ISRs (0x18 apart, INTID 55 + n): write
 *                  `bit & +0x08` to +0x10, and for mode 2 flip +0x14 to the
 *                  edge the pin has not yet made, then call the callback.
 *
 * So: +0x08 is pending (only ever read), +0x10 acknowledges (write one to
 * clear; the RMW in sub_40C15F80 reads it, and it reads 0 here), +0x00
 * masks, +0x14 picks the edge. Mode 2's flip says +0x14 = 1 waits for a
 * rising edge: it sets 1 while the pin is low. +0x18 set is taken to mean
 * edge-triggered and clear level-triggered -- an inference (mode 2's
 * edge-chasing only makes sense for edges), labelled as such; level mode
 * then holds pending while the pin is at the +0x14 level. Nothing is
 * evaluated for a line until the firmware has changed one of its bits in
 * +0x14 or +0x18: the reset values are not known, and an all-zero reset
 * read as "level, active low" would assert every idle line at once.
 *
 * Which pin feeds which line is board wiring, not a register: the runtime
 * line descriptors (0xa02dc3dd8 + 8 * (INTID - 55)) put EXT_INT_2 on GPIO
 * port 3 bit 2 and EXT_INT_3 on port 3 bit 3. Lines are driven through
 * z63_extint_set(); a line nothing drives stays low, as before.
 *
 * Nine lines a bank: the line records sub_40C15DFC installs from
 * ({enable, bank, bit, INTID, ISR, callback}, .data 0xa0280c1b8 onward,
 * 0x28 apart) put EXT_INT_0..8 at bank 0 bits 0..8 and EXT_INT_9..17 at
 * bank 1 bits 0..8 (EXT_INT_14, the touch panel's, is {1, 5, INTID 69}).
 * This model first assumed sixteen a bank, which only bank 0's lines --
 * all that had been driven -- could not show.
 */

#ifndef HW_ARM_NIKON_Z63_EXTINT_H
#define HW_ARM_NIKON_Z63_EXTINT_H

#define Z63_EXTINT_BASE     0x0c86e000ULL
#define Z63_EXTINT_BANKS    2
#define Z63_EXTINT_SPAN     0x20        /* overlaid words per bank */
#define Z63_EXTINT_LINES    18          /* EXT_INT_0..17 */
#define Z63_EXTINT_PER_BANK 9           /* bits 0..8 of each bank */

typedef struct Z63ExtInt Z63ExtInt;

typedef struct Z63ExtIntBank {
    MemoryRegion mr;
    Z63ExtInt *e;
    unsigned bank;
} Z63ExtIntBank;

struct Z63ExtInt {
    Z63ExtIntBank win[Z63_EXTINT_BANKS];
    uint32_t mask[Z63_EXTINT_BANKS];    /* +0x00 */
    uint32_t r04[Z63_EXTINT_BANKS];     /* +0x04, storage */
    uint32_t pending[Z63_EXTINT_BANKS]; /* +0x08 */
    uint32_t r0c[Z63_EXTINT_BANKS];     /* +0x0c, storage */
    uint32_t polarity[Z63_EXTINT_BANKS]; /* +0x14 */
    uint32_t edge[Z63_EXTINT_BANKS];    /* +0x18 */
    uint32_t level[Z63_EXTINT_BANKS];   /* what the pins are doing */
    uint32_t configured[Z63_EXTINT_BANKS]; /* written in +0x14/+0x18 */
    qemu_irq irq[Z63_EXTINT_LINES];
};

static void z63_extint_update(Z63ExtInt *e, unsigned bank)
{
    for (unsigned bit = 0; bit < 32; bit++) {
        unsigned line = bank * Z63_EXTINT_PER_BANK + bit;
        uint32_t m = 1u << bit;

        if (bit >= Z63_EXTINT_PER_BANK || line >= Z63_EXTINT_LINES) {
            continue;
        }
        if ((e->configured[bank] & m) && !(e->edge[bank] & m) &&
            !!(e->level[bank] & m) == !!(e->polarity[bank] & m)) {
            e->pending[bank] |= m;      /* level-triggered, still asserted */
        }
        qemu_set_irq(e->irq[line], (e->pending[bank] & ~e->mask[bank] & m) != 0);
    }
}

/* A pin feeding EXT_INT `line` changed level. */
static void z63_extint_set(Z63ExtInt *e, unsigned line, bool level)
{
    unsigned bank = line / Z63_EXTINT_PER_BANK;
    uint32_t m = 1u << (line % Z63_EXTINT_PER_BANK);
    bool was = e->level[bank] & m;

    if (was == level) {
        return;
    }
    e->level[bank] = level ? (e->level[bank] | m) : (e->level[bank] & ~m);
    if ((e->configured[bank] & m) && (e->edge[bank] & m) &&
        level == !!(e->polarity[bank] & m)) {
        e->pending[bank] |= m;          /* the edge the line waits for */
    }
    z63_extint_update(e, bank);
}

static uint64_t z63_extint_read(void *opaque, hwaddr addr, unsigned size)
{
    Z63ExtIntBank *w = opaque;
    Z63ExtInt *e = w->e;
    unsigned bank = w->bank;

    switch (addr) {
    case 0x00: return e->mask[bank];
    case 0x04: return e->r04[bank];
    case 0x08: return e->pending[bank];
    case 0x0c: return e->r0c[bank];
    case 0x10: return 0;
    case 0x14: return e->polarity[bank];
    case 0x18: return e->edge[bank];
    }
    z63_stop_read("EXTINT", Z63_EXTINT_BASE + bank * 0x1000 + addr, size);
    return 0;
}

static void z63_extint_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    Z63ExtIntBank *w = opaque;
    Z63ExtInt *e = w->e;
    unsigned bank = w->bank;

    switch (addr) {
    case 0x00: e->mask[bank] = value; break;
    case 0x04: e->r04[bank] = value; break;
    case 0x0c: e->r0c[bank] = value; break;
    case 0x10:
        e->pending[bank] &= ~(uint32_t)value;
        break;
    case 0x14:
        e->configured[bank] |= e->polarity[bank] ^ (uint32_t)value;
        e->polarity[bank] = value;
        break;
    case 0x18:
        e->configured[bank] |= e->edge[bank] ^ (uint32_t)value;
        e->edge[bank] = value;
        break;
    default:
        z63_stop_write("EXTINT", Z63_EXTINT_BASE + bank * 0x1000 + addr,
                       size, value);
        return;
    }
    z63_extint_update(e, bank);
}

static const MemoryRegionOps z63_extint_ops = {
    .read = z63_extint_read,
    .write = z63_extint_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void z63_extint_init(Object *owner, MemoryRegion *sysmem,
                            DeviceState *gic, Z63ExtInt *e)
{
    for (unsigned line = 0; line < Z63_EXTINT_LINES; line++) {
        e->irq[line] = qdev_get_gpio_in(gic,
                                        Z63_GIC_SPI(Z63_SPI_EXT_INT_0 + line));
    }
    for (unsigned bank = 0; bank < Z63_EXTINT_BANKS; bank++) {
        Z63ExtIntBank *w = &e->win[bank];

        w->e = e;
        w->bank = bank;
        memory_region_init_io(&w->mr, owner, &z63_extint_ops, w,
                              bank ? "EXTINT1" : "EXTINT0", Z63_EXTINT_SPAN);
        memory_region_add_subregion_overlap(sysmem,
                                            Z63_EXTINT_BASE + bank * 0x1000,
                                            &w->mr, 1);
    }
}

#endif /* HW_ARM_NIKON_Z63_EXTINT_H */
