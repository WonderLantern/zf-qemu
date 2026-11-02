/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit external input experiment for own GPIO descriptor302/group22/bit8.
 * This is not a card model or a DCF state override. Disabled by default.
 */
#ifndef NIKON_ZF_GPIO_INPUT_H
#define NIKON_ZF_GPIO_INPUT_H
#include <stdint.h>
#include <stdbool.h>
static inline uint16_t zf_gpio22_bit8_word(bool is_zf, bool drive_high,
                                         bool drive_low, uint64_t offset, uint16_t value)
{
    if (!is_zf || offset != UINT64_C(0x58)) return value;
    if (drive_high) return (uint16_t)(value | UINT16_C(0x100));
    if (drive_low) return (uint16_t)(value & ~UINT16_C(0x100));
    return value;
}
#endif
