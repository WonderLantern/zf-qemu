/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included by sdhci-pci.c: reuse its complete SD controller implementation.
 * This explicit probe only exposes four historical, native-read cache words.
 * It is not a physical controller identity, reset profile or UHS-II link. */
#include "system/runstate.h"
#include "qemu/error-report.h"
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_aer.h"

#define TYPE_ZF_SDHCI_CAP_PROBE "nikon-zf-sdhci-cap-probe"
typedef struct ZfSdhciCapProbe {
    SDHCIState parent_obj;
    MemoryRegion bar, extension;
    bool historical_capabilities;
    bool pcie_config;
    bool vendor_clock_retention;
    bool vendor_config_write_protect;
    uint64_t vendor_config_blocked_writes;
    uint64_t clock_config_writes;
    uint64_t config64_reads, config64_clock_disabled, config64_clock_unstable, config64_clock_stable;
    uint32_t config64_last;
    uint16_t config64_last_clock;
    uint8_t config64_last_power;
    uint64_t capability_reads, unsupported;
} ZfSdhciCapProbe;

static DeviceRealize zf_sdhci_probe_parent_realize;
static void zf_sdhci_probe_device_realize(DeviceState *dev, Error **errp)
{
    ZfSdhciCapProbe *s = (ZfSdhciCapProbe *)dev;
    /* The parent has a conventional PCI interface. Select allocation size
     * before its PCI DeviceState realize allocates configuration arrays. */
    if (s->pcie_config) {
        PCI_DEVICE(dev)->cap_present |= QEMU_PCI_CAP_EXPRESS;
    }
    zf_sdhci_probe_parent_realize(dev, errp);
}

static const uint32_t zf_sdhci_historical_capabilities[4] = {
    0x00060188, 0x00120040, 0x80004000, 0x00000000,
};

static void zf_sdhci_probe_unsupported(ZfSdhciCapProbe *s, hwaddr offset,
                                       unsigned size, bool write)
{
    s->unsupported++;
    error_report("ZF_SD_CAP_PROBE: unsupported %s BAR0+0x%" HWADDR_PRIx
                 " size=%u; stop, no success/status response",
                 write ? "write" : "read", offset + 0x100, size);
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
}

static uint64_t zf_sdhci_probe_read(void *opaque, hwaddr addr, unsigned size)
{
    ZfSdhciCapProbe *s = opaque;
    if (size != 4 || addr < 0x50 || addr > 0x5c || (addr & 3)) {
        zf_sdhci_probe_unsupported(s, addr, size, false);
        return 0;
    }
    s->capability_reads++;
    return zf_sdhci_historical_capabilities[(addr - 0x50) / 4];
}

static void zf_sdhci_probe_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    zf_sdhci_probe_unsupported(opaque, addr, size, true);
}

