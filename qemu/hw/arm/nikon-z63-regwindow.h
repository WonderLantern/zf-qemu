/*
 * Nikon Z 6III -- an MMIO window whose register set came out of the firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Several EXPEED 7 blocks are reached only through a descriptor: the base
 * never appears as an immediate, so no address scan of the text finds them,
 * and the driver is a family of tiny accessors that each do
 *
 *     LDR Xn, [X0,#8]        ; the device base, out of the handle
 *     LDR/STR Wt, [Xn,#imm]  ; one register
 *
 * For a block like that there is nothing to model but the register file, and
 * the honest model is: an offset the firmware is known to name is storage,
 * and an offset it is not stops the machine with its PC. Storage is right
 * where the firmware read-modify-writes a register, which is most of them;
 * where the firmware only ever reads, storage is a declared choice and the
 * block's own header says so and lists which offsets those are.
 *
 * This does not model behaviour, and it must not grow to. If a block turns
 * out to need a side effect -- an interrupt, a completion bit, a FIFO -- it
 * needs its own MemoryRegionOps, and the firmware address that demanded the
 * side effect belongs in the comment above it. What this gives is the floor:
 * the machine stops being stopped by a register file, so the next thing it
 * does is the next thing worth looking at.
 *
 * The offset sets are produced by z63-tools/handle_offsets.py (constant
 * offsets off a [X0,#8]-derived base, by a per-function register walk) and
 * z63-tools/window_pointers.py (absolute addresses the image holds in a
 * table, which no offset scan can see). Both read the image this board
 * loads; neither guesses.
 */

#ifndef HW_ARM_NIKON_Z63_REGWINDOW_H
#define HW_ARM_NIKON_Z63_REGWINDOW_H

/* Defined by the board; declared here as nikon-z63-display.h does. */
static void z63_stop_read(const char *block, hwaddr addr, unsigned size);
static void z63_stop_write(const char *block, hwaddr addr, unsigned size,
                           uint64_t value);
/* An optional model asked before the storage, which answers only where the
 * model declines (nikon-z63-z9lab.h; off unless -machine z9-capture=on). */
static bool z63_front_read(hwaddr addr, unsigned size, uint64_t *value);
static bool z63_front_write(hwaddr addr, uint64_t value, unsigned size);

/* A window may be one device or an array of identical ones. With a stride,
 * the offset set describes one instance and the same set applies at every
 * instance base -- which is how the firmware addresses them: it builds
 * `window_base + index * stride` and then applies the register offset. */
typedef struct Z63RegWindow {
    MemoryRegion mr;
    const char *name;
    hwaddr base;
    unsigned size;
    unsigned stride;            /* 0 for a single device */
    uint32_t *words;
    unsigned long *modelled;    /* indexed by (addr % stride) / 4 */
    bool dense;                 /* the whole span is storage; see below */
    /* A model that needs to see one register's writes without owning the
     * block (nikon-z63-dspipc.h watches a DSP's reset release): called
     * after each stored write, with the window offset. Storage stays the
     * answer to every read. */
    void (*write_hook)(void *opaque, hwaddr off, uint32_t value);
    void *write_hook_opaque;
} Z63RegWindow;

/*
 * Dense windows: where an enumerated set is the wrong model
 * --------------------------------------------------------
 * The offset set is a tripwire -- an offset the firmware was not seen to
 * name stops the machine, which is how the next block gets found. That
 * only works where the firmware names its offsets as immediates.
 *
 * Some blocks are register *files* the firmware fills with a loop, and the
 * extent of the loop is a runtime count. 0x40c23974 onward, writing the
 * window at 0x15040000, is the plain case:
 *
 *     40c23978  MOV  W12, W1            ; the count, an argument
 *     40c23980  MOV  W11, #0x28c
 *     40c23990  ADD  X15, X8, X9,LSL#2  ; base + 4k
 *     40c239c0  ADD  X15, X8, X11       ; base + 0x28c + 0x10k
 *     40c239c8  ADD  X11, X11, #0x10
 *     40c239dc  STUR W13, [X15,#-8]     ; base + 0x284 + 0x10k
 *
 * No static set can enumerate that: the offsets depend on an argument. A
 * set derived from the immediates therefore has holes, and each hole is a
 * false stop -- the machine halts on a register the firmware is plainly
 * entitled to write, and the halt says nothing. (0x150402c4, which is
 * 0x284 + 0x10 * 4, is exactly that: the crank stopped there after the
 * block before it was fixed.)
 *
 * For such a window `dense` makes the whole span storage. This buys nothing
 * about behaviour -- it is the same storage, with the same declared choice
 * -- and it gives up the tripwire for that one window, which is the cost
 * and is why it is per-window and not the default. Prefer the enumerated
 * set; set `dense` only with the loop that forces it named above the call.
 */
