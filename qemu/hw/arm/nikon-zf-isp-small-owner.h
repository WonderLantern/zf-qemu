/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact whole195/197/198 ordinary masks, generated from own23 writers. */
#ifndef NIKON_ZF_ISP_SMALL_OWNER_H
#define NIKON_ZF_ISP_SMALL_OWNER_H
static inline bool zf_isp_small_aperture(hwaddr addr)
{
    return addr >= 0x150d5000 && addr < 0x150d5300;
}
static inline bool zf_isp_small_config_address(hwaddr addr)
{
    static const uint64_t bits[3] = {UINT64_C(0x000000ffe71ffff3), UINT64_C(0x00013ffffffff1ff), UINT64_C(0x00000133fe7fffff)};
    if (!zf_isp_small_aperture(addr) || (addr & 3)) {
        return false;
    }
    return (bits[(addr - 0x150d5000) / 0x100] >> ((addr & 0xff) / 4)) & 1;
}
#endif
