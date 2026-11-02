/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Complete own9 writers/51 fields each. Results and270 remain separate. */
#ifndef NIKON_ZF_ISP192_OWNER_H
#define NIKON_ZF_ISP192_OWNER_H
static inline bool zf_isp192_aperture(hwaddr addr)
{
    return addr >= 0x150ce000 && addr < 0x150ce800;
}
static inline bool zf_isp192_owner_offset(unsigned off)
{
    return !(off & 3) && (off < 0x70 || off == 0x10c ||
                         (off >= 0x200 && off < 0x258));
}
#endif
