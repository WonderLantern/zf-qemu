/*
 * Nikon Z 6III -- the NoC bus windows: how DMA masters see DRAM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Much of the 0x14000000 file the board maps as the "video pipeline" is not
 * one: the region table calls 0x14000000-0x15000000 NoC, and it holds the
 * address windows through which DMA masters reach DRAM. A window is 32
 * bytes: +0 start of its bus aperture, +8 minus its size, +0x10 physical
 * target. The image locates them itself -- 0x44b156b0 maps window ids to
 * (group, slot), and 0x44b15cbc gives each of 17 groups its two register
 * banks and slot count, the table below -- and reads one back with
 * sub_411877B0:
 *
 *   bank  = group in 0xf87c, or bank 0's start has bit 4  ? 1 : 0
 *   start = +0 & ~0x3f, span = -(+8 & ~0x3f) (32 bits)
 *   phys  = (bank ? 0x800000000 : 0xc00000000) + (+0x10 & 0xfffffffc0)
 *
 * Groups 3, 4 and 10 are paired: sub_4118767C writes the slot and the next
 * one together. Ports are read on even slots and written on odd ones (the
 * id table pairs them that way, and every driver registers them in that
 * order -- sub_40CAA5FC for the two XDMACs, sub_402EEF20 for graphics).
 *
 * Provisioning -- a DECLARED CHOICE
 * ---------------------------------
 * The RTOS never sets start or size. sub_40CB8440 reads all 193 windows
 * back into its own table at 0xa02814458, and sub_40CB8098 then only
 * retargets a window inside the aperture it found. On the body the loader
 * has set them; here nothing does, so every aperture read back as 0, every
 * window request failed, and the bus addresses the firmware derived from
 * them (sub_402014D0) were stack garbage -- 0x70617200 in all six of the
 * graphics engine's slots.
 *
 * z63_noc_provision fills them with ../z9-qemu's explicit lab hypothesis
 * for the same SoC, not with measurements: 128 MiB apertures from bus
 * 0x40000000, 512 MiB for groups 1, 2 and 15 (the drivers ask for exactly
 * that: sub_40CAA5FC and sub_402EEF20 register 0x20000000-byte ranges),
 * 1 GiB at 0x80000000 for group 16, and bit 4 set in each bank-0 start so
 * that sub_411877B0 reads bank 1 -- except group 16 and group 1's first
 * four slots, which have only bank 0.
 *
 * Targets, too, are the loader's, and also DECLARED here. At start-up the
 * RTOS adds its own bias to every target the loader left below 1 GiB
 * (sub_41187344: +0x10 |= 0x200000000, stopping at the first slot with any
 * of bits 30..35 set), which puts a target of 0 at DRAM's start,
 * 0xa00000000. Left at 0, all eight of DSP 1's 128 MiB apertures
 * (window ids 56..63, bus 0x40000000..0xb0000000) alias DRAM's first
 * 128 MiB, so the DSP driver's own translation (sub_40C910D0, then
 * sub_40C907E0 on completion) finds no window for any buffer above that
 * and uses an uninitialised local: the photometry jobs 0x1a/0x1b went out
 * with bus address 0xa, and each completion wrote its translated-back
 * "address" -- stale stack, 0xa02faf820 -- over DSP 1's queue-0 lock word
 * and then linked the queue through it (head lost, sub_40C90630 never
 * sending again), so the photometry stage waited for ever with its lock
 * held, live-view frame records (allocator type 17) were never released
 * and the camera stalled.
 *
 * The DSP driver looks only at the read windows of its DSP's group -- ids
 * 40..47 (group 3, DSP 0), 56..63 (group 4, DSP 1), 72..77 (group 5,
 * DSP 2), the {first, last} pairs in sub_40C90B24 -- which sit at bus
 * 0x40000000 + k * 0x10000000; the write windows of each pair sit 128 MiB
 * above. So for those three groups only, read window k and its write
 * sibling are given target k * 128 MiB: DSP 0's and DSP 1's eight windows
 * tile the 1 GiB below the RTOS's limit exactly, DSP 2's six the first
 * 768 MiB. Group 0 -- the HDMAC's, so the serial controllers' DMA --
 * has the same layout and gets the same targets: its users only look a
 * buffer up (sub_40CB8280, which reports success with bus address 0 when
 * no window holds the buffer), and the lens link's time-stamp command 0x21
 * sends from 0xa1cea5840, 476 MiB in, which went out as bus 0 and never
 * moved (nikon-z63-lens.h). The other groups' targets stay 0, as before:
 * their drivers retarget the windows they use (sub_40CB8098).
 */

#ifndef HW_ARM_NIKON_Z63_NOC_H
#define HW_ARM_NIKON_Z63_NOC_H

typedef struct Z63Noc {
    uint32_t *words;                   /* the board's register file */
    hwaddr base, size;
} Z63Noc;

