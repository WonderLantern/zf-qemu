/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reuse the cached Z63 SD PIO attachment and Z8 SDHCI wiring.
 * Zf 40290c88 registers INTID132; its 402922f4 ISR reads17820030/38.
 * Card and transfers are QEMU's SD implementation, backed by a named drive.
 */
#ifndef HW_ARM_NIKON_ZF_SD_H
#define HW_ARM_NIKON_ZF_SD_H
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "system/block-backend.h"
#include "system/memory.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/designware.h"

#define TYPE_ZF_SD_DMA "nikon-zf-sd-dma"
#define ZF_SD_MAIN_BASE 0x17820000
#define ZF_SD_SECOND_FRONT_BASE 0x17828000
#define ZF_SD_SECOND_BASE 0x17828200
#define ZF_SD_MAIN_IRQ 132
#define ZF_SD_SECOND_IRQ 135
typedef struct ZfSdDma {
    IOMMUMemoryRegion parent;
    Z63Noc *noc;
    PCIDevice *pci_master;
    uint64_t mapped, direct, rejected, accesses;
} ZfSdDma;

static IOMMUTLBEntry zf_sd_dma_translate(IOMMUMemoryRegion *mr, hwaddr addr,
                                         IOMMUAccessFlags flags, int index)
{
    ZfSdDma *view = container_of(mr, ZfSdDma, parent);
    hwaddr page = addr & ~(hwaddr)63, physical = page;
    view->accesses++;
    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory, .iova = page,
        .addr_mask = 63, .perm = IOMMU_NONE,
    };
    if (view->pci_master && !view->pci_master->is_master) {
        view->rejected++;
        return entry;
    }
    if (view->pci_master && page > UINT32_MAX &&
        (page < UINT64_C(0xa00000000) || page >= UINT64_C(0xa40000000))) {
        view->rejected++;
        return entry;
    }
    /* The SDK writes ADMA descriptors into its on-SoC SRAM and passes
     *16800000 directly to BAR58 (own402ada2c..dad0). */
    if (view->pci_master && page >= 0x16800000 && page < 0x16801000) {
        entry.translated_addr = page;
        entry.perm = IOMMU_RW;
        view->direct++;
        return entry;
    }
    if (view->pci_master && page <= UINT32_MAX) {
        /* SDK402ada30..4c maps Axxxx DRAM to40000000+offset before
         * putting the32-bit payload in ADMA. This is not the first
         * host's group12 callback, and must not inherit its windows. */
        if (page < 0x40000000 || page >= 0x80000000) {
            view->rejected++;
            return entry;
        }
        entry.translated_addr = UINT64_C(0xa00000000) + page - 0x40000000;
        entry.perm = IOMMU_RW;
        view->mapped++;
        return entry;
    }
    /* Own402e7de8 uses40cb9480(group12,0,pointer,out) for descriptors
     * and data in both transfer directions. Use that same address view. */
    if (page <= UINT32_MAX) {
        if (!view->noc->words || !z63_noc_translate(view->noc, 12, false,
                                                   page, 64, &physical)) {
            if (view->rejected++ < 8) {
                error_report("ZF_SD_DMA: no group12 window for bus=%" HWADDR_PRIx, addr);
            }
            return entry;
        }
        view->mapped++;
    } else {
        /* Preserve generic64-bit physical DMA, separately tested by qtest. */
        view->direct++;
    }
    entry.translated_addr = physical;
    entry.perm = IOMMU_RW;
    return entry;
}

static uint64_t zf_sd_dma_page_size(IOMMUMemoryRegion *mr)
{
    return 64; /* NoC targets are64-byte aligned, not necessarily4KiB. */
}

static void zf_sd_dma_class_init(ObjectClass *klass, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);
    imrc->translate = zf_sd_dma_translate;
    imrc->get_min_page_size = zf_sd_dma_page_size;
}

static const TypeInfo zf_sd_dma_type = {
    .name = TYPE_ZF_SD_DMA, .parent = TYPE_IOMMU_MEMORY_REGION,
    .instance_size = sizeof(ZfSdDma), .class_init = zf_sd_dma_class_init,
};

static void zf_sd_dma_register_types(void)
{
    type_register_static(&zf_sd_dma_type);
}
type_init(zf_sd_dma_register_types)

static char *zf_sd_dma_state(Object *obj, Error **errp)
{
    SDHCIState *host = SYSBUS_SDHCI(obj);
    ZfSdDma *view = container_of(IOMMU_MEMORY_REGION(host->dma_mr),
                                 ZfSdDma, parent);
    return g_strdup_printf("{\"group\":12,\"mapped\":%" PRIu64
        ",\"direct64\":%" PRIu64 ",\"rejected\":%" PRIu64
        ",\"accesses\":%" PRIu64 "}",
        view->mapped, view->direct, view->rejected, view->accesses);
}

static char *zf_sd_pci_dma_state(Object *obj, Error **errp)
{
    SDHCIState *host = PCI_SDHCI(obj);
    ZfSdDma *view = container_of(IOMMU_MEMORY_REGION(host->dma_mr),
                                 ZfSdDma, parent);
    return g_strdup_printf("{\"view\":\"sdk-dram-and-sram\",\"mapped\":%" PRIu64
        ",\"direct64\":%" PRIu64 ",\"rejected\":%" PRIu64
        ",\"accesses\":%" PRIu64 "}",
        view->mapped, view->direct, view->rejected, view->accesses);
}

