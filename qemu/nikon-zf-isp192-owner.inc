/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Own complete ordinary driver, retaining the existing donor's storage.
 * Its ordinary write branch has no valid/invalidate/commit side effects.
 * +10 preserves full old32, including bit0 in the ffffeeef RMW mask. */
#include "hw/arm/nikon-zf-isp192-owner.h"
bool z63_z9lab_zf_isp192_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value, bool diagnostic);
bool z63_z9lab_zf_isp192_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_isp192_profile_valid(Object *obj);
char *z63_z9lab_zf_isp192_state(Object *obj, Error **errp);

bool z63_z9lab_zf_isp192_profile_valid(Object *obj)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return s->bootstrap_fixtures && s->multi_io_counter_model &&
           !s->engine150c_control_model && !s->engine150c_stop_model &&
           !s->zr_primary_bank1_model && !s->primary_capture_deferred &&
           !s->raw_pipeline_model && !s->primary_d980_single_model &&
           !s->engine150c_configured && !s->engine150c_latched &&
           !s->engine150c_active && !s->engine150c_commits && !s->engine150c_starts;
}

bool z63_z9lab_zf_isp192_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value, bool diagnostic)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (size != 4 || !zf_isp192_aperture(addr)) {
        return false;
    }
    unsigned off = (addr - 0x150ce000) % 0x400;
    if (!zf_isp192_owner_offset(off) ||
        (!diagnostic && off != 0x10)) {
        return false; /* No config echo standing in for result getters. */
    }
    uint32_t mask, *word = engine_150c_upper_word(s, addr, size, &mask);
    if (!word) {
        return false;
    }
    *value = *word;
    return true;
}

bool z63_z9lab_zf_isp192_write(Object *obj, hwaddr addr, uint64_t value, unsigned size)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (size != 4 || value > UINT32_MAX || !zf_isp192_aperture(addr)) {
        return false;
    }
    unsigned bank = (addr - 0x150ce000) / 0x400;
    unsigned off = (addr - 0x150ce000) % 0x400;
    if (!zf_isp192_owner_offset(off)) {
        return false;
    }
    uint32_t mask, *word = engine_150c_upper_word(s, addr, size, &mask);
    if (!word || (off != 0x10 && (value & ~mask))) {
        return false;
    }
    *word = value;
    s->zf_isp192_writes[bank]++;
    return true;
}

char *z63_z9lab_zf_isp192_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    GString *out = g_string_new("{\"ordinary_config_only\":true,\"words_per_instance\":51,");
    g_string_append_printf(out, "\"writes\":[%" PRIu64 ",%" PRIu64 "],"
        "\"configured\":%u,\"latched\":%u,\"active\":%u,"
        "\"commits\":%" PRIu64 ",\"starts\":%" PRIu64 ",\"banks\":[",
        s->zf_isp192_writes[0], s->zf_isp192_writes[1], s->engine150c_configured,
        s->engine150c_latched, s->engine150c_active, s->engine150c_commits, s->engine150c_starts);
    for (unsigned bank = 0; bank < 2; bank++) {
        bool first = true;
        g_string_append(out, bank ? ",{" : "{");
        for (unsigned off = 0; off < 0x400; off += 4) {
            if (!zf_isp192_owner_offset(off)) {
                continue;
            }
            uint32_t mask, *word = engine_150c_upper_word(s, 0x150ce000 + bank * 0x400 + off, 4, &mask);
            assert(word);
            g_string_append_printf(out, "%s\"%x\":%u", first ? "" : ",", off, *word);
            first = false;
        }
        g_string_append_c(out, '}');
    }
    g_string_append(out, "]}");
    return g_string_free(out, false);
}
