/*
 * Nikon Z 6III -- a synthetic lens on the Z mount's data contacts.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The body MCU only powers the lens and reports whether one is on the
 * mount (docs/mcu-ex/lens.md, sections 1-8). The EXPEED talks to it itself,
 * and until something answers there the connect sequence stops at its first
 * exchange and starts over, power off and on, for as long as the camera
 * runs. This file is NOT a lens. It is a peer that implements the link
 * exactly as this image's lens transport expects it, logs every frame, and
 * answers with the replies listed below -- each one a declared, synthetic
 * input, not data read from any lens.
 *
 * The link, from the transport (task 0x4117a310, opened by 0x4117ad38;
 * docs/mcu-ex/lens.md section 10 and docs/mcu-ex/eg/eg-lens.md):
 *
 *   serial instance 3, 8 MHz (config 0xa02aff508: +24 = 8000000 Hz),
 *            16-bit words, full duplex: the body shifts a word out while
 *            the lens shifts one in.
 *   pin 41   GPIO port 3 bit 10, EXT_INT_10 (line record {bank 1, bit 1,
 *            INTID 65}), installed mode 0 with config word 0: the falling
 *            edge (nikon-z63-extint.h). ISR 0x4117b338. The lens's
 *            "ready", active low: the transport starts a transfer only
 *            while it reads 0 (sub_40C15BFC == 0, 0x4117a3b4), and each
 *            phase of a transfer moves on at its next falling edge.
 *   pin 58   GPIO port 4 bit 11, the body's request. The wake handshake
 *            0x4117b210(1) sets it in the bank sub_40C15C7C writes (bank 2,
 *            0x0d151210) and then waits up to 200 ticks for the pin-41
 *            interrupt; 0x4117b210(0) clears it again.
 *
 *   one transfer (0x4117a310; ISR 0x4117b338 cases 3/4 and 5/6/7)
 *     1  the body checks pin 41 is low, builds the 8-byte header
 *        (0x4117b950): 0x55, link sequence, class (1/2), command low,
 *        command high, u16 payload length in words, checksum
 *        (-(sum of bytes 0..6)), and sends it: 4 words, transfer type 3.
 *     2  the lens drops pin 41 again (falling edge, ISR state 3): the body
 *        starts the data phase, length/2 words, transfer type 4 (DMA):
 *        out goes 0x55, its payload, a checksum byte that makes the block
 *        sum to 0; in comes the lens's block of the same length, checked
 *        by sub_4117BFE8 for the same two rules (0x55 first, sum 0).
 *     3  the lens drops pin 41 once more (ISR state 4 -> 8): done.
 *   Commands whose record has +16 = 1 (0x27e, 0x37e, 0x67e, most of the
 *   0x70 group) take two data phases instead (ISR states 5 -> 6 -> 7):
 *   first the body's block (length from the header; what comes back goes
 *   to 0xa04ce3800 and is not checked), then the lens's (the record's RX
 *   length; the body sends a 0x55 filler block, 0xa04ce5780, and checks
 *   what comes back), a pin-41 edge before each and after the last.
 *
 * The header goes out in PIO (transfer type 3), every data phase by DMA
 * (type 4), which is how this peer tells them apart; a data phase's length
 * is the DMA transfer's own. Every data phase is answered with a complete
 * block (0x55, payload, checksum): in a two-phase command the first one is
 * ignored by the body and the second is the reply.
 *
 * So this peer: holds pin 41 high (not ready) while the lens is off, not
 * on the mount, or asleep; on the request's rising edge it raises pin 41
 * if needed and drops it Z63_LENS_WAKE_NS later (awake); for each transfer
 * it clocks the four header words, raises pin 41 and drops it
 * Z63_LENS_PREP_NS later; clocks the data words, answering with its reply
 * block; and pulses pin 41 again. The header words it answers with are 0
 * (nothing reads them). The byte order inside a word is taken from the
 * header's first word, whose first byte is 0x55.
 *
 * Power and presence come from the real MCU when there is one: the lens is
 * on the mount while the MCU's lens-detect input PG0 reads low
 * (lenshw_detect_read 0x08035600), and powered while either power path is
 * on (PB0 = path A, PA9 = path B: lenshw_set_a/_b 0x08035850/0x0803586C).
 * Without the real MCU the lens is always there and powered.
 *
 * With the replies below the EXPEED takes it for a Z lens: the connect
 * sequence runs 0x1F, 0x10, 0x11, 0x74, the 0x70 group (0x4070, 0x4170,
 * 0x2870), 0x78, 0x75, 0x17E, 0x16, 0x7E, 0x13, 0x15, 0x76, 0x67E, and
 * from then on the body polls 0x2670; live view leaves the no-lens state
 * (exposure mode P instead of A, the aperture shows F1), and the lens
 * status item 0x2C19 reads "10-1000mm f/1".
 *
 * DECLARED CHOICES, each a guess where the hardware is not known: the
 * timings (word time = 16 bits at 8 MHz; the wake, preparation and
 * completion delays), that the lens stays awake when the request drops,
 * and every reply byte (z63_lens_replies). Off by default (-machine
 * lens=on).
 */

