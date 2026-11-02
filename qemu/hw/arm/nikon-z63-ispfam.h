/*
 * Nikon Z 6III -- the rest of the ISP_APB device family.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * nikon-z63-isp-apb.h found the family -- one driver (0x40c1b000-
 * 0x40c28000), a descriptor table of {device id, MMIO base} at flat
 * 0x451fe5a8 -- and mapped the one instance the firmware had reached,
 * 0x1a0 at 0x150c0000, predicting that "the other instances will appear as
 * stops at their own bases when the firmware first reaches them. Each is
 * then one line here." The MCU-driven start reached them: 0x199 first
 * (nikon-z63-dev199.h), then 0x192 at 0x150ce200. This file is those lines,
 * for every instance in the table at once.
 *
 * Each window runs from its base to the next device's base, capped (0x1000
 * where the gap is open-ended), so no window can swallow a neighbour the
 * table names. The register set is the family's union from make_window.py
 * over the whole driver below 0x3000 (z63_dev199_offsets), clipped to each
 * window -- wider than any one device's file, the same declared trade-off
 * as nikon-z63-aux.h: storage for an offset that belongs to a sibling costs
 * nothing, but can hide an access worth seeing, so this is the first set to
 * re-derive per device if something here looks wrong. An offset outside it
 * still stops the machine.
 */

#ifndef HW_ARM_NIKON_Z63_ISPFAM_H
#define HW_ARM_NIKON_Z63_ISPFAM_H

typedef struct Z63IspFamDev {
    uint16_t id;
    hwaddr base;
    unsigned size;
} Z63IspFamDev;

/* 0x1a0 (nikon-z63-isp-apb.h) and 0x199 (nikon-z63-dev199.h) are mapped
 * by their own headers and are not repeated here. */
static const Z63IspFamDev z63_ispfam_devs[] = {
    { 0x19d, 0x150c0400, 0x1000 },  /* gap to 0x150c8000 is open-ended */
    { 0x19a, 0x150cb000, 0x3000 },
    { 0x192, 0x150ce000, 0x0400 },
    { 0x193, 0x150ce400, 0x0400 },
    { 0x196, 0x150ce800, 0x1800 },
    { 0x1a3, 0x150d0000, 0x2000 },
    { 0x1a4, 0x150d2000, 0x2000 },
    { 0x194, 0x150d4000, 0x0c00 },
    { 0x19b, 0x150d4c00, 0x0200 },
    { 0x19c, 0x150d4e00, 0x0200 },
    { 0x195, 0x150d5000, 0x0100 },
    { 0x197, 0x150d5100, 0x0100 },
    { 0x198, 0x150d5200, 0x0e00 },
    { 0x1a1, 0x150d6000, 0x2000 },
    { 0x1a2, 0x150d8000, 0x1800 },
    { 0x19f, 0x150d9800, 0x0200 },
    { 0x19e, 0x150d9a00, 0x0600 },  /* to the next 4 KiB boundary */
};

static void z63_ispfam_init(Object *owner, MemoryRegion *sysmem,
                            Z63RegWindow *w)
{
    for (size_t d = 0; d < ARRAY_SIZE(z63_ispfam_devs); d++) {
        const Z63IspFamDev *dev = &z63_ispfam_devs[d];
        uint16_t *offs = g_new(uint16_t, ARRAY_SIZE(z63_dev199_offsets));
        size_t n = 0;

        for (size_t i = 0; i < ARRAY_SIZE(z63_dev199_offsets); i++) {
            if (z63_dev199_offsets[i] < dev->size) {
                offs[n++] = z63_dev199_offsets[i];
            }
        }
        z63_regwindow_init(owner, sysmem, &w[d],
                           g_strdup_printf("ISP-APB-DEV-0x%x", dev->id),
                           dev->base, dev->size, offs, n);
        g_free(offs);
    }
}

#endif /* HW_ARM_NIKON_Z63_ISPFAM_H */
