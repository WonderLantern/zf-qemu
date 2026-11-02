/* SPDX-License-Identifier: GPL-2.0-or-later
 * Zf EX's STM32 RNG register contract, not a source of physical body secrets.
 * Cached ST CMSIS f407 header commit 0fa0e489...: 50060800 CR/SR/DR,
 * RNGEN=4, IE=8, DRDY=1, error latches=0x60, IRQ80.
 * Research model: host entropy; fixed 2us ready latency; clock/seed failures
 * and RCC clock gating/reset coupling not simulated. Not cycle accurate.
 */
#include "qemu/guest-random.h"

typedef struct ZfRng {
    MemoryRegion mr;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t cr, sr, dr;
    unsigned generated, read;
} ZfRng;

static void zf_rng_irq(ZfRng *s)
{
    qemu_set_irq(s->irq, !!((s->cr & 8) && (s->sr & 0x61)));
}

static void zf_rng_ready(void *opaque)
{
    ZfRng *s = opaque;
    if (!(s->cr & 4)) return;
    qemu_guest_getrandom_nofail(&s->dr, sizeof(s->dr));
    s->sr |= 1;
    s->generated++;
    zf_rng_irq(s);
}

static uint64_t zf_rng_read(void *opaque, hwaddr off, unsigned size)
{
    ZfRng *s = opaque;
    if (size != 4 || (off & 3) || off > 8) {
        z63_stm32_stop("ZF_RNG", 0x50060800 + off, size, false, 0);
        return 0;
    }
    if (off == 0) return s->cr;
    if (off == 4) return s->sr;
    uint32_t result = s->dr;
    if (s->sr & 1) {
        s->sr &= ~1;
        zf_rng_irq(s);
        if (s->cr & 4) timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2000);
        if (s->read++ < 8) error_report("ZF_RNG: consumed host-entropy word %u (PC 0x%08x)",
                                      s->read, z63_stm32_pc());
    }
    return result;
}

static void zf_rng_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ZfRng *s = opaque;
    if (size != 4 || (off & 3) || off > 4 || (off == 0 && (value & ~0xcU))) {
        z63_stm32_stop("ZF_RNG", 0x50060800 + off, size, true, value);
        return;
    }
    if (off == 0) {
        uint32_t old = s->cr;
        s->cr = value;
        if (!(value & 4)) { timer_del(s->timer); s->sr &= ~1; }
        else if (!(old & 4)) timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2000);
        error_report("ZF_RNG: CR 0x%x -> 0x%x (PC 0x%08x)", old, s->cr, z63_stm32_pc());
    } else {
        s->sr &= (uint32_t)value | ~0x60U; /* error flags cleared by writing zero */
    }
    zf_rng_irq(s);
}

static const MemoryRegionOps zf_rng_ops = {
    .read = zf_rng_read, .write = zf_rng_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void zf_rng_init(Z63Stm32 *mcu, Object *owner)
{
    ZfRng *s = g_new0(ZfRng, 1);
    s->irq = qdev_get_gpio_in(DEVICE(&mcu->armv7m), 80);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, zf_rng_ready, s);
    memory_region_init_io(&s->mr, owner, &zf_rng_ops, s, "zf-mcu-rng", 0x400);
    memory_region_add_subregion(&mcu->mem, 0x50060800, &s->mr);
    error_report("ZF_RNG: host entropy, 2us virtual ready latency; RCC/error injection unmodelled");
}