#ifndef HW_ARM_NIKON_Z63_LENS_H
#define HW_ARM_NIKON_Z63_LENS_H

#include "nikon-zf-lens-profile.h"

#define Z63_LENS_SIO        3       /* serial instance (0x4117ad38) */
#define Z63_LENS_RDY_LINE   10      /* pin 41 -> EXT_INT_10 */
#define Z63_LENS_RDY_WORD   0x00c   /* port 3's data word */
#define Z63_LENS_RDY_BIT    10
#define Z63_LENS_REQ_WORD   0x210   /* pin 58: port 4 in bank 2 */
#define Z63_LENS_REQ_BIT    11
#define Z63_LENS_WORD_NS    2000                /* 16 bits at 8 MHz */
#define Z63_LENS_WAKE_NS    (500 * SCALE_US)    /* a choice */
#define Z63_LENS_PREP_NS    (100 * SCALE_US)    /* a choice */
#define Z63_LENS_DONE_NS    (20 * SCALE_US)     /* a choice */
#define Z63_LENS_BLOCK_MAX  8192
#define Z63_LENS_LOG        200

/* A reply: the payload between the 0x55 and the checksum. Commands not
 * listed get zeros. */
typedef struct Z63LensReply {
    uint16_t code;
    uint8_t len;
    uint8_t bytes[96];
} Z63LensReply;

/*
 * The replies that take the connect sequence (engine 0x4107d730, step
 * table 0xa02b44de0; docs/mcu-ex/lens.md 10.5) through to a connected
 * Z lens. Each step's done function reads reply bytes as "fields" (field
 * table 0xa02b4d210, getter 0x4109710c; z63-tools/mcu-ex/lensfields.py
 * says which byte of which reply a field is). Everything else is 0, which
 * the body then shows as it would a lens reporting 0: f/1, no focal
 * length, no name.
 */
static const Z63LensReply z63_lens_replies[] = {
    /* 0x1F, the first exchange after the wake (step 4, done 0x41087ecc):
     * byte 0 bit 0 is field 36; 0 goes straight to step 11 (no second
     * 0x1F). */
    { 0x001f, 2, { 0x00, 0x00 } },
    /* 0x10, identification (step 11): byte 9 bit 7 is field 24 and makes
     * the lens kind 1 (0x41096d14 reads it from the raw block), the Z
     * lens, which step 12 (done 0x41088810) sends to step 31; byte 13
     * bit 0, field 92, must stay 0 (done 0x410885bc). */
    { 0x0010, 9, { 0, 0, 0, 0, 0, 0, 0, 0, 0x80 } },
    /* 0x15, a status poll (step 20, done 0x4108904c, the kind-2 path):
     * byte 1 bits 0..7 are fields 104 103 102 101 105 106 108 107, byte
     * 2 field 153; field 108 (bit 6) = 1 moves on to step 22. */
    { 0x0015, 3, { 0x40, 0x00, 0x00 } },
    /* 0x74 (step 31, done 0x4108a048): byte 4 is field 62 (0 = no error),
     * byte 2 bit 0 field 59 (must be 1, else the sequence starts over),
     * byte 1 bit 0 field 58 (0 goes on to step 33). */
    { 0x0074, 6, { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 } },
    /* 0x76 (step 48, done 0x4108bacc via 0x4108f194): byte 4 bit 5 is
     * field 333 (must be 0), bit 6 field 334 (must be 1). After it the
     * sequence completes and the body polls 0x2670 for the lens state. */
    { 0x0076, 10, { 0x00, 0x00, 0x00, 0x40, 0, 0, 0, 0, 0, 0 } },
    /* 0x2870, lens data (step 37; parser 0x4109c744 copies block bytes
     * 5..48 to the start of buffer 4). Buffer 4 +0x10 is the focal length
     * now (field 503), +0x14/+0x15 the shortest/longest (507/508), each a
     * code into the focal table 0xa02b3447c (focal x 10; 5 mm * 2^(code /
     * 24)): 80 = 50 mm, 24 = 10 mm, 183 = 1000 mm. The body names the
     * lens from these itself ("%g-%gmm " 0xa02b4ab3a, sub_4106A450): the
     * synthetic lens is a 10-1000mm zoom. Its widest aperture stays f/1:
     * the aperture bytes (buffer 4 +0x16/+0x17, string fields 39/40, and
     * +0x19, field 512) are read unsigned, Av x 12, and f/1 is their 0;
     * the f-number table (0x4185b8b0, f x 100 from Av -2) starts at f/0.5
     * but a byte cannot reach below index 12. */
    { 0x2870, 26, { [4 + 0x10] = 80, [4 + 0x14] = 24, [4 + 0x15] = 183 } },
};

