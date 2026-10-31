/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit diagnostic virtual MCU report, following the upstream minimal
 * family4/kind3 input. This is not a physical battery/authentication model.
 * Firmware and EX SRAM remain unchanged. Disabled by default.
 */
#ifndef NIKON_ZF_BATTERY_STANDIN_H
#define NIKON_ZF_BATTERY_STANDIN_H
#include <stdint.h>
#include <stdbool.h>

typedef struct ZfBatteryStandin {
    bool enabled;
    uint8_t raw[256], wire[256];
    uint64_t reports;
} ZfBatteryStandin;

static inline bool zf_battery_standin_selected(const ZfBatteryStandin *s)
{
    return s->enabled && s->raw[0] == 4 && s->raw[1] == 2 &&
           s->raw[2] == 112 && s->raw[3] == 0 && s->raw[8] == 3;
}

/* Called before a word is clocked; 'at' is the bridge's committed index.
 * Repeated calls at an unclocked index are harmless. Preserve the original
 * checksum residual, so a bad input frame never becomes valid by rewriting.
 */
static inline uint16_t zf_battery_standin_word(ZfBatteryStandin *s,
                                              unsigned at, uint16_t raw)
{
    if (!s->enabled) {
        return raw;
    }
    unsigned n = 2 * at;
    s->raw[n] = s->wire[n] = raw;
    s->raw[n + 1] = s->wire[n + 1] = raw >> 8;
    if (at >= 4 && at < 60 && zf_battery_standin_selected(s)) {
        s->wire[n] = at == 4 ? 3 : 0;
        s->wire[n + 1] = 0;
    }
    if (at == 127 && zf_battery_standin_selected(s)) {
        unsigned old_sum = 0, wire_sum = 0;
        for (unsigned i = 0; i < 255; i++) {
            old_sum += s->raw[i];
            wire_sum += s->wire[i];
        }
        s->wire[255] = s->raw[255] + old_sum - wire_sum;
    }
    return s->wire[n] | (uint16_t)s->wire[n + 1] << 8;
}
#endif
