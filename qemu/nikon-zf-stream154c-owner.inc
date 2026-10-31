/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hw/arm/nikon-zf-stream154c.h"
bool z63_z9lab_zf_stream154c_profile_valid(Object *obj);
bool z63_z9lab_zf_stream154c_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
char *z63_z9lab_zf_stream154c_state(Object *obj, Error **errp);

bool z63_z9lab_zf_stream154c_profile_valid(Object *obj)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (!s->bootstrap_fixtures || !s->multi_io_counter_model || !s->stream154c_stop_model ||
        s->raw_pipeline_model || s->statistics_model || s->engine150c_control_model ||
        s->primary_capture_deferred || s->primary_d980_single_model) {
        return false;
    }
    for (unsigned ch = 0; ch < 3; ch++) {
        if (s->stream154c_stop_requests[ch] || s->stream154c_reset_commands[ch] ||
            s->zf_stream154c_writes[ch]) {
            return false;
        }
        for (unsigned i = 0; i < 0x400 / 4; i++) {
            if (s->stream154c_config[ch][i]) {
                return false;
            }
        }
    }
    return true;
}

bool z63_z9lab_zf_stream154c_write(Object *obj, hwaddr addr, uint64_t value, unsigned size)
{
    if (size != 4 || value > UINT32_MAX || !zf_stream154c_write_address(addr) ||
        !z63_z9lab_write(obj, addr, value, size)) {
        return false;
    }
    NIKON_Z9_LAB(obj)->zf_stream154c_writes[(addr - 0x154c0000) / 0x400]++;
    return true;
}

char *z63_z9lab_zf_stream154c_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    GString *out = g_string_new("{\"empty_only\":true,\"ordinary_words_per_channel\":48,\"channels\":[");
    for (unsigned ch = 0; ch < 3; ch++) {
        g_string_append_printf(out, "%s{\"channel\":%u,\"CPU_writes\":%" PRIu64
            ",\"stop_requests\":%" PRIu64 ",\"reset_pulses\":%" PRIu64 ",\"words\":{",
            ch ? "," : "", ch, s->zf_stream154c_writes[ch], s->stream154c_stop_requests[ch], s->stream154c_reset_commands[ch]);
        bool first = true;
        for (unsigned off = 0; off < 0x400; off += 4) {
            hwaddr addr = 0x154c0000 + ch * 0x400 + off;
            if (zf_stream154c_read_address(addr) || zf_stream154c_write_address(addr)) {
                g_string_append_printf(out, "%s\"%x\":%u", first ? "" : ",", off, s->stream154c_config[ch][off / 4]);
                first = false;
            }
        }
        g_string_append(out, "}}");
    }
    g_string_append(out, "]}");
    return g_string_free(out, false);
}