#define Z63_LENS_USER_MAX   32      /* lens-replies= entries */

typedef struct Z63Lens {
    ZfLensProfile zf_profile;
    uint64_t profile_frames;
    Z63LensReply user[Z63_LENS_USER_MAX];   /* lens-replies=, first */
    unsigned n_user;
    Z63Sio *sio;
    Z63ExtInt *extint;
    Z63Stm32 *mcu;                  /* NULL: always on the mount, powered */
    bool enabled;
    bool powered, awake, req;
    bool rdy;                       /* pin 41: true = high, not ready */
    QEMUTimer *word_timer, *rdy_timer;
    enum { Z63_LENS_IDLE, Z63_LENS_HEADER, Z63_LENS_DATA } phase;
    bool swap;                      /* first byte of a word in bits 15..8 */
    uint8_t hdr[8];
    unsigned hdr_at;
    uint16_t code;
    unsigned len, at;               /* data block length, bytes moved */
    unsigned dphase;                /* data phases since the header */
    uint8_t out[Z63_LENS_BLOCK_MAX], in[Z63_LENS_BLOCK_MAX];
    uint64_t frames, bad, headers;
    unsigned named;
} Z63Lens;

static bool z63_lens_present(Z63Lens *l)
{
    return !l->mcu || !(z63_stm32_gpio_idr(&l->mcu->gpio[6]) & 1);  /* PG0 */
}

static bool z63_lens_power_on(Z63Lens *l)
{
    return !l->mcu || (l->mcu->gpio[1].odr & 1) ||       /* PB0 */
           (l->mcu->gpio[0].odr & (1u << 9));            /* PA9 */
}

static void z63_lens_set_rdy(Z63Lens *l, bool high)
{
    if (l->rdy != high) {
        l->rdy = high;
        z63_extint_set(l->extint, Z63_LENS_RDY_LINE, high);
    }
}

/* Raise pin 41 now and drop it `ns` later: the edge the body waits for. */
static void z63_lens_pulse(Z63Lens *l, int64_t ns)
{
    z63_lens_set_rdy(l, true);
    timer_mod(l->rdy_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void z63_lens_rdy_tick(void *opaque)
{
    Z63Lens *l = opaque;

    if (l->powered && z63_lens_present(l)) {
        l->awake = true;
        z63_lens_set_rdy(l, false);
    }
}

static void z63_lens_reset(Z63Lens *l)
{
    l->awake = false;
    l->phase = Z63_LENS_IDLE;
    l->hdr_at = 0;
    timer_del(l->rdy_timer);
    z63_lens_set_rdy(l, true);
}

/* The MCU switched a power path (or the board asks again). */
static void z63_lens_power(Z63Lens *l)
{
    bool on = l->enabled && z63_lens_power_on(l) && z63_lens_present(l);

    if (on != l->powered) {
        l->powered = on;
        if (l->named < 16) {
            l->named++;
            error_report("Z63_LENS: power %s", on ? "on" : "off");
        }
        if (!on) {
            z63_lens_reset(l);
        }
    }
}

/* The body changed pin 58's bank-2 bit. */
static void z63_lens_request(Z63Lens *l, bool req)
{
    if (!l->enabled || req == l->req) {
        return;
    }
    l->req = req;
    z63_lens_power(l);
    if (l->named < 16) {
        l->named++;
        error_report("Z63_LENS: request %s (lens %s)", req ? "on" : "off",
                     l->powered ? "powered" : "unpowered");
    }
    if (req && l->powered) {
        z63_lens_pulse(l, Z63_LENS_WAKE_NS);
    }
}

static void z63_lens_log(Z63Lens *l, const char *what)
{
    GString *g;
    unsigned n = l->len < 48 ? l->len : 48;

    if (l->frames > Z63_LENS_LOG && l->frames % 100) {
        return;
    }
    g = g_string_new(NULL);
    g_string_append_printf(g, "Z63_LENS: #%" PRIu64 " cmd 0x%04x seq %u "
                           "class %u len %u%s out", l->frames, l->code,
                           l->hdr[1], l->hdr[2], l->len, what);
    for (unsigned i = 0; i < n; i++) {
        g_string_append_printf(g, " %02x", l->out[i]);
    }
    g_string_append(g, n < l->len ? " .. in" : " in");
    for (unsigned i = 0; i < n; i++) {
        g_string_append_printf(g, " %02x", l->in[i]);
    }
    error_report("%s", g->str);
    g_string_free(g, true);
}

/* The header is in: work out the data phase and prepare the reply. */
static void z63_lens_header(Z63Lens *l)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < 8; i++) {
        sum += l->hdr[i];
    }
    l->code = l->hdr[3] | (l->hdr[4] << 8);
    l->len = 2 * (l->hdr[5] | (l->hdr[6] << 8)) + 2;
    if (l->hdr[0] != 0x55 || sum || l->len > Z63_LENS_BLOCK_MAX) {
        l->bad++;
        error_report("Z63_LENS: bad header %02x %02x %02x %02x %02x %02x "
                     "%02x %02x -- ignored", l->hdr[0], l->hdr[1], l->hdr[2],
                     l->hdr[3], l->hdr[4], l->hdr[5], l->hdr[6], l->hdr[7]);
        l->hdr_at = 0;
        return;
    }
    l->phase = Z63_LENS_IDLE;
    l->dphase = 0;
    l->headers++;
    if (l->headers <= Z63_LENS_LOG && l->len > 64) {
        error_report("Z63_LENS: header cmd 0x%04x seq %u class %u, %u bytes "
                     "to come", l->code, l->hdr[1], l->hdr[2], l->len);
    }
    z63_lens_pulse(l, Z63_LENS_PREP_NS);
}

