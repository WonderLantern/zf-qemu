/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Own13 functions: two independent AUX banks, channel0 only.
 * 44 ordinary configuration words,2 IRQ masks,2 pending/W1C model words.
 */
#ifndef HW_ARM_NIKON_ZF_SENSORIF_AUX_CONFIG_H
#define HW_ARM_NIKON_ZF_SENSORIF_AUX_CONFIG_H
static bool zf_sensorif_aux_pending_address(uint64_t addr)
{
    return addr == UINT64_C(0x15085108) || addr == UINT64_C(0x15085508);
}
static bool zf_sensorif_aux_address_verified(uint64_t addr)
{
    unsigned off;
    if (addr & 3) { return false; }
    if (addr >= UINT64_C(0x15085000) && addr <= UINT64_C(0x15085150)) {
        off = addr - UINT64_C(0x15085000);
    } else if (addr >= UINT64_C(0x15085400) && addr <= UINT64_C(0x15085550)) {
        off = addr - UINT64_C(0x15085400);
    } else { return false; }
    switch (off) {
    case 0x0:
    case 0x4:
    case 0x8:
    case 0xc:
    case 0x14:
    case 0x18:
    case 0x1c:
    case 0x20:
    case 0x28:
    case 0x100:
    case 0x104:
    case 0x108:
    case 0x10c:
    case 0x114:
    case 0x120:
    case 0x124:
    case 0x128:
    case 0x12c:
    case 0x130:
    case 0x134:
    case 0x138:
    case 0x13c:
    case 0x140:
    case 0x150:
        return true;
    default: return false;
    }
}
#endif
