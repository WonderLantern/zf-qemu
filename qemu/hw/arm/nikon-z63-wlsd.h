/*
 * Nikon Z 6III -- the SDIO host Linux uses for the wireless module.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linux's device tree has `sdhci@0x17830000` (compatible "nikon,xxx-sdhci",
 * 0x300 bytes, SPI 97-99, 4-bit, fixed 1.8 V, non-removable) with a Broadcom
 * SDIO child, and the boot's S01-create-devices skips it on purpose; the
 * camera applications load sdhci-nikon.ko once they run, which they first
 * did when the MCU peer let the start-up proceed. The RTOS never touches
 * this block.
 *
 * The register file is an SDHCI one. sdhci-nikon.ko is a thin wrapper
 * around the kernel's SDHCI core; on the standard set it names only Power
 * Control (+0x29), Clock (+0x2c), Interrupt Status (+0x30) and Host
 * Control 2 (+0x3e), and leaves the rest to the core. So the standard
 * 0x100 bytes are QEMU's own generic SDHCI (hw/sd/sdhci.c, spec 3.00, its
 * default capabilities) with NO card on its bus: the wireless chip is not
 * modelled, and an empty slot is the honest thing for the core to find --
 * its commands time out and it gives the slot up, instead of the machine
 * stopping. The capability word is QEMU's default, a declared choice.
 *
 * The vendor words above the standard set are storage, and only the ones
 * sdhci-nikon.ko names (host->ioaddr is held at host+0x18):
 *
 *   +0x108 +0x114   read and written (set_clock / voltage switch)
 *   +0x124          written
 *   +0x200 = 0, +0x20c = 0x3f, +0x208 = 8, +0x210 = 6, +0x230 = 1
 *                   written in that order by sdhci_nikon_probe, 0xad0..0xb28
 *
 * which is the same vendor layout the RTOS's SD host at 0x17820000 has
 * (nikon-z63-storage.h): the same IP. Anything else above +0x100 stops.
 */

#ifndef HW_ARM_NIKON_Z63_WLSD_H
#define HW_ARM_NIKON_Z63_WLSD_H

#include "hw/sd/sdhci.h"

#define Z63_WLSD_BASE       0x17830000ULL
#define Z63_WLSD_STD        0x100
#define Z63_WLSD_VENDOR     0x200   /* +0x100 .. +0x2ff */
#define Z63_WLSD_SPI        97      /* first of the node's three lines */

/* Relative to +0x100. */
static const uint16_t z63_wlsd_vendor_offsets[] = {
    0x008, 0x014, 0x024, 0x100, 0x108, 0x10c, 0x110, 0x130,
};

static void z63_wlsd_init(Object *owner, MemoryRegion *sysmem,
                          DeviceState *gic, Z63RegWindow *vendor)
{
    DeviceState *sd = qdev_new(TYPE_SYSBUS_SDHCI);

    qdev_prop_set_uint8(sd, "sd-spec-version", 3);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sd), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sd), 0, Z63_WLSD_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(sd), 0,
                       qdev_get_gpio_in(gic, Z63_GIC_SPI(Z63_WLSD_SPI)));
    z63_regwindow_init(owner, sysmem, vendor, "WLSD-VENDOR",
                       Z63_WLSD_BASE + Z63_WLSD_STD, Z63_WLSD_VENDOR,
                       z63_wlsd_vendor_offsets,
                       ARRAY_SIZE(z63_wlsd_vendor_offsets));
}

#endif /* HW_ARM_NIKON_Z63_WLSD_H */