static const MemoryRegionOps zf_sdhci_probe_ops = {
    .read = zf_sdhci_probe_read,
    .write = zf_sdhci_probe_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void zf_sdhci_probe_realize(PCIDevice *dev, Error **errp)
{
    ERRP_GUARD();
    ZfSdhciCapProbe *s = (ZfSdhciCapProbe *)dev;
    SDHCIState *host = &s->parent_obj;
    if (!s->historical_capabilities) {
        error_setg(errp, "Zf SD capability probe requires historical-capabilities=on; not a qualified controller");
        return;
    }
    if (s->vendor_clock_retention && !s->pcie_config) {
        error_setg(errp, "vendor-clock-retention requires pcie-config=on for extended configuration allocation");
        return;
    }
    if (s->vendor_config_write_protect && !s->vendor_clock_retention) {
        error_setg(errp, "vendor-config-write-protect requires vendor-clock-retention=on");
        return;
    }
    sdhci_initfn(host);
    sdhci_common_realize(host, errp);
    if (*errp) {
        return;
    }
    if (s->pcie_config) {
        if (pcie_endpoint_cap_init(dev, 0x60) < 0) {
            error_setg(errp, "Cannot initialize probe PCIe capability");
            sdhci_common_unrealize(host); sdhci_uninitfn(host); return;
        }
        pcie_cap_deverr_init(dev);
        if (pcie_aer_init(dev, PCI_ERR_VER, 0x100, PCI_ERR_SIZEOF, errp) < 0) {
            pcie_cap_exit(dev);
            sdhci_common_unrealize(host); sdhci_uninitfn(host); return;
        }
    }
    dev->config[PCI_CLASS_PROG] = 0x01;
    dev->config[PCI_INTERRUPT_PIN] = 0x01;
    host->irq = pci_allocate_irq(dev);
    host->dma_as = pci_get_address_space(dev);
    memory_region_init(&s->bar, OBJECT(dev), "zf-sd-probe-bar", 0x200);
    memory_region_init_io(&s->extension, OBJECT(dev), &zf_sdhci_probe_ops,
                          s, "zf-sd-probe-extension", 0x100);
    memory_region_add_subregion(&s->bar, 0, &host->iomem);
    memory_region_add_subregion(&s->bar, 0x100, &s->extension);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
    error_report("ZF_SD_CAP_PROBE: historical150..15c only; generic SDHCI below100; UHS-II/controller identity unqualified");
}

static void zf_sdhci_probe_exit(PCIDevice *dev)
{
    ZfSdhciCapProbe *s = (ZfSdhciCapProbe *)dev;
    if (s->pcie_config) {
        pcie_aer_exit(dev);
        pcie_cap_exit(dev);
    }
    memory_region_del_subregion(&s->bar, &s->parent_obj.iomem);
    memory_region_del_subregion(&s->bar, &s->extension);
    sdhci_pci_exit(dev);
}

static char *zf_sdhci_probe_state(Object *obj, Error **errp)
{
    ZfSdhciCapProbe *s = (ZfSdhciCapProbe *)obj;
    return g_strdup_printf("{\"historical_capabilities\":%s,\"reads\":%" PRIu64
        ",\"unsupported\":%" PRIu64 ",\"pcie_config\":%s,\"uhs2_qualified\":false}",
        s->historical_capabilities ? "true" : "false", s->capability_reads, s->unsupported,
        s->pcie_config ? "true" : "false");
}

static void zf_sdhci_probe_config_write(PCIDevice *dev, uint32_t address,
                                       uint32_t value, int size)
{
    ZfSdhciCapProbe *s = (ZfSdhciCapProbe *)dev;
    /* Explicit family-hypothesis option: own d0.bit31 sequencing matches
     * cached O2Micro D3.bit7 unlock/write/lock. Only scoped vendor fields
     * are gated; never PCI Command, BARs, AER or a synthesized ready flag. */
    if (s->vendor_config_write_protect && size == 1 && address == 0xd3) {
        uint32_t previous = pci_get_long(dev->config+0xd0);
        pci_set_long(dev->config+0xd0, (previous & 0x7fffffffu) | ((value & 0x80u)<<24));
        s->clock_config_writes++;
        return;
    }
    if (s->vendor_config_write_protect && size == 4 &&
        (address == 0x304 || address == 0x384 || address == 0x354 || address == 0x3d4) &&
        (pci_get_long(dev->config+0xd0) & 0x80000000u)) {
        s->vendor_config_blocked_writes++;
        return;
    }
    if (s->vendor_clock_retention && size == 4 &&
        (address == 0xd0 || address == 0x304 || address == 0x384)) {
        /* Clock helper updates high16; earlier own initialization also
         * programs304.low using(old&ffff8881)|(source&777e).
         * No low384 fields or config64 ready behavior are inferred. */
        uint32_t mask = address == 0xd0 ? 0x80000000u :
                        address == 0x304 ? 0xffff777eu : 0xffff0000u;
        uint32_t previous = pci_get_long(dev->config + address);
        pci_set_long(dev->config + address, (previous & ~mask) | (value & mask));
        s->clock_config_writes++;
        return;
    }
    if (s->vendor_clock_retention && size == 4 &&
        (address == 0x354 || address == 0x3d4) && value == 0) {
        /* Own reset writes zero here. Do not infer reset completion. */
        s->clock_config_writes++;
        return;
    }
    pci_default_write_config(dev, address, value, size);
    if (((ZfSdhciCapProbe *)dev)->pcie_config) {
        pcie_aer_write_config(dev, address, value, size);
    }
}

static uint32_t zf_sdhci_probe_config_read(PCIDevice *dev, uint32_t address, int size)
{
    ZfSdhciCapProbe *s = (ZfSdhciCapProbe *)dev;
    uint32_t value = pci_default_read_config(dev, address, size);
    if (address == 0x64 && size == 4) {
        /* Same-call latch observation: do not perform extra MMIO reads or
         * change any controller bit. Includes debugger configuration reads. */
        uint16_t clock = s->parent_obj.clkcon;
        uint8_t power = s->parent_obj.pwrcon;
        if (!s->config64_reads || clock != s->config64_last_clock ||
            power != s->config64_last_power || value != s->config64_last) {
            error_report("ZF_SD_CONFIG64_OBSERVE: config=%08x clock=%04x power=%02x; no state injection",
                         value, clock, power);
        }
        s->config64_reads++;
        if (!(clock & 1)) s->config64_clock_disabled++;
        else if (!(clock & 2)) s->config64_clock_unstable++;
        else s->config64_clock_stable++;
        s->config64_last=value;
        s->config64_last_clock=clock;
        s->config64_last_power=power;
    }
    return value;
}

static char *zf_sdhci_config64_observation(Object *obj, Error **errp)
{
    ZfSdhciCapProbe *s = (ZfSdhciCapProbe *)obj;
    return g_strdup_printf("{\"reads\":%" PRIu64 ",\"clock_disabled\":%" PRIu64
        ",\"clock_unstable\":%" PRIu64 ",\"clock_stable\":%" PRIu64
        ",\"last_config\":%u,\"last_clock\":%u,\"last_power\":%u,"
        "\"includes_debugger_reads\":true}",s->config64_reads,s->config64_clock_disabled,
        s->config64_clock_unstable,s->config64_clock_stable,s->config64_last,
        s->config64_last_clock,s->config64_last_power);
}

static char *zf_sdhci_clock_state(Object *obj, Error **errp)
{
    ZfSdhciCapProbe *s = (ZfSdhciCapProbe *)obj;
    PCIDevice *dev = PCI_DEVICE(obj);
    return g_strdup_printf("{\"enabled\":%s,\"writes\":%" PRIu64
        ",\"commit\":%u,\"pll0_high16\":%u,\"pll1_high16\":%u,"
        "\"ready_synthesized\":false}", s->vendor_clock_retention ? "true" : "false",
        s->clock_config_writes, s->pcie_config ? pci_get_long(dev->config+0xd0)>>31 : 0,
        s->pcie_config ? pci_get_long(dev->config+0x304)>>16 : 0,
        s->pcie_config ? pci_get_long(dev->config+0x384)>>16 : 0);
}

static const Property zf_sdhci_probe_properties[] = {
    DEFINE_PROP_BOOL("historical-capabilities", ZfSdhciCapProbe, historical_capabilities, false),
    DEFINE_PROP_BOOL("pcie-config", ZfSdhciCapProbe, pcie_config, false),
    DEFINE_PROP_BOOL("vendor-clock-retention", ZfSdhciCapProbe, vendor_clock_retention, false),
    DEFINE_PROP_BOOL("vendor-config-write-protect", ZfSdhciCapProbe, vendor_config_write_protect, false),
};

static char *zf_sdhci_config_protection(Object *obj, Error **errp)
{
    ZfSdhciCapProbe *s=(ZfSdhciCapProbe *)obj;
    return g_strdup_printf("{\"enabled\":%s,\"locked\":%s,\"blocked_writes\":%" PRIu64
        ",\"physical_identity_verified\":false}",s->vendor_config_write_protect?"true":"false",
        s->pcie_config && (pci_get_long(PCI_DEVICE(obj)->config+0xd0)&0x80000000u)?"true":"false",
        s->vendor_config_blocked_writes);
}

static void zf_sdhci_probe_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_parent_realize(dc, zf_sdhci_probe_device_realize,
                                   &zf_sdhci_probe_parent_realize);
    pc->realize = zf_sdhci_probe_realize;
    pc->exit = zf_sdhci_probe_exit;
    pc->config_write = zf_sdhci_probe_config_write;
    pc->config_read = zf_sdhci_probe_config_read;
    dc->hotpluggable = false;
    device_class_set_props(dc, zf_sdhci_probe_properties);
    object_class_property_add_str(klass, "zf-probe-state", zf_sdhci_probe_state, NULL);
    object_class_property_add_str(klass, "zf-clock-config-state", zf_sdhci_clock_state, NULL);
    object_class_property_add_str(klass, "zf-config64-observation", zf_sdhci_config64_observation, NULL);
    object_class_property_add_str(klass, "zf-config-protection-state", zf_sdhci_config_protection, NULL);
}

static const TypeInfo zf_sdhci_probe_type = {
    .name = TYPE_ZF_SDHCI_CAP_PROBE,
    .parent = TYPE_PCI_SDHCI,
    .instance_size = sizeof(ZfSdhciCapProbe),
    .class_init = zf_sdhci_probe_class_init,
};

static void zf_sdhci_probe_register_types(void)
{
    type_register_static(&zf_sdhci_probe_type);
}
type_init(zf_sdhci_probe_register_types)
