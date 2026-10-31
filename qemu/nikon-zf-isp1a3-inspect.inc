/* SPDX-License-Identifier: GPL-2.0-or-later */
/* No replacement writer: adapter invokes original config150d write path, including
 * bank counters, secondary validbits and invalidating its old40000 submission. */
#include "hw/arm/nikon-zf-isp1a3-config.h"
bool z63_z9lab_zf_isp1a3_profile_valid(Object *obj);
char *z63_z9lab_zf_isp1a3_state(Object *obj, Error **errp);

bool z63_z9lab_zf_isp1a3_profile_valid(Object *obj)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return s->bootstrap_fixtures && s->multi_io_counter_model && s->config150d_model &&
           !s->config150d_writes[0] && !s->config150d_writes[1] &&
           !s->config150d_commits && !s->secondary150d_submits &&
           !s->engine150d_control_model && !s->secondary150d_submit_model &&
           !s->engine150d_readback_valid && !s->engine150c_control_model &&
           !s->raw_pipeline_model && !s->primary_capture_deferred;
}

char *z63_z9lab_zf_isp1a3_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    GString *state = g_string_new(NULL);
    g_string_append_printf(state, "{\"writes\":[%" PRIu64 ",%" PRIu64
        "],\"commits\":%" PRIu64 ",\"secondary_submits\":%" PRIu64
        ",\"engine_configured\":%u,\"banks\":[", s->config150d_writes[0],
        s->config150d_writes[1], s->config150d_commits, s->secondary150d_submits,
        s->engine150c_configured);
    for (unsigned bank = 0; bank < 2; bank++) {
        g_string_append_printf(state, "%s[", bank ? "," : "");
        unsigned count = 0;
        for (unsigned off = 0; off <= 0x1500; off += 4) {
            hwaddr addr = 0x150d0000 + bank * 0x2000 + off;
            if (zf_isp1a3_config_address(addr)) {
                uint32_t mask;unsigned selected;
                uint32_t *word = config150d_word(s, addr, 4, &mask, &selected);
                assert(word && selected == bank);
                g_string_append_printf(state, "%s%u", count++ ? "," : "", *word);
            }
        }
        g_string_append(state, "]");
    }
    g_string_append(state, "],\"secondary_valid\":[");
    unsigned count = 0;
    for (unsigned off = 0; off <= 0x1500; off += 4) {
        if (zf_isp1a3_config_address(0x150d2000 + off)) {
            g_string_append_printf(state, "%s%s", count++ ? "," : "",
                s->secondary150d_valid[off / 4] ? "true" : "false");
        }
    }
    g_string_append(state, "]}");
    return g_string_free(state, false);
}
