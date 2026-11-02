/*
 * Nikon Z 6III -- XDMAC, the transfer engine the firmware's memory copies use.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Two controllers, four channels each. This board already mapped both as
 * plain storage, under the names PAIR-A and PAIR-B, and the comment there
 * had even noticed the `orr w9, w9, #0x10000000` that programs them without
 * knowing what it was. Storage is why nothing worked: a transfer was
 * programmed, started, and never happened, so the waiter never woke.
 *
 * What it blocks
 * --------------
 * `sub_40D710DC` -- the system init sequence -- copies 0x1000 bytes from
 * 0x40195000 to 0x08201000 and only then sets ROMRAM's stage-6 bit, which is
 * what the entire display and menu lifecycle waits for. That copy goes
 * sub_40CAB028 -> sub_40CAAF20 -> sub_40C1B4D8 (program) + sub_40C1B608
 * (start) + sub_40CAAAC8 (wait on event flag 0xa030511c8). The full chain is
 * in docs/serial-nor-resources.md.
 *
 * Where the layout comes from
 * ---------------------------
 * The firmware's own descriptors, read out of a running machine at
 * 0xa0280d388 and following, 0x40 apart:
 *
 *   ch  id      register base   INTID   handler
 *   0   0x1bf   0x0c850010      0x22    0x40c1b444
 *   1   0x1c0   0x0c850040      0x23    0x40c1b658
 *   2   0x1c1   0x0c850070      0x24    0x40c1b660
 *   3   0x1c2   0x0c8500a0      0x25    0x40c1b668
 *   4   0x1c3   0x16808010      0x7d    0x40c1b670
 *   5   0x1c4   0x16808040      0x7e    0x40c1b678
 *   6   0x1c5   0x16808070      0x7f    0x40c1b680
 *   7   0x1c6   0x168080a0      0x80    0x40c1b688
 *
 * INTIDs 0x22-0x25 are 34-37, SPI 2-5, named PERI_XDMAC_0..3 in the
 * interrupt table at 0x41847000; 0x7d-0x80 are 125-128, SPI 93-96,
 * EXS_XDMAC_0..3. Those eight handlers are exactly the ones the runtime
 * handler table at 0xa02c2c750 has installed for those INTIDs, so the
 * descriptor and the GIC agree.
 *
 * The register map is read off the three functions that use it:
 *
 *   sub_40C1B4D8(dev, desc, arg)   programs, with base = *(u64 *)(dev + 8):
 *       +0x00 = desc[0]            transfer length minus one
 *       +0x04 = desc[1]            source
 *       +0x08 = desc[2]            destination
 *       +0x0c, +0x10               source and destination configuration
 *       +0x14, +0x18              mode and control fields
 *   sub_40C1B608(dev, mode)        starts: reads +0x18 and rewrites it with
 *                                  the run bits 0 and 1 set (and bit 28,
 *                                  which every write to this register sets)
 *   sub_40C1B444()                 the channel-0 handler: reads +0x20 and
 *                                  keeps its low four bits as the cause,
 *                                  reads +0x00 for what is left, then acks
 *                                  with `+0x20 &= ~0xf` and `+0x18 &= ~3`
 *
 * What is modelled and what is declared
 * -------------------------------------
 * A write to +0x18 with bit 28 set copies `(+0x00) + 1` bytes from +0x04 to
 * +0x08 through the guest address space, zeroes +0x00 because the handler
 * reads it as the remainder, sets bit 0 of +0x20 and raises the channel's
 * line. Clearing the low nibble of +0x20 lowers it again, which is the ack
 * the handler performs.
 *
 * Two DECLARED CHOICES. The transfer is synchronous, so a guest that looks
 * at +0x00 between starting and the interrupt sees it already finished;
 * real silicon would count down. And **which** of the four cause bits a
 * completion sets is not established -- the handler keeps all four and tests
 * none -- so bit 0 is used and nothing is claimed about the other three.
 *
 * Addresses
 * ---------
 * Source and destination are bus addresses. Below 0x40000000 a bus address
 * is the physical one -- the firmware's own converter, sub_402015B8, returns
 * those unchanged. Above, they go through the controller's NoC window
 * group (nikon-z63-noc.h): sub_40CAA5FC registers group 1's windows (ids
 * 16-27) and group 2's (ids 28-39) for the two controllers, reads on even
 * slots and writes on odd ones, which is ../z9-qemu's reading of the same
 * SoC (PERI on group 1, EXS on group 2). Until the windows existed every
 * DRAM buffer reached this model untranslated, which only worked because
 * the failed window lookup left the CPU address in place.
 *
 * A window may land in any RAM, not only DRAM: the EXS controller's upper
 * four windows (bus 0x80000000-0xffffffff) are retargeted by sub_40CB8098
 * to BUFFER_EXT, 0x840000000-0x8c0000000 in the firmware's own table at
 * 0xa02814458. The card's file cache lives there -- playback's and the
 * card scan's copy of a movie's moov box (sub_40CAAF20, bus 0xf2096e3c ->
 * 0xf02c1168) -- and refusing anything outside DRAM stopped the camera on
 * the first MP4 on the card.
 *
 * One DECLARED CHOICE: a source inside the image window
 * (0x40000000-0x48000000) that no window turns into DRAM is read where it
 * stands. That is sub_40D710DC's copy of 0x40195000, through windows the
 * firmware points at 0xF00000000 (ids 16, 17, 22, 23) -- an aperture whose
 * meaning on the DMA side is not measured. Anything else unresolved stops.
 *
 * The configuration words at +0x0c (source) and +0x10 (destination) are,
 * per sub_40C1B4D8: width log2 in bits 17:16, burst in 11:8, bit 2 address
 * fixed, bit 0 unknown. Only bit 2 of the source is acted on: sub_40CAB55C,
 * the DMA memset, broadcasts its byte into the channel's 8-byte slot at
 * 0x16807f00 + 8 * channel and points a fixed, 8-byte-wide source at it
 * (0x30f04 against the destination's 0x30f00), so a fixed source repeats one
 * width unit across the transfer -- and must be that slot, since it is the
 * only fixed source the firmware is known to use. A fixed destination
 * stops. Burst and endianness are ignored; a transfer that depends on them
 * will be wrong and nothing here would notice.
 */

