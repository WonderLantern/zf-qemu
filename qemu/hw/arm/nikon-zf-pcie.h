/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Selected EXPEED7 PCIe wrapper configuration, from cached Z9/ZR model and
 * own Zf 4028ba34..4028bdd8. Zero reset is an unmeasured assumption.
 * This retains writable fields only: no fabricated link, endpoint, IRQ or DMA.
 * Unknown offsets/values fall through to the board's strict stop handler.
 */
#ifndef HW_ARM_NIKON_ZF_PCIE_H
#define HW_ARM_NIKON_ZF_PCIE_H
typedef struct ZfPcieConfig {
    uint32_t word[2][7];
    uint32_t intx_status;
    uint8_t intx_levels;
    qemu_irq intx_out;
} ZfPcieConfig;

static int zf_pcie_slot(uint64_t addr, unsigned size, unsigned *bank,
                        uint32_t *mask)
{
    /* Own4028e2c0..388 selects60/260 and RMWs bits18:17 to01.
     * Retain that field only; its physical/IRQ behavior is not supplied. */
    static const unsigned offsets[] = {0xe4c, 0x58, 0x258, 0x54, 0x254, 0x60, 0x260};
    static const uint32_t masks[] = {0x707, 1, 1, 0x700, 0x700, 0x60000, 0x60000};
    if (size != 4) return -1;
    for (unsigned b = 0; b < 2; b++) {
        for (unsigned i = 0; i < 7; i++) {
            if (addr == 0x17b00000ULL + b * 0x80000 + offsets[i]) {
                *bank = b;
                *mask = masks[i];
                return i;
            }
        }
    }
    return -1;
}

static bool zf_pcie_config_read(ZfPcieConfig *s, uint64_t addr,
                                unsigned size, uint64_t *value)
{
    if (addr == UINT64_C(0x17b800e8) && size == 4) {
        *value = s->intx_status;
        return true;
    }
    unsigned bank;
    uint32_t mask;
    int i = zf_pcie_slot(addr, size, &bank, &mask);
    if (i < 0) return false;
    *value = s->word[bank][i];
    return true;
}

static bool zf_pcie_config_write(ZfPcieConfig *s, uint64_t addr,
                                 unsigned size, uint64_t value)
{
    if (addr == UINT64_C(0x17b800e8) && size == 4) {
        s->intx_status &= ~value;
        qemu_set_irq(s->intx_out, !!s->intx_status);
        return true;
    }
    unsigned bank;
    uint32_t mask;
    int i = zf_pcie_slot(addr, size, &bank, &mask);
    if (i >= 0) {
        if (value & ~(uint64_t)mask) return false;
        s->word[bank][i] = value;
        return true;
    }
    if (size != 4) return false;
    for (unsigned b = 0; b < 2; b++) {
        uint64_t base = 0x17b00000ULL + b * 0x80000;
        /* Native initialization commands; not readable status registers. */
        if ((addr == base+0xe4 || addr == base+0x2e4) && value == 0xfe010000)
            return true;
        if ((addr == base+0xe8 || addr == base+0x2e8) && value == 0xf0000000)
            return true;
    }
    return false;
}
#endif