static bool z63_regwindow_has(Z63RegWindow *w, hwaddr addr, unsigned size)
{
    hwaddr off = w->stride ? addr % w->stride : addr;

    if (size != 4 || (addr & 3) || addr >= w->size) {
        return false;
    }
    return w->dense || test_bit(off / 4, w->modelled);
}

static uint64_t z63_regwindow_read(void *opaque, hwaddr addr, unsigned size)
{
    Z63RegWindow *w = opaque;
    uint64_t value;

    if (z63_front_read(w->base + addr, size, &value)) {
        return value;
    }
    if (!z63_regwindow_has(w, addr, size)) {
        z63_stop_read(w->name, w->base + addr, size);
        return 0;
    }
    return w->words[addr / 4];
}

static bool zf_isp199_owns_address(hwaddr addr);
static bool zf_stream154c_owns_address(hwaddr addr);
static bool zf_isp192_owns_address(hwaddr addr);
static bool zf_isp_small_owns_address(hwaddr addr);
static bool zf_statistics_owns_address(hwaddr addr);

static void z63_regwindow_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    Z63RegWindow *w = opaque;
    bool taken = z63_front_write(w->base + addr, value, size);
    /* This complete Zf driver has one storage owner. Do not also mutate
     * the old board shadow or invoke its hook after a handled access. */
    if (taken && (zf_isp199_owns_address(w->base + addr) ||
                  zf_stream154c_owns_address(w->base + addr) ||
                  zf_isp192_owns_address(w->base + addr) ||
                  zf_isp_small_owns_address(w->base + addr) ||
                  zf_statistics_owns_address(w->base + addr))) {
        return;
    }

    if (!z63_regwindow_has(w, addr, size)) {
        if (!taken) {
            z63_stop_write(w->name, w->base + addr, size, value);
        }
        return;
    }
    w->words[addr / 4] = value;
    if (w->write_hook) {
        w->write_hook(w->write_hook_opaque, addr, value);
    }
}

static const MemoryRegionOps z63_regwindow_ops = {
    .read = z63_regwindow_read,
    .write = z63_regwindow_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* For a block whose driver loads a register pair as one 64-bit access
 * (named where the window is created): QEMU splits it into two 32-bit
 * halves, and each half must be in the set like any other access. */
static const MemoryRegionOps z63_regwindow_ops64 = {
    .read = z63_regwindow_read,
    .write = z63_regwindow_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void z63_regwindow_init_ops(Object *owner, MemoryRegion *sysmem,
                                   Z63RegWindow *w, const char *name,
                                   hwaddr base, unsigned size,
                                   unsigned stride,
                                   const uint16_t *offsets, size_t n,
                                   const MemoryRegionOps *ops)
{
    unsigned span = stride ? stride : size;

    w->name = name;
    w->base = base;
    w->size = size;
    w->stride = stride;
    w->words = g_new0(uint32_t, size / 4);
    w->modelled = bitmap_new(span / 4);

    for (size_t i = 0; i < n; i++) {
        assert(offsets[i] < span && !(offsets[i] & 3));
        set_bit(offsets[i] / 4, w->modelled);
    }
    memory_region_init_io(&w->mr, owner, ops, w, name, size);
    memory_region_add_subregion(sysmem, base, &w->mr);
}

static void z63_regwindow_init_strided(Object *owner, MemoryRegion *sysmem,
                                       Z63RegWindow *w, const char *name,
                                       hwaddr base, unsigned size,
                                       unsigned stride,
                                       const uint16_t *offsets, size_t n)
{
    z63_regwindow_init_ops(owner, sysmem, w, name, base, size, stride,
                           offsets, n, &z63_regwindow_ops);
}

static void z63_regwindow_init(Object *owner, MemoryRegion *sysmem,
                               Z63RegWindow *w, const char *name,
                               hwaddr base, unsigned size,
                               const uint16_t *offsets, size_t n)
{
    z63_regwindow_init_strided(owner, sysmem, w, name, base, size, 0,
                               offsets, n);
}

#endif /* HW_ARM_NIKON_Z63_REGWINDOW_H */