#ifndef HW_ARM_NIKON_Z63_XDMAC_H
#define HW_ARM_NIKON_Z63_XDMAC_H

#define Z63_XDMAC_CTRLS      2
#define Z63_XDMAC_CHANS      4
/* Channel 0's offset inside its window. Controller A's window starts at
 * 0x0c850000 so it is 0x10; controller B's window starts at 0x16807000,
 * a page below its channels, so it is 0x1010. */
#define Z63_XDMAC_CH_STRIDE  0x30
#define Z63_XDMAC_R_LEN      0x00
#define Z63_XDMAC_R_SRC      0x04
#define Z63_XDMAC_R_DST      0x08
#define Z63_XDMAC_R_SCFG     0x0c
#define Z63_XDMAC_R_DCFG     0x10
#define Z63_XDMAC_R_CTRL     0x18
#define Z63_XDMAC_FIXED      (1u << 2)
#define Z63_XDMAC_FILL_SLOTS 0x16807f00ULL     /* 8 bytes per channel */
#define Z63_XDMAC_R_STATUS   0x20
/*
 * The run bits are 0 and 1, not bit 28.
 *
 * sub_40C1B608 sets both of them -- `| 0x1100003` on one path and `| 3` on
 * the other -- and the handler's acknowledgement clears exactly those two:
 * `*(u32 *)(base + 24) &= 0xfffffffc`. Bit 28 is set on every write to this
 * register, including the acknowledgement, so it is an enable or a
 * write-permit and not a start.
 *
 * This model treated bit 28 as the start, which made the acknowledgement
 * look like another start. The write trace showed it precisely: a single
 * 0x11109003 (run bits set) followed by 0x11109000 written 2,386,913 times
 * in 25 seconds, one per acknowledgement, each one starting a fresh
 * transfer. Starting on the 0 -> 1 edge of the run bits is what the pair of
 * functions actually describes.
 */
