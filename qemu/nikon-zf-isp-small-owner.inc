/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hw/arm/nikon-zf-isp-small-owner.h"
bool z63_z9lab_zf_isp_small_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value);
bool z63_z9lab_zf_isp_small_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_isp_small_profile_valid(Object *obj);
char *z63_z9lab_zf_isp_small_state(Object *obj, Error **errp);

bool z63_z9lab_zf_isp_small_profile_valid(Object *obj)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return s->bootstrap_fixtures && s->multi_io_counter_model &&
           !s->engine150c_control_model && !s->engine150c_stop_model &&
           !s->zr_primary_bank1_model && !s->primary_capture_deferred &&
           !s->raw_pipeline_model && !s->primary_d980_single_model &&
           !s->engine150c_configured && !s->engine150c_latched &&
           !s->engine150c_active && !s->engine150c_commits && !s->engine150c_starts;
}

bool z63_z9lab_zf_isp_small_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value)
{
    /* Only197+0 has an ordinary guest read contract. Result getters remain strict. */
    if (size != 4 || addr != 0x150d5100) {
        return false;
    }
    uint32_t mask, *word = engine_150c_upper_word(NIKON_Z9_LAB(obj), addr, size, &mask);
    if (!word) {
        return false;
    }
    *value = *word;
    return true;
}

bool z63_z9lab_zf_isp_small_write(Object *obj, hwaddr addr, uint64_t value, unsigned size)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (size != 4 || value > UINT32_MAX || !zf_isp_small_config_address(addr)) {
        return false;
    }
    uint32_t mask, *word = engine_150c_upper_word(s, addr, size, &mask);
    if (!word || (addr != 0x150d5100 && (value & ~mask))) {
        return false;
    }
    *word = value; /* 197+0 preserves old bits beyond the zero-initial mask. */
    s->zf_isp_small_writes[(addr - 0x150d5000) / 0x100]++;
    return true;
}

char *z63_z9lab_zf_isp_small_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    static const unsigned ids[] = {0x195,0x197,0x198};
    static const unsigned counts[] = {33,44,35};
    GString *out = g_string_new("{\"ordinary_config_only\":true,");
    g_string_append_printf(out, "\"configured\":%u,\"latched\":%u,\"active\":%u,"
        "\"commits\":%" PRIu64 ",\"starts\":%" PRIu64 ",\"instances\":[",
        s->engine150c_configured,s->engine150c_latched,s->engine150c_active,s->engine150c_commits,s->engine150c_starts);
    for (unsigned unit = 0; unit < 3; unit++) {
        hwaddr base = 0x150d5000 + unit * 0x100;
        g_string_append_printf(out, "%s{\"id\":\"%x\",\"base\":\"%" HWADDR_PRIx
            "\",\"word_count\":%u,\"CPU_writes\":%" PRIu64 ",\"words\":{",
            unit ? "," : "", ids[unit],base,counts[unit],s->zf_isp_small_writes[unit]);
        bool first = true;
        for (unsigned off = 0; off < 0x100; off += 4) {
            if (!zf_isp_small_config_address(base + off)) {
                continue;
            }
            uint32_t mask, *word = engine_150c_upper_word(s,base+off,4,&mask);
            assert(word);
            g_string_append_printf(out,"%s\"%x\":%u",first?"":",",off,*word);
            first=false;
        }
        g_string_append(out,"}}");
    }
    g_string_append(out,"]}");
    return g_string_free(out,false);
}
