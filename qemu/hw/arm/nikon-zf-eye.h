/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NIKON_ZF_EYE_H
#define NIKON_ZF_EYE_H
#include <stdbool.h>
#include <stdint.h>
/* Own GPIO44 descriptor a028092d8; 40c95634/40c95714 and task40c951a4.
 * Near is digital low on port3/bit13; interrupt is EXT_INT13 / INTID68.
 * This is an external digital signal, not optical distance or sensor power. */
#define ZF_EYE_WORD UINT64_C(0x00c)
#define ZF_EYE_BIT 13
#define ZF_EYE_EXTINT 13
static inline uint16_t zf_eye_gpio_word(bool enabled, bool near,
                                       uint64_t offset, uint16_t value)
{
    if (enabled && offset==ZF_EYE_WORD) {
        value=(value & ~(1u<<ZF_EYE_BIT)) | ((uint16_t)!near<<ZF_EYE_BIT);
    }
    return value;
}
#endif
