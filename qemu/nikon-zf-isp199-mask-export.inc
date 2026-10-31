/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pure configuration-mask query from the cached Z9 implementation.
 * No NikonZ9State object, bootstrap fixture, IRQ, or DMA action is involved.
 * The caller separately restricts offsets to complete Zf source contracts.
 * Masks describe original writer output bounds, not physical reserved bits.
 */
uint32_t z63_z9lab_isp199_config_mask(unsigned off);

uint32_t z63_z9lab_isp199_config_mask(unsigned off)
{
    uint32_t mask;

    if (off & 3) {
        return 0;
    }
    mask = engine150c_primary_mask(off);
    if (mask) {
        return mask;
    }
    /* Same mask branches as engine_150c_upper_word, with an offset argument. */
    if (off >= 0x1324 && off <= 0x1344) {
        static const uint32_t masks[9] = {
            0x30103, 0x3fff, 0x3ff03ff, 0xff00ff, 0xff00ff,
            0xff00ff, 0xff, 0x300ff, 0xff01ff,
        };
        return masks[(off - 0x1324) / 4];
    }
    if (off >= 0x1348 && off <= 0x1434) {
        static const uint32_t tail_masks[12] = {
            0x3f003f, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
            0x1fff, 0x1fff3fff, 3, 1, 1, 0x3f, 0x1fff,
        };
        unsigned i = (off - 0x1348) / 4;
        return i < 48 ? UINT32_MAX : tail_masks[i - 48];
    }
    if (off >= 0x1e00 && off <= 0x1f0c) {
        return ((off - 0x1e00) / 4) % 17 == 16 ? 0xffff : UINT32_MAX;
    }
    return 0;
}
