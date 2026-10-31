/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Three own channels, full48 ordinary words each plus explicit donor empty-only
 * reset/stop/W1C/query contracts. No fallback for other old board zero words. */
#ifndef NIKON_ZF_STREAM154C_H
#define NIKON_ZF_STREAM154C_H
static inline bool zf_stream154c_aperture(hwaddr addr)
{
    return addr >= 0x154c0000 && addr < 0x154c0c00;
}
static inline bool zf_stream154c_config_address(hwaddr addr)
{
    static const uint32_t bits[8] = {0x00000048u, 0x00000000u, 0x00000042u, 0x00000000u, 0xffff0092u, 0x00000007u, 0xffff0092u, 0x00000007u};
    unsigned off = addr & 0x3ff;
    return zf_stream154c_aperture(addr) && !(addr & 3) &&
           (bits[off / 128] & (1u << ((off / 4) % 32)));
}
static inline bool zf_stream154c_read_address(hwaddr addr)
{
    static const uint32_t bits[8] = {0x00000000u, 0x00000000u, 0x00000005u, 0x00000000u, 0x000001a1u, 0x00000000u, 0x000001a1u, 0x00000000u};
    unsigned off = addr & 0x3ff;
    return zf_stream154c_aperture(addr) && !(addr & 3) &&
           (bits[off / 128] & (1u << ((off / 4) % 32)));
}
static inline bool zf_stream154c_write_address(hwaddr addr)
{
    static const uint32_t bits[8] = {0x00000078u, 0x00000000u, 0x0000004fu, 0x00000000u, 0xffff00bbu, 0x0000000fu, 0xffff00bbu, 0x0000000fu};
    unsigned off = addr & 0x3ff;
    return zf_stream154c_aperture(addr) && !(addr & 3) &&
           (bits[off / 128] & (1u << ((off / 4) % 32)));
}
#endif