/* A data phase starts: `len` bytes each way. The lens's block is the
 * reply for the command the header named. */
static void z63_lens_data(Z63Lens *l, unsigned len)
{
    const Z63LensReply *r = NULL;
    uint8_t sum = 0;

    for (size_t i = 0; i < ARRAY_SIZE(z63_lens_replies); i++) {
        if (z63_lens_replies[i].code == l->code) {
            r = &z63_lens_replies[i];
        }
    }
    for (unsigned i = 0; i < l->n_user; i++) {
        if (l->user[i].code == l->code) {
            r = &l->user[i];
        }
    }
    l->len = MIN(MAX(len, 2u), (unsigned)Z63_LENS_BLOCK_MAX);
    memset(l->in, 0, l->len);
    memset(l->out, 0, l->len);
    l->in[0] = 0x55;
    if (r) {
        memcpy(l->in + 1, r->bytes, MIN(r->len, l->len - 2));
    }
    if (zf_lens_profile_reply(&l->zf_profile, l->code,
                              l->in + 1, l->len - 2)) {
        l->profile_frames++;
    }
    for (unsigned i = 0; i < l->len - 1; i++) {
        sum += l->in[i];
    }
    l->in[l->len - 1] = -sum;
    l->at = 0;
    l->dphase++;
    l->phase = Z63_LENS_DATA;
}

static uint16_t z63_lens_word(void *opaque, unsigned inst, uint16_t word)
{
    Z63Lens *l = opaque;
    Z63SioPio *p = &l->sio->pio[inst];
    uint8_t lo, hi;
    uint16_t back = 0;

    if (!l->awake) {
        return 0;                   /* nobody listening */
    }
    if (!p->dma) {                  /* a header word */
        if (l->phase != Z63_LENS_HEADER) {
            l->phase = Z63_LENS_HEADER;
            l->hdr_at = 0;
            l->swap = (word & 0xff) != 0x55 && (word >> 8) == 0x55;
        }
        lo = l->swap ? word >> 8 : word;
        hi = l->swap ? word : word >> 8;
        l->hdr[l->hdr_at++] = lo;
        l->hdr[l->hdr_at++] = hi;
        if (l->hdr_at == 8) {
            z63_lens_header(l);
        }
        return 0;
    }
    if (l->phase != Z63_LENS_DATA) {
        z63_lens_data(l, 2 * p->units);
    }
    lo = l->swap ? word >> 8 : word;
    hi = l->swap ? word : word >> 8;
    if (l->at + 1 < l->len) {
        l->out[l->at] = lo;
        l->out[l->at + 1] = hi;
        back = l->swap ? (l->in[l->at] << 8) | l->in[l->at + 1]
                       : l->in[l->at] | (l->in[l->at + 1] << 8);
        l->at += 2;
    }
    if (l->at + 1 >= l->len) {
        uint8_t sum = 0;

        for (unsigned i = 0; i < l->len; i++) {
            sum += l->out[i];
        }
        l->frames++;
        z63_lens_log(l, l->out[0] != 0x55 || sum ? " (bad block)" :
                        l->dphase > 1 ? " (phase 2)" : "");
        l->phase = Z63_LENS_IDLE;
        z63_lens_pulse(l, Z63_LENS_DONE_NS);
    }
    return back;
}

