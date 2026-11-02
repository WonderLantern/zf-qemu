/* Declared optical inputs for the synthetic Zf lens peer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_NIKON_ZF_LENS_PROFILE_H
#define HW_ARM_NIKON_ZF_LENS_PROFILE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

typedef struct ZfLensProfile {
    bool enabled;
    unsigned focal_mm, aperture_x100;
} ZfLensProfile;

static const unsigned zf_lens_apertures[] = {
    100, 120, 140, 160, 180, 200, 220, 250, 280, 320, 350, 400,
    500, 560, 630, 710, 800, 900, 1000, 1100, 1300, 1400, 1600
};

static inline bool zf_lens_profile_valid(unsigned focal, unsigned aperture)
{
    if (focal < 6 || focal > 1200) {
        return false;
    }
    for (size_t i = 0; i < sizeof(zf_lens_apertures) / sizeof(unsigned); i++) {
        if (aperture == zf_lens_apertures[i]) {
            return true;
        }
    }
    return false;
}

/* Wire codes are logarithmic and quantized, not integer millimeters. */
static inline uint8_t zf_lens_focal_code(unsigned focal)
{
    return lround(24 * log2(focal / 5.0));
}

static inline uint8_t zf_lens_aperture_code(unsigned aperture_x100)
{
    /* Engraved f-numbers are rounded labels for Av steps. Taking log2 of
     * the printed decimal would send f/2.2 as code 27 (shown as f/2.1),
     * and f/3.5 as code 43. Use the nominal stops, not rounded decimals.
     */
    static const uint8_t codes[] = {
        0, 6, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48,
        56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96
    };
    for (size_t i = 0; i < sizeof(zf_lens_apertures) / sizeof(unsigned); i++) {
        if (aperture_x100 == zf_lens_apertures[i]) {
            return codes[i];
        }
    }
    return 0; /* Invalid settings are rejected before reaching the peer. */
}

/* Existing peer's 2870 lens-data layout, before framing/checksum. A selected
 * f-number is a declared fixed iris: both aperture limits and the aperture
 * field agree. This does not implement a lens motor or overwrite guest state.
 * It is an experimental input layout, not a hardware-qualified Zf protocol.
 */
static inline bool zf_lens_profile_reply(const ZfLensProfile *profile,
                                       uint16_t command, uint8_t *payload,
                                       size_t capacity)
{
    if (!profile->enabled || command != 0x2870 || capacity < 30) {
        return false;
    }
    payload[20] = payload[24] = payload[25] =
        zf_lens_focal_code(profile->focal_mm);
    payload[26] = payload[27] = payload[29] =
        zf_lens_aperture_code(profile->aperture_x100);
    return true;
}

#endif
