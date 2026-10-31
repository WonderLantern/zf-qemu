/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Own two complete functions/68B. Reuse Z9 idle-read/snapshot request model;
 * do not enable the separate bitmap-consumption or timing/source options. */
#include "hw/arm/nikon-zf-sensorif-group.h"
bool z63_z9lab_zf_group_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value);
bool z63_z9lab_zf_group_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_group_profile_valid(Object *obj);

bool z63_z9lab_zf_group_profile_valid(Object *obj)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return s->bootstrap_fixtures && s->multi_io_counter_model &&
           s->event1508_group_model && !s->event1508_timing_model &&
           !s->event_secondary_timing_model && !s->event_front_boundary_model &&
           !s->event_output0_boundary_model && !s->event_programmed_clock_model &&
           !s->event1508_stop_model && !s->event1508_command_model &&
           !s->engine150c_control_model && !s->raw_pipeline_model &&
           !s->event_output_group_running &&
           !s->event_group_1508[0] && !s->event_group_1508[1] &&
           !s->event_group_latched[0] && !s->event_group_latched[1] &&
           !s->event_group_requests[0] && !s->event_group_requests[1];
}

bool z63_z9lab_zf_group_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value)
{
    if (size != 4 || !zf_sensorif_group_address(addr)) {
        return false;
    }
    /* Submit reads0 are the inherited explicit quiescent hypothesis,
     * not a measured Zf busy/ready state or a completion signal. */
    return z63_z9lab_read(obj, addr, size, value);
}

bool z63_z9lab_zf_group_write(Object *obj, hwaddr addr, uint64_t value, unsigned size)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (size != 4 || value > UINT32_MAX || !zf_sensorif_group_address(addr)) {
        return false;
    }
    if (addr == 0x15083408 || addr == 0x15083418) {
        /* Native masks old bitmap, but ORs unmasked W1. Retain it exactly. */
        s->event_group_1508[addr == 0x15083418] = value;
        return true;
    }
    /* A nonzero high submit bit needs another contract; never trim it to1. */
    return value == 1 && z63_z9lab_write(obj, addr, value, size);
}