static bool zf_sd_second_present(Object *owner)
{
    Object *host = object_resolve_path_component(owner, "zf-microsd-host");
    return host && sdbus_get_inserted(&PCI_SDHCI(host)->sdbus);
}
static void zf_sd_provision_noc(Z63Noc *noc)
{
    /* The native SD converter only looks up existing windows. Like the
     * other read-only clients, its eight read windows must span DRAM.
     * Keep the shared aperture layout; tile targets for each R/W pair.
     * Original RTOS initialization subsequently adds its DRAM bias. */
    for (unsigned slot = 0; slot < z63_noc_groups[12].slots; slot++) {
        uint32_t *w = z63_noc_slot(noc, 12, 1, slot);
        w[4] = (slot / 2) * 0x08000000;
        w[5] = 0;
    }
}

static void zf_sd_init(Object *owner, MemoryRegion *sysmem,
                       DeviceState *gic, Z63Noc *noc, const char *drive)
{
    BlockBackend *blk = blk_by_name(drive);
    if (!blk) {
        error_report("zf-sd-drive: no block backend named '%s'", drive);
        exit(1);
    }
    DeviceState *host = qdev_new(TYPE_SYSBUS_SDHCI);
    ZfSdDma *view = g_new0(ZfSdDma, 1);
    memory_region_init_iommu(view, sizeof(*view), TYPE_ZF_SD_DMA,
                             owner, "zf-sd-dma", UINT64_MAX);
    view->noc = noc;
    object_property_add_child(owner, "zf-sd-host", OBJECT(host));
    object_property_set_link(OBJECT(host), "dma", OBJECT(view), &error_fatal);
    object_property_add_str(OBJECT(host), "zf-dma-state", zf_sd_dma_state, NULL);
    qdev_prop_set_uint8(host, "sd-spec-version", 2);
    /* 50MHz, 512-byte blocks, 3.3V, SDMA and 64-bit ADMA2.
     * Native32-bit addresses pass through NoC group12. */
    qdev_prop_set_uint64(host, "capareg", 0x1148328a);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(host), &error_fatal);
    /* Own40293834..40 admits ADMA2 only for vendor version1. */
    SYSBUS_SDHCI(host)->version = 0x0101;
    MemoryRegion *regs = sysbus_mmio_get_region(SYS_BUS_DEVICE(host), 0);
    assert(memory_region_size(regs) == 0x100);
    memory_region_add_subregion_overlap(sysmem, ZF_SD_MAIN_BASE, regs, 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(host), 0, qdev_get_gpio_in(gic, ZF_SD_MAIN_IRQ - 32));

    DeviceState *card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_bit(card, "nikon-acmd22", true);
    qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
    qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"), &error_fatal);
    error_report("ZF_SD: SDH30/SD card at17820000, INTID132, backend=%s; generic SDHCI only", drive);
}

static void zf_sd_init_second(Object *owner, MemoryRegion *sysmem,
                              DeviceState *gic, Z63Noc *noc,
                              DeviceState *pcie_host,
                              const char *drive)
{
    BlockBackend *blk = blk_by_name(drive);
    if (!blk) {
        error_report("zf-microsd-drive: no block backend named '%s'", drive);
        exit(1);
    }
    DeviceState *host = qdev_new("nikon-zf-sd-mode3");
    MemoryRegion *descriptors = g_new0(MemoryRegion, 1);
    memory_region_init_ram(descriptors, OBJECT(host), "zf-sdk-adma-sram", 0x1000, &error_fatal);
    memory_region_add_subregion(sysmem, 0x16800000, descriptors);
    ZfSdDma *view = g_new0(ZfSdDma, 1);
    memory_region_init_iommu(view, sizeof(*view), TYPE_ZF_SD_DMA,
                             owner, "zf-microsd-dma", UINT64_MAX);
    view->noc = noc;
    object_property_add_child(owner, "zf-microsd-host", OBJECT(host));
    object_property_set_link(OBJECT(host), "dma", OBJECT(view), &error_fatal);
    object_property_add_str(OBJECT(host), "zf-dma-state", zf_sd_pci_dma_state, NULL);
    qdev_prop_set_uint8(host, "sd-spec-version", 2);
    qdev_prop_set_uint64(host, "capareg", 0x1148328a);
    PCIBus *bus = &PCI_BRIDGE(&DESIGNWARE_PCIE_HOST(pcie_host)->root)->sec_bus;
    qdev_prop_set_uint32(host, "addr", PCI_DEVFN(0, 0));
    qdev_realize_and_unref(host, BUS(bus), &error_fatal);
    SDHCIState *pci_host = PCI_SDHCI(host);
    pci_host->version = 0x0101;
    PCIDevice *pdev = PCI_DEVICE(host);
    view->pci_master = pdev;

    /* SDH40 is a separate on-SoC UHS-II block, not a second address for this
     * PCI endpoint. Keep its existing empty model; all SDK commands must
     * pass through guest-programmed PCI BAR and outbound iATU decoding. */
    DeviceState *card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_bit(card, "nikon-acmd22", true);
    qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
    qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"), &error_fatal);
    error_report("ZF_SD: mode3 PCI SD backend=%s, guest-programmed BAR0/iATU, PCIe INTx149/MSI150; SDH40/135 remains separate", drive);
}
#endif


