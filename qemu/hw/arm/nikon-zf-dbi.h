/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional port2 DBI adapter, following cached WonderLantern Z9/ZR
 * nikon-z9-pcie.h. Own Zf 4028bdf0..4028bed0 selects 1a800000 and
 * programs +8bc/+4/+78. Optional mode3 SD endpoint; native INTx149/MSI150
 * routing. DesignWare identity and electrical timing remain model assumptions.
 */
#ifndef HW_ARM_NIKON_ZF_DBI_H
#define HW_ARM_NIKON_ZF_DBI_H
static PCIDevice *zf_dbi_root(NikonZ63State *s)
{
    return PCI_DEVICE(&DESIGNWARE_PCIE_HOST(s->zf_pcie_host)->root);
}

static uint32_t zf_pcie_link_input(NikonZ63State *s)
{
    PCIBus *bus=&PCI_BRIDGE(zf_dbi_root(s))->sec_bus;
    bool endpoint=pci_find_device(bus,pci_bus_num(bus),PCI_DEVFN(0,0));
    /* Cached Z9/ZR wrapper mapping; endpoint-present functional input,
     * not PHY training. Current Zf board has an empty downstream bus. */
    return endpoint && s->zf_pcie.word[1][1] && !s->zf_pcie.word[1][0] ? 0x11 : 0;
}

static uint64_t zf_dbi_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;
    if ((size != 1 && size != 2 && size != 4) || (addr & (size-1))) {
        z63_stop_read("ZF_PCIE_DBI", 0x1a800000+addr, size);
        return 0;
    }
    if (addr == 0x8bc && size == 4) return s->zf_dbi_ro_enable;
    /* Upstream 710/80c/72c provide generic Linux-friendly magic/ready
     * inputs. Keep them strict until own Zf consumers justify semantics. */
    /* Own 4028c6e4..c6f4 clears/sets bit17; retain that control bit,
     * without upstream's synthetic speed-change indication. */
    if (addr == 0x80c && size == 4) return s->zf_dbi_speed_control;
    if (addr == 0x710 || addr == 0x72c) {
        z63_stop_read("ZF_PCIE_DBI_STATUS", 0x1a800000+addr, size);
        return 0;
    }
    return pci_host_config_read_common(zf_dbi_root(s), addr, 4096, size);
}

static void zf_dbi_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    NikonZ63State *s = opaque;
    if ((size != 1 && size != 2 && size != 4) || (addr & (size-1))) {
        z63_stop_write("ZF_PCIE_DBI", 0x1a800000+addr, size, value);
        return;
    }
    if (addr == 0x8bc) {
        if (size != 4 || value > 1) {
            z63_stop_write("ZF_PCIE_DBI_RO", 0x1a800000+addr, size, value);
            return;
        }
        s->zf_dbi_ro_enable = value;
        return;
    }
    if (addr == 0x80c) {
        if (size != 4 || (value & ~UINT64_C(0x20000))) {
            z63_stop_write("ZF_PCIE_SPEED_CONTROL", 0x1a800000+addr, size, value);
            return;
        }
        s->zf_dbi_speed_control = value;
        return;
    }
    if (size == 4 && (addr == 0x82c || addr == 0x830)) {
        DesignwarePCIEHost *host = DESIGNWARE_PCIE_HOST(s->zf_pcie_host);
        DesignwarePCIEMSIBank *bank = &host->root.msi.intr[0];
        if (addr == 0x82c) {
            bank->mask = value;
        } else {
            /* MSI status is W1C, never XOR: clearing an idle vector must
             * not create an interrupt. Recompute on mask changes too. */
            bank->status &= ~value;
        }
        qemu_set_irq(host->pci.msi, !!(bank->status & ~bank->mask));
        return;
    }
    pci_host_config_write_common(zf_dbi_root(s), addr, 4096, value, size);
}

/* Z9/ZR unrolled-to-viewport adapter. Own Zf 4028c8d4 onward uses the
 * same region0 offsets. These are actual upstream address-translation
 * windows, not a copied register page or a completed-transfer fixture. */
static bool zf_atu_select(hwaddr addr, unsigned size, unsigned *reg,
                          unsigned *selection)
{
    static const unsigned legacy[] = {0x904,0x908,0x90c,0x910,0x914,0x918,0x91c};
    unsigned region = addr / 0x200, off = addr % 0x100;
    if (size != 4 || (addr & 3) || region >= DESIGNWARE_PCIE_NUM_VIEWPORTS ||
        off >= sizeof(legacy)) return false;
    *reg = legacy[off/4];
    *selection = region | ((addr & 0x100) ? 1U<<31 : 0);
    return true;
}

static uint64_t zf_atu_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s=opaque;
    unsigned reg, selection;
    if (!zf_atu_select(addr,size,&reg,&selection)) {
        z63_stop_read("ZF_PCIE_ATU",0x1ab00000+addr,size);return 0;
    }
    PCIDevice *d=zf_dbi_root(s);
    PCIDeviceClass *dc=PCI_DEVICE_GET_CLASS(d);
    uint32_t previous=dc->config_read(d,0x900,4);
    dc->config_write(d,0x900,selection,4);
    uint32_t value=dc->config_read(d,reg,4);
    dc->config_write(d,0x900,previous,4);
    return value;
}

