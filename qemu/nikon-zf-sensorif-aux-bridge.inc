/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reuse the Z9 auxiliary-bank configuration shadow and pending/IRQ model.
 * The generated address predicate admits only own-bound bank0/1 channel0.
 * No source input or timer is created here. RMW fields retain all old bits:
 * in particular, Zf +104 sets C000, outside Z9's earlier FF writer bound.
 */
#include "hw/arm/nikon-zf-sensorif-aux-config.h"

bool z63_z9lab_zf_aux_read(Object *obj, hwaddr addr, unsigned size,
                          uint64_t *value);
bool z63_z9lab_zf_aux_write(Object *obj, hwaddr addr, uint64_t value,
                           unsigned size);
bool z63_z9lab_zf_aux_profile_valid(Object *obj);

bool z63_z9lab_zf_aux_profile_valid(Object *obj)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    /* TYPE_OBJECT creation bypasses the full-machine validation path. */
    return s->bootstrap_fixtures && s->multi_io_counter_model &&
           s->event1508_aux_config_model && s->event1508_aux_model &&
           s->event1508_aux1_model && s->event_aux0_irq && s->event_aux1_irq &&
           !s->event_aux0_pending && !s->event_aux1_pending &&
           !s->event1508_aux_paired_boundary && !s->event1508_aux_status_valid &&
           !s->event1508_timing_model && !s->event_secondary_timing_model &&
           !s->event_front_boundary_model && !s->event1508_channel_model &&
           !s->event1508_global0_model && !s->event1508_global1_model &&
           !s->event1508_group_model && !s->event1508_command_model &&
           !s->engine150c_control_model && !s->zr_primary_bank1_model &&
           !s->raw_pipeline_model;
}

bool z63_z9lab_zf_aux_read(Object *obj, hwaddr addr, unsigned size,
                          uint64_t *value)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    uint32_t mask;
    uint32_t *word;

    if (size != 4 || !zf_sensorif_aux_address_verified(addr)) {
        return false;
    }
    if (zf_sensorif_aux_pending_address(addr)) {
        return z63_z9lab_read(obj, addr, size, value);
    }
    word = event_field_1508(s, addr, size, &mask);
    if (!word) {
        return false;
    }
    *value = *word;
    return true;
}

bool z63_z9lab_zf_aux_write(Object *obj, hwaddr addr, uint64_t value,
                           unsigned size)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    uint32_t mask;
    uint32_t *word;

    if (size != 4 || value > UINT32_MAX ||
        !zf_sensorif_aux_address_verified(addr)) {
        return false;
    }
    if (zf_sensorif_aux_pending_address(addr)) {
        /* Original Z9 behavior only supports pending bits0/1. A different
         * clear mask is rejected, not silently retained as a status word. */
        return !(value & ~UINT64_C(3)) &&
               z63_z9lab_write(obj, addr, value, size);
    }
    if ((addr == 0x1508510c || addr == 0x1508550c) &&
        (value & ~UINT64_C(0xf3))) {
        return false;
    }
    word = event_field_1508(s, addr, size, &mask);
    if (!word) {
        return false;
    }
    *word = value;
    if (addr == 0x1508510c) {
        event_aux0_update_irq(s);
    } else if (addr == 0x1508550c) {
        event_aux1_update_irq(s);
    }
    return true;
}