#define Z63_XDMAC_RUN        0x3u
#define Z63_XDMAC_CAUSE      0xfu
/* A single transfer this model will perform. Larger is a programming error
 * or a register layout this code has misread, and is worth stopping for. */
#define Z63_XDMAC_MAX        (64 * MiB)

/* One more DECLARED CHOICE, 2026-10-26. With rom-page-stub=on the board
 * lays z63_rompage_keep_ops (an MMIO overlay, priority 1) over the
 * boot-ROM service page at 0x08201000 in WorkSRAM. That overlay passes
 * every write through to the RAM underneath and answers reads from it
 * (ret at the six entries), but as MMIO it is not "backed" for
 * z63_xdmac_backed, which refused sub_40D710DC's documented
 * 0x40195000 -> 0x08201000 copy of the genuine service page and stopped
 * the boot -- the stub and this model could not both be used. A
 * controller the board marks rompage_keep accepts a physical *write*
 * address inside that one page; the bytes still land in WorkSRAM
 * through the overlay's own write path, so nothing is invented beyond
 * letting the documented copy through. Reads of the page keep the
 * overlay's semantics (ret at the six entries). */
#define Z63_XDMAC_KEEP_BASE  0x08201000ULL
#define Z63_XDMAC_KEEP_END   0x08202000ULL

/* Defined by the board, like the stop helpers nikon-z63-display.h declares. */
static void z63_set_irq(qemu_irq line, int level, const char *name);

typedef struct Z63XdmacCtrl {
    MemoryRegion mr;
    const char *name;
    hwaddr base;
    unsigned size;
    unsigned first_intid;
    unsigned ch_base;
    uint32_t *words;
    qemu_irq irq[Z63_XDMAC_CHANS];
    bool reported[Z63_XDMAC_CHANS];
    Z63Noc *noc;
    unsigned noc_group;
    bool rompage_keep;          /* the service page's keep overlay writes
                                 * through to WorkSRAM; see the DECLARED
                                 * CHOICE above. */
} Z63XdmacCtrl;

/* A transfer address, as this controller's master sees it (see header). */
static bool z63_xdmac_backed(hwaddr pa, uint64_t len)
{
    MemoryRegionSection sec = memory_region_find(get_system_memory(), pa, len);
    bool ok = sec.mr && int128_get64(sec.size) == len &&
              (memory_region_is_ram(sec.mr) || memory_region_is_romd(sec.mr));

    if (sec.mr) {
        memory_region_unref(sec.mr);
    }
    return ok;
}

static bool z63_xdmac_addr(Z63XdmacCtrl *c, uint32_t bus, uint64_t len,
                           bool write, hwaddr *pa)
{
    if (bus < 0x40000000) {
        /* Physical -- but a device here would be a device feeding DMA,
         * which no model in this board does, so only memory passes.
         * The exception is the service page's keep overlay, which
         * writes through to WorkSRAM underneath (see the DECLARED
         * CHOICE by Z63_XDMAC_KEEP_BASE). */
        if (c->rompage_keep && write && bus >= Z63_XDMAC_KEEP_BASE &&
            (uint64_t)bus + len <= Z63_XDMAC_KEEP_END) {
            *pa = bus;
            return true;
        }
        *pa = bus;
        return z63_xdmac_backed(bus, len);
    }
    if (z63_noc_translate(c->noc, c->noc_group, write, bus, len, pa) &&
        z63_xdmac_backed(*pa, len)) {
        return true;
    }
    if (!write && (uint64_t)bus + len <= 0x48000000) {
        *pa = bus;                              /* the image window */
        return true;
    }
    return false;
}

static bool z63_xdmac_channel(Z63XdmacCtrl *c, hwaddr off, unsigned *chan,
                              unsigned *reg)
{
    if (off < c->ch_base) {
        return false;
    }
    off -= c->ch_base;
    if (off / Z63_XDMAC_CH_STRIDE >= Z63_XDMAC_CHANS) {
        return false;
    }
    *chan = off / Z63_XDMAC_CH_STRIDE;
    *reg = off % Z63_XDMAC_CH_STRIDE;
    return true;
}

