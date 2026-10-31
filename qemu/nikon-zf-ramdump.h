/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit diagnostic compatibility for Zf C3.01 only.
 * Original load40c07cc8 copies registered MMIO spans to record54. Missing
 * words are declared synthetic zero, never a normal status response.
 * These eleven spans are the current native descriptors in
 * helperdumpregions01 (table SHA2569983ec64...19fea6), not a register map.
 * Normal reads, writes, unaligned accesses and other PCs remain strict.
 */
#ifndef HW_ARM_NIKON_ZF_RAMDUMP_H
#define HW_ARM_NIKON_ZF_RAMDUMP_H
#include <stdbool.h>
#include <stdint.h>

static inline bool zf_ramdump_read_allowed(bool is_zf, bool enabled,
                                         uint64_t pc, uint64_t address,
                                         unsigned size)
{
    static const struct { uint64_t base; uint32_t bytes; } spans[] = {
        {0x15080000, 0x3c4}, {0x15081000, 0x490}, {0x15084000, 0x148},
        {0x15083000, 0x554}, {0x15085000, 0x300}, {0x15085400, 0x300},
        {0x150c8000, 0x3000}, {0x150cb000, 0x3000},
        {0x150ce000, 0x400}, {0x150ce400, 0x400}, {0x15280000, 0xfd0},
    };
    if (!is_zf || !enabled || pc != UINT64_C(0x40c07cc8) ||
        size != 4 || (address & 3)) {
        return false;
    }
    for (unsigned i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
        if (address >= spans[i].base &&
            address - spans[i].base <= spans[i].bytes - 4) {
            return true;
        }
    }
    return false;
}
#endif
