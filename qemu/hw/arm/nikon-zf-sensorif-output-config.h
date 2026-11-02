/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Complete own 11-function/256-byte configuration union; excludes commands/status. */
#ifndef HW_ARM_NIKON_ZF_SENSORIF_OUTPUT_CONFIG_H
#define HW_ARM_NIKON_ZF_SENSORIF_OUTPUT_CONFIG_H
static const unsigned zf_sensorif_output_offsets[] = {
    0x3420, 0x3424, 0x3428, 0x342c, 0x3430, 0x3434, 0x3438, 0x343c,
    0x3440, 0x3444, 0x3448, 0x353c, 0x3540, 0x3544, 0x3550,
};
static inline bool zf_sensorif_output_address_verified(hwaddr addr)
{
    for (unsigned i = 0; i < sizeof(zf_sensorif_output_offsets) /
                                  sizeof(zf_sensorif_output_offsets[0]); i++) {
        if (addr == UINT64_C(0x15080000) + zf_sensorif_output_offsets[i]) {
            return true;
        }
    }
    return false;
}
#endif
