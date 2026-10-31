/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Same Z9 configuration owner from boot. No commit/start/status path here. */
#include "hw/arm/nikon-zf-sensorif-output-config.h"
bool z63_z9lab_zf_output_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value);
bool z63_z9lab_zf_output_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_output_profile_valid(Object *obj);
char *z63_z9lab_zf_output_state(Object *obj, Error **errp);

bool z63_z9lab_zf_output_profile_valid(Object *obj)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return s->bootstrap_fixtures && s->multi_io_counter_model &&
           !s->event1508_timing_model && !s->event_secondary_timing_model &&
           !s->event_front_boundary_model && !s->event_output0_boundary_model &&
           !s->event_programmed_clock_model && !s->event1508_stop_model &&
           !s->event1508_group_model && !s->event1508_command_model &&
           !s->engine150c_control_model && !s->raw_pipeline_model;
}

bool z63_z9lab_zf_output_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value)
{
    uint32_t mask;
    if (size != 4 || !zf_sensorif_output_address_verified(addr)) {
        return false;
    }
    uint32_t *word = event_field_1508(NIKON_Z9_LAB(obj), addr, size, &mask);
    if (!word) {
        return false;
    }
    *value = *word;
    return true;
}

bool z63_z9lab_zf_output_write(Object *obj, hwaddr addr, uint64_t value, unsigned size)
{
    uint32_t mask;
    if (size != 4 || value > UINT32_MAX || !zf_sensorif_output_address_verified(addr)) {
        return false;
    }
    uint32_t *word = event_field_1508(NIKON_Z9_LAB(obj), addr, size, &mask);
    if (!word) {
        return false;
    }
    /* Own3430 RMW preserves31 old bits. Z9's writer-bound1 must not clip them. */
    *word = value;
    return true;
}

char *z63_z9lab_zf_output_state(Object *obj, Error **errp)
{
    GString *state = g_string_new("{\"words\":[");
    for (unsigned i = 0; i < ARRAY_SIZE(zf_sensorif_output_offsets); i++) {
        uint32_t mask;
        uint32_t *word = event_field_1508(NIKON_Z9_LAB(obj),
            UINT64_C(0x15080000) + zf_sensorif_output_offsets[i], 4, &mask);
        assert(word);
        g_string_append_printf(state, "%s%u", i ? "," : "", *word);
    }
    g_string_append(state, "]}");
    return g_string_free(state, false);
}