static const struct {
    uint32_t bank[2];
    uint8_t slots, paired;
} z63_noc_groups[17] = {
    { { 0x14154040, 0x14154260 }, 16, 0 }, { { 0x14018060, 0x140181e0 }, 12, 0 },
    { { 0,          0x140ac040 }, 12, 0 }, { { 0,          0x141680c0 }, 16, 1 },
    { { 0,          0x141644c0 }, 16, 1 }, { { 0,          0x14048060 }, 12, 0 },
    { { 0,          0x14044060 }, 12, 0 }, { { 0x14060060, 0x14060140 },  6, 0 },
    { { 0x14058060, 0x14058180 },  8, 0 }, { { 0x1404c060, 0x1404c180 },  8, 0 },
    { { 0x141600c0, 0x14160500 }, 16, 1 }, { { 0,          0x1406c040 },  6, 0 },
    { { 0,          0x1409c040 }, 16, 0 }, { { 0,          0x140a0040 }, 16, 0 },
    { { 0,          0x14098040 },  8, 0 }, { { 0,          0x140d0040 }, 12, 0 },
    { { 0x140100e0, 0          },  1, 0 },
};

static uint32_t *z63_noc_slot(Z63Noc *n, unsigned group, unsigned bank,
                              unsigned slot)
{
    hwaddr a = z63_noc_groups[group].bank[bank] + slot * 0x20;

    assert(a >= n->base && a + 0x18 <= n->base + n->size);
    return &n->words[(a - n->base) / 4];
}

static uint64_t z63_noc_word(const uint32_t *w, unsigned i)
{
    return w[2 * i] | ((uint64_t)w[2 * i + 1] << 32);
}

static void z63_noc_provision(Z63Noc *n)
{
    for (unsigned g = 0; g < ARRAY_SIZE(z63_noc_groups); g++) {
        unsigned paired = z63_noc_groups[g].paired;
        unsigned slots = z63_noc_groups[g].slots << paired;

        for (unsigned bank = 0; bank < 2; bank++) {
            if (!z63_noc_groups[g].bank[bank]) {
                continue;
            }
            for (unsigned e = 0; e < slots; e++) {
                bool bank0_only = g == 16 || (g == 1 && e < 4);
                uint64_t start = 0x40000000 + (e >> paired) * 0x08000000ULL;
                uint64_t size = 0x08000000;
                uint32_t *w = z63_noc_slot(n, g, bank, e);

                if (g == 1 || g == 2 || g == 15) {
                    start = 0x40000000 + (e / 2) * 0x20000000ULL;
                    size = 0x20000000;
                }
                if (g == 16) {
                    start = 0x80000000;
                    size = 0x40000000;
                }
                if (bank == 0 && !bank0_only) {
                    start |= 0x10;
                }
                w[0] = start;
                w[1] = start >> 32;
                w[2] = (uint32_t)-size;
                w[3] = (-size >> 32) & 0xf;        /* a 36-bit field */
                if (g == 0 || (g >= 3 && g <= 5)) {     /* see above */
                    w[4] = (((start & ~0x3fULL) - 0x40000000) >> 28) << 27;
                    w[5] = 0;
                }
            }
        }
    }
}

/*
 * A bus address seen by a master on `group`, reading (even slots) or
 * writing (odd slots), as the firmware's own sub_411877B0 would resolve it.
 * True only when the whole span lies inside one window.
 */
static bool z63_noc_window(Z63Noc *n, unsigned group, bool write,
                           uint32_t bus, hwaddr *pa, uint64_t *left)
{
    unsigned paired = z63_noc_groups[group].paired;
    unsigned slots = z63_noc_groups[group].slots << paired;

    for (unsigned e = write; e < slots; e += 2) {
        bool hi = ((0xf87c >> group) & 1) || !z63_noc_groups[group].bank[0] ||
                  (z63_noc_word(z63_noc_slot(n, group, 0, e), 0) & 0x10);
        const uint32_t *w;
        uint32_t start, span;

        if (hi && !z63_noc_groups[group].bank[1]) {
            hi = false;
        }
        w = z63_noc_slot(n, group, hi, e);
        start = z63_noc_word(w, 0) & 0xffffffc0;
        span = -(uint32_t)(z63_noc_word(w, 1) & 0xffffffc0);
        if (span && bus >= start && bus - start < span) {
            *pa = (hi ? 0x800000000ULL : 0xc00000000ULL) +
                  (z63_noc_word(w, 2) & 0xfffffffc0ULL) + (bus - start);
            *left = span - (bus - start);
            return true;
        }
    }
    return false;
}

/*
 * The whole of [bus, bus + len) as one physical range. Each beat on the
 * bus is translated by itself, so a buffer may run from one window into
 * the next; it is one range wherever their targets are contiguous, as the
 * graphics driver programs group 15's 512 MiB windows (bus 0x40000000 and
 * 0x60000000 -> DRAM +0 and +512 MiB, so the menu's 640x4320 clear at bus
 * 0x5f979000 crosses into the second one). Anything else is refused, as a
 * range outside every window is.
 */
static bool z63_noc_translate(Z63Noc *n, unsigned group, bool write,
                              uint32_t bus, uint64_t len, hwaddr *pa)
{
    uint64_t at = bus, left;
    hwaddr first, next;

    if (!z63_noc_window(n, group, write, bus, &first, &left)) {
        return false;
    }
    next = first;
    while (left < len) {
        hwaddr p;

        at += left;
        next += left;
        len -= left;
        if (at > UINT32_MAX ||
            !z63_noc_window(n, group, write, at, &p, &left) || p != next) {
            return false;
        }
    }
    *pa = first;
    return true;
}

#endif /* HW_ARM_NIKON_Z63_NOC_H */