/* Words move in one burst per timer expiry: the whole of what is queued,
 * and in DMA mode the whole transfer (z63_sio_clock refills the FIFO from
 * memory as it goes). Clocking a FIFO's worth per expiry made a 7426-byte
 * data phase take ~90 ms of virtual time -- QEMU's timers fire ~1.5 ms
 * late -- past the 50-tick watchdog that transfer type 1 arms (the type
 * table at 0x4117a688), so the body cancelled it and asked again. */
static void z63_lens_word_tick(void *opaque)
{
    Z63Lens *l = opaque;

    for (unsigned i = 0; i < Z63_LENS_BLOCK_MAX &&
         z63_sio_clock(l->sio, Z63_LENS_SIO, Z63_SIO_FIFO, z63_lens_word, l);
         i++) {
    }
}

static void z63_lens_kick(void *opaque, unsigned inst)
{
    Z63Lens *l = opaque;

    if (!timer_pending(l->word_timer)) {
        timer_mod(l->word_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)l->sio->pio[inst].tx_queued * Z63_LENS_WORD_NS);
    }
}

/* "0x15:40+0x1f:0000": replace the built-in reply of each command named
 * with these payload bytes (hex, the bytes after the 0x55). "+" separates
 * entries because "," belongs to -machine. */
static bool z63_lens_set_replies(Z63Lens *l, const char *spec, Error **errp)
{
    g_auto(GStrv) items = g_strsplit(spec ? spec : "", "+", -1);

    l->n_user = 0;
    for (char **it = items; *it; it++) {
        Z63LensReply *r;
        const char *hex;
        unsigned long code;

        if (!**it) {
            continue;
        }
        if (l->n_user == Z63_LENS_USER_MAX ||
            qemu_strtoul(*it, &hex, 0, &code) || *hex != ':' ||
            code > 0xffff) {
            error_setg(errp, "lens-replies: '%s' is not <code>:<hex bytes>",
                       *it);
            return false;
        }
        r = &l->user[l->n_user++];
        memset(r, 0, sizeof(*r));
        r->code = code;
        hex++;
        while (hex[0] && hex[1] && r->len < sizeof(r->bytes)) {
            char b[3] = { hex[0], hex[1], 0 };

            if (!g_ascii_isxdigit(b[0]) || !g_ascii_isxdigit(b[1])) {
                error_setg(errp, "lens-replies: '%s': bad hex", *it);
                return false;
            }
            r->bytes[r->len++] = strtoul(b, NULL, 16);
            hex += 2;
        }
    }
    return true;
}

/* Pin 41's level, overlaid on port 3's data word. */
static uint16_t z63_lens_rdy_word(Z63Lens *l, uint16_t value)
{
    if (!l->enabled) {
        return value;
    }
    return (value & ~(1u << Z63_LENS_RDY_BIT)) |
           ((unsigned)l->rdy << Z63_LENS_RDY_BIT);
}

static void z63_lens_init(Z63Lens *l, Z63Sio *sio, Z63ExtInt *extint,
                          Z63Stm32 *mcu)
{
    l->sio = sio;
    l->extint = extint;
    l->mcu = mcu;
    l->enabled = true;
    l->word_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_lens_word_tick, l);
    l->rdy_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_lens_rdy_tick, l);
    sio->kick[Z63_LENS_SIO] = z63_lens_kick;
    sio->kick_opaque[Z63_LENS_SIO] = l;
    l->rdy = false;
    z63_lens_set_rdy(l, true);
    z63_lens_power(l);
    error_report("Z63_LENS: a synthetic lens on serial instance %u, ready on "
                 "EXT_INT_%u, request on GPIO port 4 bit %u -- declared "
                 "replies, see nikon-z63-lens.h (-machine lens=on)",
                 Z63_LENS_SIO, Z63_LENS_RDY_LINE, Z63_LENS_REQ_BIT);
}

#endif /* HW_ARM_NIKON_Z63_LENS_H */
