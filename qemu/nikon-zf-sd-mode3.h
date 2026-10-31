/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional Zf SDK mode-3 controller. Generic virtual PCI identity; this is
 * not an identification of the physical reader or a UHS-II implementation.
 * Included after sdhci-pci.c so commands/data/reset use its SDHCI core. */
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_aer.h"
#include "hw/pci/msi.h"
#include "system/runstate.h"

#define TYPE_ZF_SD_MODE3 "nikon-zf-sd-mode3"
typedef struct ZfSdMode3 {
    SDHCIState parent_obj;
    MemoryRegion bar, extension;
    uint64_t config_reads, extension_reads, unsupported;
    uint64_t intx_assertions, msi_messages;
    bool irq_level, msi_latched;
    MemoryRegion core;
} ZfSdMode3;

static void zf_sd_mode3_irq(void *opaque, int n, int level)
{
    ZfSdMode3 *s = opaque;
    PCIDevice *dev = PCI_DEVICE(s);
    bool msi = msi_enabled(dev);

    if (!level || !msi) {
        s->msi_latched = false;
    }
    if (level && msi && !s->msi_latched) {
        msi_notify(dev, 0);
        s->msi_latched = true;
        s->msi_messages++;
    } else if (level && !msi && !s->irq_level) {
        s->intx_assertions++;
    }
    s->irq_level = level;
    pci_set_irq(dev, level && !msi);
}

static DeviceRealize zf_sd_mode3_parent_realize;
static DeviceReset zf_sd_mode3_parent_reset;
static void zf_sd_mode3_device_realize(DeviceState *dev, Error **errp)
{
    PCI_DEVICE(dev)->cap_present |= QEMU_PCI_CAP_EXPRESS;
    zf_sd_mode3_parent_realize(dev, errp);
}

/* Keep the shared SDHCI implementation. Reset writes must additionally
 * lower this endpoint's routed IRQ: the common core clears its registers
 * but does not notify the line on RESET_ALL/RESET_CMD/RESET_DATA. */
static uint64_t zf_sd_mode3_core_read(void *opaque, hwaddr addr, unsigned size)
{
    SDHCIState *host = &((ZfSdMode3 *)opaque)->parent_obj;
    return host->io_ops->read(host, addr, size);
}

static void zf_sd_mode3_core_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    ZfSdMode3 *s = opaque;
    SDHCIState *host = &s->parent_obj;
    host->io_ops->write(host, addr, value, size);
    if (addr <= 0x2f && addr + size > 0x2f) {
        bool pending = (host->norintsts & host->norintsigen) ||
            (host->errintsts & host->errintsigen) ||
            ((host->norintsts & SDHC_NIS_INSERT) &&
             (host->wakcon & SDHC_WKUP_ON_INS)) ||
            ((host->norintsts & SDHC_NIS_REMOVE) &&
             (host->wakcon & SDHC_WKUP_ON_RMV));
        qemu_set_irq(host->irq, pending);
    }
}

static const MemoryRegionOps zf_sd_mode3_core_ops = {
    .read = zf_sd_mode3_core_read, .write = zf_sd_mode3_core_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 1, .max_access_size = 4},
};

static void zf_sd_mode3_reset(DeviceState *dev)
{
    ZfSdMode3 *s = (ZfSdMode3 *)dev;
    zf_sd_mode3_parent_reset(dev);
    msi_reset(PCI_DEVICE(dev));
    zf_sd_mode3_irq(s, 0, 0);
}

static uint64_t zf_sd_mode3_read(void *opaque, hwaddr addr, unsigned size)
{
    ZfSdMode3 *s = opaque;
    /* Four native-read cache words bound in the native SDK extension notes.
     * Their UHS-II interpretation remains unqualified; unknown extension
     * operations stop rather than completing an unimplemented command. */
    static const uint32_t caps[] = {0x00060188, 0x00120040, 0x80004000, 0};
    if (size == 4 && addr >= 0x50 && addr <= 0x5c && !(addr & 3)) {
        s->extension_reads++;
        return caps[(addr - 0x50) / 4];
    }
    s->unsupported++;
    error_report("ZF_SD_MODE3: unsupported extension read +%x/%u", (unsigned)addr+0x100, size);
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
    return 0;
}

static void zf_sd_mode3_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ZfSdMode3 *s = opaque;
    s->unsupported++;
    error_report("ZF_SD_MODE3: unsupported extension write +%x/%u", (unsigned)addr+0x100, size);
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
}

static const MemoryRegionOps zf_sd_mode3_ops = {
    .read = zf_sd_mode3_read, .write = zf_sd_mode3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 1, .max_access_size = 4},
    .impl = {.min_access_size = 1, .max_access_size = 4},
};

