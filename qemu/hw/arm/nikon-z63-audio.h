/*
 * Nikon Z 6III -- the audio codec on serial-bus instance 10.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * What is on the wire
 * -------------------
 * The audio service ("AudSvSe0"/"AudSvMx0", tag "DEC" at 0xa02e7b700)
 * talks to a codec over logical serial channel 5 (sub_40C7C894:
 * sub_40C0040C(5), stored at 0xa02e7b768), which is serial instance 10,
 * with a GPIO chip select: resource 76, pin descriptor 0xa028097d8 =
 * {0x4c, port 5, bit 13}, driven through GPIO_TOP port 5's data word
 * (0x0d151014, upper half the write mask) by sub_40C15BB8. Every access
 * is one of two shapes, each inside one chip-select low:
 *
 *   write  sub_40C7DDFC   one transfer: [address, data]
 *   read   sub_40C7E2F0   one transfer: [address | 0x8000], then a second,
 *                         receive-only one of a single word (sent as 0 by
 *                         the serial driver's fill, sub_40C1538C), whose
 *                         received word is the register
 *
 * 16-bit words, addresses up to 0x5ffe (both functions refuse higher).
 * Those two and the debug dump are the only callers of the codec's
 * transfer routine sub_40C7EF2C.
 *
 * Why it matters
 * --------------
 * The codec's init runs inside module 0x421's start-up handler
 * (sub_40E3FE2C -> sub_40C7F010 -> the register script interpreter
 * sub_40C7CAC0), on a worker task. Unclocked, its first transfer waited for
 * ever (Z63_SIO: "instance 10 has TX words and no peer"), module 0x421 never
 * finished starting, its subscriber sub_40F9792C never got event 0x4210000,
 * the movie live-view set-up's 3 s wait in sub_40F99E7C timed out, and the
 * GUI never drew the shooting screen.
 *
 * What the reads feed
 * -------------------
 * Reads exist only for the script's set-bits and clear-bits operations
 * (sub_40C7E45C, sub_40C7E4AC: read, OR or AND-NOT, write back) and for
 * the debug register dump sub_40C7E064. No read is compared with anything
 * -- there is no identity check and no poll -- so a register file answers
 * the driver the way it uses the device.
 *
 * DECLARED CHOICES. The register file starts at zero, which is not the
 * codec's measured reset state (the codec is not identified); reads return
 * what was last written. The time per word is the write-only sinks'
 * (Z63_SIO_SINK_NS). Nothing here produces or consumes audio.
 */

#ifndef HW_ARM_NIKON_Z63_AUDIO_H
#define HW_ARM_NIKON_Z63_AUDIO_H

#define Z63_AUDIO_SIO_INST   10      /* logical channel 5 */
#define Z63_AUDIO_CS_PORT    5       /* GPIO_TOP port 5, 0x0d151014 */
#define Z63_AUDIO_CS_BIT     13
#define Z63_AUDIO_REGS       0x6000
#define Z63_AUDIO_LOG        16      /* first accesses named on stderr */

typedef enum {
    Z63_AUDIO_ADDR,                  /* the next word is an address */
    Z63_AUDIO_WRITE,                 /* the next word is data to store */
    Z63_AUDIO_READ,                  /* the next word clocks the register out */
    Z63_AUDIO_DONE,                  /* frame complete; more words are stray */
} Z63AudioState;

typedef struct Z63Audio {
    Z63Sio *sio;
    QEMUTimer *timer;
    bool attached;
    bool selected;                   /* chip select low */
    Z63AudioState state;
    uint16_t reg;
    uint16_t regs[Z63_AUDIO_REGS];
    uint64_t writes, reads, stray;
} Z63Audio;

static void z63_audio_log(Z63Audio *a, const char *what, uint16_t reg,
                          uint16_t value)
{
    uint64_t n = a->writes + a->reads;

    if (n <= Z63_AUDIO_LOG) {
        error_report("Z63_AUDIO: %s reg 0x%04x = 0x%04x%s", what, reg, value,
                     n == Z63_AUDIO_LOG ? " (further accesses not logged)"
                                        : "");
    }
}

static uint16_t z63_audio_word(void *opaque, unsigned inst, uint16_t out)
{
    Z63Audio *a = opaque;
    uint16_t in = 0;

    switch (a->state) {
    case Z63_AUDIO_ADDR:
        a->reg = out & 0x7fff;
        if (a->reg >= Z63_AUDIO_REGS) {
            a->state = Z63_AUDIO_DONE;
            goto stray;
        }
        a->state = (out & 0x8000) ? Z63_AUDIO_READ : Z63_AUDIO_WRITE;
        break;
    case Z63_AUDIO_WRITE:
        a->regs[a->reg] = out;
        a->writes++;
        z63_audio_log(a, "write", a->reg, out);
        a->state = Z63_AUDIO_DONE;
        break;
    case Z63_AUDIO_READ:
        in = a->regs[a->reg];
        a->reads++;
        z63_audio_log(a, "read", a->reg, in);
        a->state = Z63_AUDIO_DONE;
        break;
    case Z63_AUDIO_DONE:
    stray:
        if (a->stray++ == 0) {
            error_report("Z63_AUDIO: word 0x%04x outside the two transfer "
                         "shapes (pc=0x%" PRIx64 "); answered 0 -- see "
                         "nikon-z63-audio.h", out, z63_current_pc());
        }
        break;
    }
    return in;
}

static void z63_audio_clock(void *opaque)
{
    Z63Audio *a = opaque;

    z63_sio_clock(a->sio, Z63_AUDIO_SIO_INST, Z63_SIO_FIFO, z63_audio_word,
                  a);
}

static void z63_audio_kick(void *opaque, unsigned inst)
{
    Z63Audio *a = opaque;
    unsigned words = a->sio->pio[inst].tx_queued;

    if (!timer_pending(a->timer)) {
        timer_mod(a->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)words * Z63_SIO_SINK_NS);
    }
}

/* GPIO_TOP port Z63_AUDIO_CS_PORT changed: a falling chip select starts a
 * frame. */
static void z63_audio_cs(Z63Audio *a, uint16_t port_word)
{
    bool selected = !(port_word & (1u << Z63_AUDIO_CS_BIT));

    if (!a->attached || selected == a->selected) {
        return;
    }
    a->selected = selected;
    if (selected) {
        a->state = Z63_AUDIO_ADDR;
    }
}

static void z63_audio_init(Z63Audio *a, Z63Sio *sio, bool attach)
{
    a->sio = sio;
    if (!attach) {
        return;
    }
    if (sio->kick[Z63_AUDIO_SIO_INST]) {
        error_report("Z63_AUDIO: serial instance %u already has a peer",
                     Z63_AUDIO_SIO_INST);
        exit(1);
    }
    a->attached = true;
    a->state = Z63_AUDIO_DONE;          /* until the first chip select */
    a->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_audio_clock, a);
    sio->kick[Z63_AUDIO_SIO_INST] = z63_audio_kick;
    sio->kick_opaque[Z63_AUDIO_SIO_INST] = a;
    error_report("Z63_AUDIO: codec register file on serial instance %u, "
                 "chip select GPIO port %u bit %u (declared: starts at zero; "
                 "see nikon-z63-audio.h, -machine audio-codec=)",
                 Z63_AUDIO_SIO_INST, Z63_AUDIO_CS_PORT, Z63_AUDIO_CS_BIT);
}

#endif /* HW_ARM_NIKON_Z63_AUDIO_H */
