/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native primary control and IRQ interfaces, with the same owner as FRONT config.
 * This stage adds no source clock or pending producer. W1S/W1C and latch behavior
 * are the explicit functional donor model, not measured physical timing. */
#include "hw/arm/nikon-zf-sensorif-primary.h"
bool z63_z9lab_zf_primary_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value);
bool z63_z9lab_zf_primary_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_primary_profile_valid(Object *obj);
char *z63_z9lab_zf_primary_state(Object *obj, Error **errp);

bool z63_z9lab_zf_primary_profile_valid(Object *obj)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (!s->bootstrap_fixtures || !s->multi_io_counter_model ||
        !s->event1508_front_model || !s->event1508_command_model ||
        !s->event1508_channel_model || !s->event1508_global0_model ||
        !s->event1508_global1_model || !s->event_channel_irq ||
        !s->event_global0_irq || !s->event_global1_irq ||
        s->event_pending_1508 || s->event_global0_pending || s->event_global1_pending ||
        s->event_channel_inputs || s->event_global0_inputs || s->event_global1_inputs ||
        s->event1508_timing_model || s->event_timing_active || s->event_timing_period_ns ||
        s->event_secondary_timing_model || s->event_secondary_timing_active ||
        s->event_front_boundary_model || s->event_front_boundary_active ||
        s->event_front_boundary_pending || s->event_output0_boundary_model ||
        s->event_programmed_clock_model || s->event1508_stop_model ||
        s->event1508_global0_bit10_model || s->event1508_global0_bit26_model ||
        s->event_output_group_running || s->engine150c_control_model ||
        s->zr_primary_bank1_model || s->primary_capture_deferred ||
        s->primary_d980_single_model || s->primary_d980_single_armed ||
        s->raw_pipeline_model || s->statistics_model || s->z63_sensor_vd ||
        z63_z9lab_lane0_ack_on) {
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (s->event_control_1508[i]) {
            return false;
        }
    }
    return true;
}

bool z63_z9lab_zf_primary_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    uint32_t mask;
    if (size != 4 || !zf_sensorif_primary_address(addr)) {
        return false;
    }
    if (zf_sensorif_front_config_address(addr)) {
        uint32_t *word = event_field_1508(s, addr, size, &mask);
        if (!word) {
            return false;
        }
        *value = *word;
        return true;
    }
    switch (addr) {
    case 0x15080010: case 0x15080018: case 0x15080020: case 0x15080028:
    case 0x15080044: case 0x15080048: case 0x15080050: case 0x15080054:
    case 0x15080068: case 0x1508006c:
        return z63_z9lab_read(obj, addr, size, value);
    default:
        /* Clear registers, front strobes and commands have no native read contract. */
        return false;
    }
}

bool z63_z9lab_zf_primary_write(Object *obj, hwaddr addr, uint64_t value, unsigned size)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    uint32_t mask;
    if (size != 4 || value > UINT32_MAX || !zf_sensorif_primary_address(addr)) {
        return false;
    }
    if (zf_sensorif_front_config_address(addr)) {
        uint32_t *word = event_field_1508(s, addr, size, &mask);
        if (!word) {
            return false;
        }
        *word = value; /* Keep original RMW high bits; do not clip writer bounds. */
        return true;
    }
    if (addr >= 0x15080010 && addr <= 0x1508002c) {
        unsigned group = (addr - 0x15080010) / 8;
        return !(value & ~(group == 3 ? UINT64_C(0x7fff) : UINT64_C(1))) &&
               z63_z9lab_write(obj, addr, value, size);
    }
    switch (addr) {
    case 0x15080030: case 0x15080034:
    case 0x15080038: case 0x1508003c: case 0x15080040:
        return value == 1 && z63_z9lab_write(obj, addr, value, size);
    case 0x15080044:
        if (value & ~UINT64_C(0xffff)) {
            return false;
        }
        s->event_mask_1508 = value;
        event_channel_update_irq(s);
        return true;
    case 0x15080048:
        s->event_pending_1508 &= ~(uint32_t)value;
        s->event_channel_acks++;
        event_channel_update_irq(s);
        return true;
    case 0x15080050:
        s->event_extended_1508[0x50 / 4] = value;
        event_global0_update_irq(s);
        return true;
    case 0x15080054:
        s->event_global0_pending &= ~(uint32_t)value;
        s->event_global0_acks++;
        event_global0_update_irq(s);
        return true;
    case 0x15080068:
        s->event_extended_1508[0x68 / 4] = value;
        event_global1_update_irq(s);
        return true;
    case 0x1508006c:
        s->event_global1_pending &= ~(uint32_t)value;
        s->event_global1_acks++;
        event_global1_update_irq(s);
        return true;
    default:
        return false;
    }
}

char *z63_z9lab_zf_primary_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    GString *state = g_string_new(NULL);
    g_string_append_printf(state, "{\"control\":[%u,%u,%u,%u],\"front_fields\":[",
        s->event_control_1508[0], s->event_control_1508[1],
        s->event_control_1508[2], s->event_control_1508[3]);
    for (unsigned i = 0; i < ARRAY_SIZE(zf_sensorif_front_offsets); i++) {
        uint32_t mask;
        uint32_t *word = event_field_1508(s,
            UINT64_C(0x15080000) + zf_sensorif_front_offsets[i], 4, &mask);
        assert(word);
        g_string_append_printf(state, "%s%u", i ? "," : "", *word);
    }
    g_string_append(state, "]}");
    return g_string_free(state, false);
}