static void zf_sd_mode3_realize(PCIDevice *dev, Error **errp)
{
    ERRP_GUARD();
    ZfSdMode3 *s = (ZfSdMode3 *)dev;
    SDHCIState *host = &s->parent_obj;
    sdhci_initfn(host);
    sdhci_common_realize(host, errp);
    if (*errp) return;
    dev->config[PCI_CLASS_PROG] = 1;
    dev->config[PCI_INTERRUPT_PIN] = 1;
    host->irq = qemu_allocate_irq(zf_sd_mode3_irq, s, 0);
    host->dma_as = pci_get_address_space(dev);
    if (host->dma_mr) {
        host->dma_as = &host->sysbus_dma_as;
        address_space_init(host->dma_as, host->dma_mr, "zf-mode3-dma");
    }
    /* Allocate a genuine PCIe/AER layout away from vendor64. Capability
     * position is a model choice, not a captured physical configuration. */
    if (pcie_endpoint_cap_init(dev, 0x80) < 0) {
        error_setg(errp, "Cannot initialize Zf mode3 PCIe capability");
        sdhci_pci_exit(dev); return;
    }
    pcie_cap_deverr_init(dev);
    if (pcie_aer_init(dev, PCI_ERR_VER, 0x100, PCI_ERR_SIZEOF, errp) < 0) {
        pcie_cap_exit(dev); sdhci_pci_exit(dev); return;
    }
    /* Virtual single-vector MSI, outside vendor64 and PCIe80. Delivery
     * uses the PCI bus address space and the root's programmed MSI target;
     * it must not use the SD payload/SRAM DMA translation. */
    if (msi_init(dev, 0x48, 1, true, true, errp) < 0) {
        pcie_aer_exit(dev); pcie_cap_exit(dev); sdhci_pci_exit(dev); return;
    }
    /* Register one BAR container containing the original controller core. */
    memory_region_init(&s->bar, OBJECT(dev), "zf-sd-mode3-bar", 0x200);
    memory_region_init_io(&s->core, OBJECT(dev), &zf_sd_mode3_core_ops,
                          s, "zf-sd-mode3-core", 0x100);
    memory_region_init_io(&s->extension, OBJECT(dev), &zf_sd_mode3_ops,
                          s, "zf-sd-mode3-extension", 0x100);
    memory_region_add_subregion(&s->bar, 0, &s->core);
    memory_region_add_subregion(&s->bar, 0x100, &s->extension);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
    /* Bounded native vendor RMW fields. No blanket writable config RAM. */
    static const unsigned offsets[] = {0xd0,0xf0,0xfc,0x300,0x304,0x384,
        0x354,0x3d4,0x3e4,0x3f0,0x428,0x42c,0x434};
    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
        pci_set_long(dev->wmask + offsets[i], UINT32_MAX);
    }
    pci_set_long(dev->wmask + 0x64, 0);
}

static uint32_t zf_sd_mode3_config_read(PCIDevice *dev, uint32_t addr, int size)
{
    ZfSdMode3 *s = (ZfSdMode3 *)dev;
    if (addr <= 0x64 && addr + size > 0x64) {
        /* Functional clock acknowledgements derived from the core. Native
         * setup enables internal clock before polling mask4, then bit3
         * before polling mask8. This timing model is explicit, not a
         * measured PLL or a constant ready response. */
        uint32_t ack = (s->parent_obj.clkcon & 3) == 3 ? 4 : 0;
        if (ack && (s->parent_obj.clkcon & 8)) ack |= 8;
        pci_set_long(dev->config + 0x64, ack);
        s->config_reads++;
    }
    return pci_default_read_config(dev, addr, size);
}

static void zf_sd_mode3_config_write(PCIDevice *dev, uint32_t addr, uint32_t value, int size)
{
    /* D3.bit7 protects the native PLL settings. PCI Command/BAR/AER remain
     * governed by PCI core, including bus master and INTx-disable gates. */
    if ((pci_get_long(dev->config+0xd0) & 0x80000000u) &&
        ((addr >= 0x300 && addr < 0x308) || (addr >= 0x380 && addr < 0x388))) return;
    pci_default_write_config(dev, addr, value, size);
    pcie_aer_write_config(dev, addr, value, size);
    zf_sd_mode3_irq(dev, 0, ((ZfSdMode3 *)dev)->irq_level);
}

static void zf_sd_mode3_exit(PCIDevice *dev)
{
    ZfSdMode3 *s = (ZfSdMode3 *)dev;
    msi_uninit(dev); pcie_aer_exit(dev); pcie_cap_exit(dev);
    memory_region_del_subregion(&s->bar, &s->core);
    memory_region_del_subregion(&s->bar, &s->extension);
    sdhci_pci_exit(dev);
}

static char *zf_sd_mode3_state(Object *obj, Error **errp)
{
    ZfSdMode3 *s = (ZfSdMode3 *)obj;
    return g_strdup_printf("{\"clock_config_reads\":%" PRIu64
        ",\"extension_reads\":%" PRIu64 ",\"unsupported\":%" PRIu64
        ",\"intx_assertions\":%" PRIu64 ",\"msi_messages\":%" PRIu64
        ",\"clock_ack_model\":true,\"physical_identity_verified\":false}",
        s->config_reads, s->extension_reads, s->unsupported,
        s->intx_assertions, s->msi_messages);
}

static void zf_sd_mode3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    device_class_set_parent_realize(dc, zf_sd_mode3_device_realize, &zf_sd_mode3_parent_realize);
    pc->realize = zf_sd_mode3_realize; pc->exit = zf_sd_mode3_exit;
    pc->config_read = zf_sd_mode3_config_read;
    pc->config_write = zf_sd_mode3_config_write;
    dc->hotpluggable = false;
    zf_sd_mode3_parent_reset = dc->legacy_reset;
    device_class_set_legacy_reset(dc, zf_sd_mode3_reset);
    object_class_property_add_str(klass, "zf-mode3-state", zf_sd_mode3_state, NULL);
}

static const TypeInfo zf_sd_mode3_type = {
    .name = TYPE_ZF_SD_MODE3, .parent = TYPE_PCI_SDHCI,
    .instance_size = sizeof(ZfSdMode3), .class_init = zf_sd_mode3_class_init,
};
static void zf_sd_mode3_register(void) { type_register_static(&zf_sd_mode3_type); }
type_init(zf_sd_mode3_register)
