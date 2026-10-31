/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Own core/lane CFG:107core+220lane+129LUT words per registered bank.
 * 1218/base0 controlbits,1200..1214 readback and engine submits excluded. */
#ifndef HW_ARM_NIKON_ZF_ISP1A3_CONFIG_H
#define HW_ARM_NIKON_ZF_ISP1A3_CONFIG_H
static inline bool zf_isp1a3_config_address(hwaddr addr)
{
    if ((addr & 3) || addr < 0x150d0000 || addr >= 0x150d4000) {
        return false;
    }
    unsigned off = (addr - 0x150d0000) & 0x1fff;
    if (off <= 0x1a8 || (off >= 0x1300 && off <= 0x1500)) {
        return true;
    }
    if (off >= 0x200 && off < 0x1200) {
        unsigned lane = (off >> 8) - 2;
        switch (off & 0xff) {
        case 0x10: case 0x14: case 0x3c:
        case 0x40: case 0x44: case 0x48: case 0x4c:
        case 0x50: case 0x54: case 0x70: case 0x74: case 0xa8:
            return true;
        case 0x20:
            return lane != 6 && lane != 7 && lane != 11 && lane != 12 && lane < 14;
        case 0x58: case 0x60: case 0x68: case 0x78: case 0x80: case 0x88:
            return lane >= 13;
        }
    }
    return false;
}
#endif