static void zf_atu_write(void *opaque, hwaddr addr, uint64_t value,unsigned size)
{
    NikonZ63State *s=opaque;
    unsigned reg,selection;
    if (!zf_atu_select(addr,size,&reg,&selection) ||
        (reg==0x904 && value!=0 && value!=4 && value!=5) ||
        (reg==0x908 && value!=0 && value!=0x80000000) ||
        ((reg==0x910 || reg==0x91c) && value>255)) {
        z63_stop_write("ZF_PCIE_ATU",0x1ab00000+addr,size,value);return;
    }
    DesignwarePCIERoot *root=&DESIGNWARE_PCIE_HOST(s->zf_pcie_host)->root;
    DesignwarePCIEViewport *v=&root->viewports[selection>>31][selection&3];
    if (reg==0x908 && value) {
        uint64_t limit=(v->base & ~UINT64_C(0xffffffff)) | v->limit;
        if (limit<v->base || v->base>=(UINT64_C(1)<<40) ||
            (selection>>31 && v->cr[0]!=0)) {
            z63_stop_write("ZF_PCIE_ATU_RANGE",0x1ab00000+addr,size,value);return;
        }
    }
    PCIDevice *d=PCI_DEVICE(root);
    PCIDeviceClass *dc=PCI_DEVICE_GET_CLASS(d);
    uint32_t previous=dc->config_read(d,0x900,4);
    dc->config_write(d,0x900,selection,4);
    dc->config_write(d,reg,value,4);
    dc->config_write(d,0x900,previous,4);
}

static const MemoryRegionOps zf_atu_ops = {
    .read=zf_atu_read,.write=zf_atu_write,.endianness=DEVICE_LITTLE_ENDIAN,
    .valid={.min_access_size=4,.max_access_size=4},
};

static const MemoryRegionOps zf_dbi_ops = {
    .read = zf_dbi_read, .write = zf_dbi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static bool zf_exs_pcie2_link(void *opaque)
{
    return zf_pcie_link_input(NIKON_Z63(opaque)) != 0;
}

static void zf_pcie_intx(void *opaque, int irq, int level)
{
    NikonZ63State *s = opaque;
    uint8_t bit = 1u << irq;
    if (level && !(s->zf_pcie.intx_levels & bit)) {
        /* Own ISR4028b4a4 reads/W1Cs bits15..12 at17b800e8. */
        s->zf_pcie.intx_status |= 1u << (15 - irq);
    }
    s->zf_pcie.intx_levels = (s->zf_pcie.intx_levels & ~bit) | (level ? bit : 0);
    qemu_set_irq(s->zf_pcie.intx_out, !!s->zf_pcie.intx_status);
}

static void zf_dbi_init(NikonZ63State *s, MemoryRegion *mem)
{
    DeviceState *dev = qdev_new(TYPE_DESIGNWARE_PCIE_HOST);
    object_property_add_child(OBJECT(s), "zf-pcie-host", OBJECT(dev));
    s->zf_pcie_host = dev;
    s->usb.zf_pcie2_link_input = zf_exs_pcie2_link;
    s->usb.zf_pcie2_link_opaque = s;
    qdev_prop_set_bit(dev, "cfg0-direct-downstream", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    /* Own registration4028b2d4/4028b2f0: port2 INTx149 and MSI150.
     * SDH40 INTID135 belongs to the separate on-SoC controller. */
    s->zf_pcie.intx_out = qdev_get_gpio_in(s->gic, 149 - 32);
    qemu_irq *intx = qemu_allocate_irqs(zf_pcie_intx, s, 4);
    for (unsigned i = 0; i < 4; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, intx[i]);
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 4, qdev_get_gpio_in(s->gic, 150 - 32));
    memory_region_init(&s->zf_pcie_outbound, OBJECT(s),
                       "zf.pcie2-outbound", UINT64_C(0x100000000));
    DesignwarePCIERoot *root = &DESIGNWARE_PCIE_HOST(dev)->root;
    for (unsigned i = 0; i < DESIGNWARE_PCIE_NUM_VIEWPORTS; i++) {
        DesignwarePCIEViewport *v =
            &root->viewports[DESIGNWARE_PCIE_VIEWPORT_OUTBOUND][i];
        memory_region_del_subregion(mem, &v->mem);
        memory_region_del_subregion(mem, &v->cfg);
        memory_region_add_subregion(&s->zf_pcie_outbound, 0, &v->mem);
        memory_region_add_subregion(&s->zf_pcie_outbound, 0, &v->cfg);
    }
    memory_region_add_subregion(mem, UINT64_C(0x600000000),
                                &s->zf_pcie_outbound);
    memory_region_init_io(&s->zf_dbi, OBJECT(s), &zf_dbi_ops, s,
                           "zf.pcie2-dbi", 0x1000);
    memory_region_add_subregion(mem, 0x1a800000, &s->zf_dbi);
    memory_region_init_io(&s->zf_atu, OBJECT(s), &zf_atu_ops,s,"zf.pcie2-atu",0x1000);
    memory_region_add_subregion(mem,0x1ab00000,&s->zf_atu);
    error_report("ZF_PCIE: functional DesignWare DBI/iATU, optional endpoint, INTx149/MSI150; physical IP/timing unverified");
}
#endif