static void z63_xdmac_update_irq(Z63XdmacCtrl *c, unsigned chan)
{
    uint32_t *r = c->words + (c->ch_base +
                              chan * Z63_XDMAC_CH_STRIDE) / 4;

    if (c->irq[chan]) {
        z63_set_irq(c->irq[chan], (r[Z63_XDMAC_R_STATUS / 4] & Z63_XDMAC_CAUSE)
                    != 0, c->name);
    }
}

static void z63_xdmac_run(Z63XdmacCtrl *c, unsigned chan)
{
    uint32_t *r = c->words + (c->ch_base +
                              chan * Z63_XDMAC_CH_STRIDE) / 4;
    uint64_t len = (uint64_t)r[Z63_XDMAC_R_LEN / 4] + 1;
    uint32_t sbus = r[Z63_XDMAC_R_SRC / 4];
    uint32_t dbus = r[Z63_XDMAC_R_DST / 4];
    hwaddr src, dst;

    if (len > Z63_XDMAC_MAX) {
        z63_stop_write("XDMAC (transfer length)", c->base +
                       c->ch_base + chan * Z63_XDMAC_CH_STRIDE,
                       4, len - 1);
        return;
    }
    uint32_t scfg = r[Z63_XDMAC_R_SCFG / 4], dcfg = r[Z63_XDMAC_R_DCFG / 4];
    unsigned unit = 1u << ((scfg >> 16) & 3);
    bool fill = scfg & Z63_XDMAC_FIXED;

    src = sbus;                     /* a fill reads its slot in place */

    if ((dcfg & Z63_XDMAC_FIXED) ||
        (fill && !(sbus >= Z63_XDMAC_FILL_SLOTS &&
                   sbus + unit <= Z63_XDMAC_FILL_SLOTS + 8 * 8))) {
        error_report("Z63_XDMAC: %s channel %u: fixed address (source 0x%08x "
                     "cfg 0x%x, destination cfg 0x%x) outside the memset "
                     "path this model knows", c->name, chan, sbus, scfg, dcfg);
        z63_stop_write("XDMAC (address mode)", c->base + c->ch_base +
                       chan * Z63_XDMAC_CH_STRIDE + Z63_XDMAC_R_CTRL, 4,
                       r[Z63_XDMAC_R_CTRL / 4]);
        return;
    }
    if ((!fill && !z63_xdmac_addr(c, sbus, len, false, &src)) ||
        !z63_xdmac_addr(c, dbus, len, true, &dst)) {
        error_report("Z63_XDMAC: %s channel %u: %" PRIu64 " bytes 0x%08x -> "
                     "0x%08x, an address no NoC window of group %u resolves",
                     c->name, chan, len, sbus, dbus, c->noc_group);
        z63_stop_write("XDMAC (bus address)", c->base + c->ch_base +
                       chan * Z63_XDMAC_CH_STRIDE + Z63_XDMAC_R_CTRL, 4,
                       r[Z63_XDMAC_R_CTRL / 4]);
        return;
    }
    if (!c->reported[chan]) {
        c->reported[chan] = true;
        error_report("Z63_XDMAC: %s channel %u first transfer, %" PRIu64
                     " bytes bus 0x%08x -> 0x%08x = 0x%09" PRIx64 " -> 0x%09"
                     PRIx64 " (SPI %u). Synchronous; see nikon-z63-xdmac.h.",
                     c->name, chan, len, sbus, dbus, (uint64_t)src,
                     (uint64_t)dst, c->first_intid + chan);
    }
    {
        g_autofree uint8_t *buf = g_malloc(len);

        if (fill) {
            uint8_t pat[8];

            /* The slot is this window's own storage, so read it there. */
            address_space_read(&address_space_memory, sbus,
                               MEMTXATTRS_UNSPECIFIED, pat, unit);
            for (uint64_t i = 0; i < len; i++) {
                buf[i] = pat[i % unit];     /* a short tail is truncated */
            }
        } else {
            cpu_physical_memory_read(src, buf, len);
        }
        cpu_physical_memory_write(dst, buf, len);
    }
    r[Z63_XDMAC_R_LEN / 4] = 0;           /* the handler reads the remainder */
    r[Z63_XDMAC_R_STATUS / 4] |= 1;
    z63_xdmac_update_irq(c, chan);
}

