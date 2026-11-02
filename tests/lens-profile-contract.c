/* Synthetic lens payload bounds/encoding contract; no firmware required.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <assert.h>
#include <string.h>
#include "../qemu/hw/arm/nikon-zf-lens-profile.h"

int main(void)
{
    ZfLensProfile profile = { true, 50, 100 };
    uint8_t block[36], original[36];
    memset(block, 0x5a, sizeof(block));
    memcpy(original, block, sizeof(block));
    assert(!zf_lens_profile_reply(&profile, 0x2670, block + 1, 30));
    assert(!memcmp(block, original, sizeof(block)));
    assert(!zf_lens_profile_reply(&profile, 0x2870, block + 1, 29));
    assert(!memcmp(block, original, sizeof(block)));
    assert(zf_lens_profile_reply(&profile, 0x2870, block + 1, 30));
    assert(block[21] == 80 && block[25] == 80 && block[26] == 80);
    assert(block[27] == 0 && block[28] == 0 && block[30] == 0);
    assert(block[0] == 0x5a && block[31] == 0x5a);
    assert(!zf_lens_profile_valid(5, 100));
    assert(!zf_lens_profile_valid(1201, 100));
    assert(!zf_lens_profile_valid(50, 110));
    for (unsigned focal = 6; focal <= 1200; focal++) {
        unsigned code = zf_lens_focal_code(focal);
        double encoded = 5 * pow(2, code / 24.0);
        assert(fabs(encoded / focal - 1) < 0.015);
        if (focal > 6) {
            assert(code >= zf_lens_focal_code(focal - 1));
        }
        for (size_t i = 0; i < sizeof(zf_lens_apertures) / sizeof(unsigned); i++) {
            profile.focal_mm = focal;
            profile.aperture_x100 = zf_lens_apertures[i];
            assert(zf_lens_profile_valid(focal, profile.aperture_x100));
            assert(zf_lens_profile_reply(&profile, 0x2870, block + 1, 30));
            assert(block[21] == code && block[25] == code && block[26] == code);
            assert(block[27] == zf_lens_aperture_code(profile.aperture_x100));
            assert(block[27] == block[28] && block[27] == block[30]);
        }
    }
    assert(zf_lens_aperture_code(1600) == 96);
    assert(zf_lens_aperture_code(220) == 28);
    assert(zf_lens_aperture_code(350) == 44);
    profile.enabled = false;
    assert(!zf_lens_profile_reply(&profile, 0x2870, block + 1, 30));
    return 0;
}