static uint64_t z63_xdmac_read(void *opaque, hwaddr off, unsigned size)
{
    Z63XdmacCtrl *c = opaque;

    if (size != 4 || (off & 3) || off >= c->size) {
        z63_stop_read(c->name, c->base + off, size);
        return 0;
    }
    return c->words[off / 4];
}

static void z63_xdmac_write(void *opaque, hwaddr off, uint64_t value,
                            unsigned size)
{
    Z63XdmacCtrl *c = opaque;
    unsigned chan, reg;
    uint32_t prev;

    if (size != 4 || (off & 3) || off >= c->size) {
        z63_stop_write(c->name, c->base + off, size, value);
        return;
    }
    prev = c->words[off / 4];
    c->words[off / 4] = value;

    if (!z63_xdmac_channel(c, off, &chan, &reg)) {
        return;
    }
    if (reg == Z63_XDMAC_R_CTRL &&
        (value & Z63_XDMAC_RUN) && !(prev & Z63_XDMAC_RUN)) {
        z63_xdmac_run(c, chan);
    } else if (reg == Z63_XDMAC_R_STATUS) {
        z63_xdmac_update_irq(c, chan);
    }
}

/*
 * 64-bit accesses reach this window, so `valid` has to admit them and
 * `impl` narrows them back to the 4-byte storage above. Rejecting them in
 * `valid` is not a refusal the board can see -- QEMU turns an access
 * outside `valid` into a bus error, which arrives in the guest as an
 * external abort rather than stopping the machine with a PC, so the model
 * would be lying by omission.
 *
 * What does it: a per-channel table of 8-byte entries at +0xf00, written
 * by `sub_40CDB64C` around 0x40cab680 --
 *
 *     MOV  X8, #0x7f00 / MOVK X8, #0x1680,LSL#16   ; 0x16807f00
 *     ADD  X21, X8, W0,UXTW#3                      ; + channel * 8
 *     MUL  X9, X9, #0x101010101010101              ; one byte, broadcast
 *     STR  X9, [X21]                               ; 64-bit store
 *
 * The RTOS named this for us: with the store refused, its own handler
 * printed `task [STSQNOR] ... EC = 00000025 : Data abort ... WnR 1 ...
 * DFSC 0x10 Synchronous External abort ... FAR = 0x0000000016807F10`.
 *
 * Declared choice: the entry is storage, and a read returns the byte
 * pattern last written. What the broadcast byte means to the hardware --
 * a per-channel priority or arbitration weight is the obvious reading --
 * is not established here, and nothing in this model acts on it.
 */
static const MemoryRegionOps z63_xdmac_ops = {
    .read = z63_xdmac_read,
    .write = z63_xdmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl  = { .min_access_size = 4, .max_access_size = 4 },
};

static void z63_xdmac_init(Object *owner, MemoryRegion *sysmem, DeviceState *gic,
                           Z63XdmacCtrl *c, const char *name, hwaddr base,
                           unsigned size, unsigned ch_base,
                           unsigned first_intid, uint32_t *words,
                           Z63Noc *noc, unsigned noc_group)
{
    c->noc = noc;
    c->noc_group = noc_group;
    c->name = name;
    c->base = base;
    c->size = size;
    c->ch_base = ch_base;
    c->first_intid = first_intid;
    c->words = words;
    for (unsigned ch = 0; ch < Z63_XDMAC_CHANS; ch++) {
        c->irq[ch] = qdev_get_gpio_in(gic, first_intid + ch);
    }
    memory_region_init_io(&c->mr, owner, &z63_xdmac_ops, c, name, size);
    memory_region_add_subregion(sysmem, base, &c->mr);
}

#endif /* HW_ARM_NIKON_Z63_XDMAC_H */
