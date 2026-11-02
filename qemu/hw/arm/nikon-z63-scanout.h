/*
 * Nikon Z 6III -- scanout for the main LCD controller at 0x15480000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * nikon-z63-display.h models this block's registers: the whole 0x4000 window
 * per unit is storage, so every offset named below already reads back what
 * the firmware wrote. What was missing was a consumer -- nothing read the
 * configured framebuffer, so a correctly programmed controller would still
 * show nothing. This file is that consumer.
 *
 * Where the register meanings come from
 * ------------------------------------
 * ../n-research/z8-qemu reversed this controller from the Z 8 3.11 image and
 * wrote it up in docs/native-lcd-controller.md and
 * docs/native-lcd-minimum-tuple.md, with a reproducible extractor and
 * per-register function provenance. It is the same block: that work names
 * resource 0x1c7 with register base 0x15480000, auxiliary reset 0x15490800
 * and LUT RAM 0x15c80000, and resource 0x1c8 with 0x15484000 / 0x15490900 /
 * 0x15c84000 -- and the Z 6III image's own descriptor table says exactly the
 * same thing, independently: `.data` 0xa02808438 holds
 * {0x1c7, 0x15480000, 0x15490800, 0x15c80000} and 0xa02808470 holds
 * {0x1c8, 0x15484000, ...}. Two images, two reversing passes, one layout.
 *
 * The offsets this file reads, all relative to a unit base:
 *
 *   0x1200 & 3    output command/status; 3 is running (nikon-z63-display.h,
 *                 z63_disp_command)
 *   0x15cc        output mode bit; selects which timing pair is live
 *   0x132c/0x1328 mode 0 output width / height (1024 x 682 on the Z 8)
 *   0x13ac/0x13a8 mode 1 output width / height (1280 x 960)
 *   0x1530        output LUT enable; nonzero means a stage this does not model
 *   0x2200 & 7    pixel format
 *   0x2204        composite canvas extent: width low 16, height bits 16..29
 *   0x2208        global destination origin: x low 16, y bits 16..29
 *   0x2640        ROI enable, bits 0..9
 *   0x2400+4*s    ROI source extent: width low 16, height bits 16..29
 *   0x2440/0x2444 slot 0 primary DMA address, low and high
 *   0x2480/0x2484 + 8*(s-1)   slots 1..9 primary DMA address
 *   0x2580+4*s    primary byte stride, low 16 bits
 *   0x2600+4*s    ROI placement relative to the destination origin
 *   0x2500/0x2504 + 8*s   format 6: the slot's alpha-plane address
 *   0x25c0+4*s    format 6: the alpha plane's byte stride
 *   0x2864/0x2868/0x286c  format 6: the YUV -> RGB matrix, one row a word
 *
 * Format 6 is the one this body's GUI uses (and the Z 9's): a 4:2:2
 * surface, bytes Y0 Y1 U V for each pixel pair, with a separate one-byte
 * alpha plane. ../z9-qemu (engine1548_scanout_fetch, WonderLantern,
 * GPL-2.0-or-later) decodes it so and is the reference for what follows;
 * re-checked against this image's own frame: the GUI clears the colour
 * plane with 0x80800000 four-byte elements (Y 0, U and V 0x80 -- black)
 * and the alpha plane with zero, the alpha plane sits exactly 1024 * 682 *
 * 2 bytes after the colour plane, and the strides are 2048 and 1024. The
 * matrix rows are signed bytes in Q6 applied to (Y, U - 128, V - 128); the
 * firmware loads 0x005a0040, 0x00d2ea40, 0x00007140 -- R = Y + 1.406 V,
 * G = Y - 0.344 U - 0.719 V, B = Y + 1.766 U, BT.601 to within a code.
 * The alpha blends the plane over black: there is no video under it yet
 * (the direct input at +0x100 is idle), which is z9's behaviour with no
 * video source too. Plane addresses at 0xc00000000 and above are bus
 * addresses, 0x400000000 above DRAM, as z9 has them; that is what puts
 * these two at 0xa1e8fe400 / 0xa1ea53400.
 *
 * What it refuses to do
 * --------------------
 * Events: while the shown unit is running, every period sets bit 0 of
 * its pending word (+0x1214) and a fetched frame sets bit 16 and clears
 * the layer request at +0x2684 -- the firmware's 0x402b39c4 writes 1 there
 * to ask for the plane's new addresses and waits for it to go -- which is
 * ../z9-qemu's engine1548_scanout_tick / _fetch. The period is this file's
 * 30 Hz, not a panel timing.
 *
 * Frame boundaries. The GUI rotates its plane among several buffers: it
 * writes the next buffer's addresses into the slot registers and then asks
 * for the switch with the layer request (+0x2684; 0x402b39c4). The slot
 * registers (+0x2400..+0x26ff) are therefore latched only at a frame
 * boundary with a request pending -- and once at the first frame -- and a
 * frame is composed from the latched copy, never from registers the
 * firmware is halfway through rewriting. Reading them live showed buffers
 * the engine was still clearing and drawing: the window flickered. The
 * frame is also composed in a private buffer and copied to the console
 * whole, and the console's own refresh does not recompose; the timer below
 * is the only producer of frames.
 *
 * It does not invent a picture. A frame is produced only when the firmware
 * has programmed a configuration this code can read honestly: a nonzero
 * output size, at least one enabled ROI slot with a nonzero DMA address and
 * stride, and a format it understands. Anything else leaves
 * the console blank and is reported once, naming the register that stopped
 * it. A blank screen that says why is worth more than a plausible image
 * assembled from defaults.
 *
 * Scaling, the overlay LUT and matrix, alpha blending between slots, indexed
 * colour and the external serializer at 0x1548d000 are all unmodelled. Slots
 * are drawn in index order with no blending, which is correct only while one
 * slot is enabled; with several, the result is "what the planes contain", not
 * "what the panel would show", and the one-shot report says so.
 */

#ifndef HW_ARM_NIKON_Z63_SCANOUT_H
#define HW_ARM_NIKON_Z63_SCANOUT_H

#include <sys/mman.h>

#define Z63_DISP_R_OUTMODE   0x15cc
#define Z63_DISP_R_W0        0x132c
#define Z63_DISP_R_H0        0x1328
#define Z63_DISP_R_W1        0x13ac
#define Z63_DISP_R_H1        0x13a8
#define Z63_DISP_R_OUTLUT    0x1530
#define Z63_DISP_R_CONTROL   0x1200
#define Z63_DISP_R_LAYERREQ  0x2684
#define Z63_DISP_R_FORMAT    0x2200
#define Z63_DISP_R_CANVAS    0x2204
#define Z63_DISP_R_ORIGIN    0x2208
#define Z63_DISP_R_ROIEN     0x2640
#define Z63_DISP_R_ROIEXT    0x2400
#define Z63_DISP_R_ADDR0_LO  0x2440
#define Z63_DISP_R_ADDR0_HI  0x2444
#define Z63_DISP_R_ADDRN_LO  0x2480
#define Z63_DISP_R_STRIDE    0x2580
#define Z63_DISP_R_PLACE     0x2600
#define Z63_DISP_R_ALPHA_LO  0x2500
#define Z63_DISP_R_ASTRIDE   0x25c0
#define Z63_DISP_R_MATRIX    0x2864
#define Z63_DISP_SLOTS       10

/*
 * Which unit: 0x15480000 is resource 0x1c7 and 0x15484000 is 0x1c8. The
 * Z 8 work calls 0x1c7 the main LCD; on this body the MCU-driven start
 * programs 0x1c8 (unit 1) with 1024 x 682 at +0x1328/+0x132c and leaves
 * unit 0 alone -- 1024 x 682 x 3 is this body's 2.1M-dot rear monitor. So
 * the scanout follows whichever unit has an output size, preferring one
 * with an enabled plane, and says when it switches.
 */
#define Z63_SCANOUT_UNITS    2

/*
 * The scanout runs off its own timer rather than off the host display's
 * refresh. A real controller scans out continuously once it is enabled, so
 * this is the more faithful arrangement, and it also means the model behaves
 * the same with -display none as with a window -- which matters because the
 * refusal messages below are the only way to see what the firmware has and
 * has not programmed yet.
 *
 * Nothing the guest can observe depends on the period: no interrupt is
 * raised and no register changes. It is a host-side refresh rate and
 * nothing more.
 */
#define Z63_SCANOUT_PERIOD_NS   (NANOSECONDS_PER_SECOND / 30)

/*
 * -machine scanout-live=<file>: the last composed frame, kept in a file the
 * host maps, for a viewer outside QEMU (the z63ui repository). Host-side only,
 * like scanout-dump, but every frame rather than once a second, and without
 * a window or VNC in this build. The layout is fixed so another process can
 * read it:
 *
 *   0     magic "Z63LIVE1", written last at open
 *   8     u32 header bytes (4096), u32 pixel capacity in bytes
 *   16    u64 sequence: odd while a write is under way, bumped by two for
 *         each change of picture, size or note (a seqlock)
 *   24    u32 width, u32 height
 *   32    u64 frames composed so far (liveness; outside the seqlock)
 *   40    i64 host wall-clock ns of the last change
 *   48    note: why there is no frame, NUL-terminated; empty with a frame
 *   4096  width x height little-endian u32 0x00RRGGBB, rows packed
 *
 * A frame identical to the last one does not move the sequence, so a reader
 * only wakes for a new picture. Little-endian host only, which is every
 * host this board is run on.
 */
#define Z63_LIVE_HEADER      4096u
#define Z63_LIVE_CAPACITY    (2048u * 2048u * 4u)

typedef struct Z63LiveHeader {
    char magic[8];
    uint32_t header_bytes;
    uint32_t capacity;
    uint64_t seq;
    uint32_t width;
    uint32_t height;
    uint64_t frames;
    int64_t host_ns;
    char note[200];
} Z63LiveHeader;

QEMU_BUILD_BUG_ON(offsetof(Z63LiveHeader, seq) != 16);
QEMU_BUILD_BUG_ON(offsetof(Z63LiveHeader, note) != 48);
QEMU_BUILD_BUG_ON(sizeof(Z63LiveHeader) > Z63_LIVE_HEADER);

typedef struct Z63Scanout {
    QemuConsole *con;
    QemuConsole *panel_con[2];
    Z63LiveHeader *panel_live[2];
    bool panel_blank[2], preview_blank;
    uint64_t panel_frames[2];
    QEMUTimer *timer;
    Z63Display *disp;
    unsigned width, height;
    char *refused;              /* the last reason a frame was refused */
    bool multi_slot_reported;
    bool lut_reported;
    bool first_frame_reported;
    bool video_reported;        /* the direct input's first frame named */
    char *video_refused;        /* why it is not drawn, named once */
    uint64_t frames_video;      /* frames composed over the direct input */
    unsigned unit;              /* the unit being shown */
    bool zf_outputs;            /* own Zf uses output1 for mode1 */
    unsigned output;            /* latch identity within a unit */
    char *dump;                 /* -machine scanout-dump: a PPM, host-side */
    Z63LiveHeader *live;        /* -machine scanout-live: the mapped file */
    bool live_too_big_reported;
    uint64_t frames;
    uint64_t frames_fetched;    /* frames composed from guest planes */
    /* The slot registers as latched at the last frame boundary. */
    uint32_t layer[(0x2700 - 0x2400) / 4];
    bool latched;
    uint32_t *back;             /* the frame being composed */
    size_t back_size;
} Z63Scanout;

/* The live file's seqlock, as qemu/seqlock.h writes one. */
static void z63_live_begin(Z63LiveHeader *l)
{
    qatomic_set(&l->seq, l->seq + 1);
    smp_wmb();
}

static void z63_live_end(Z63LiveHeader *l)
{
    l->host_ns = g_get_real_time() * 1000;
    smp_wmb();
    qatomic_set(&l->seq, l->seq + 1);
}

/* A composed frame: published only if it differs from the last one. */
static void z63_scanout_live(Z63Scanout *s, const uint32_t *px,
                             unsigned w, unsigned h)
{
    Z63LiveHeader *l = s->live;
    uint32_t *dst;
    size_t bytes = (size_t)w * h * 4;

    if (!l) {
        return;
    }
    qatomic_set(&l->frames, s->frames_fetched);
    if (bytes > Z63_LIVE_CAPACITY) {
        if (!s->live_too_big_reported) {
            s->live_too_big_reported = true;
            error_report("Z63_SCANOUT: a %ux%u frame does not fit scanout-live's "
                         "%u bytes; the file keeps the last one that did",
                         w, h, Z63_LIVE_CAPACITY);
        }
        return;
    }
    dst = (uint32_t *)((uint8_t *)l + Z63_LIVE_HEADER);
    if (l->width == w && l->height == h && !l->note[0] &&
        !memcmp(dst, px, bytes)) {
        return;
    }
    z63_live_begin(l);
    l->width = w;
    l->height = h;
    l->note[0] = 0;
    memcpy(dst, px, bytes);
    z63_live_end(l);
}

/* Make and map a live file with room for `capacity` bytes of pixels. The
 * top LCD (nikon-z63-toplcd.h) publishes in the same format. */
static Z63LiveHeader *z63_live_open(const char *path, uint32_t capacity,
                                    const char *option)
{
    size_t size = Z63_LIVE_HEADER + capacity;
    Z63LiveHeader *l;
    int fd;

    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        error_report("%s: cannot make '%s': %s", option, path,
                     strerror(errno));
        exit(1);
    }
    l = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (l == MAP_FAILED) {
        error_report("%s: cannot map '%s': %s", option, path,
                     strerror(errno));
        exit(1);
    }
    l->header_bytes = Z63_LIVE_HEADER;
    l->capacity = capacity;
    smp_wmb();
    memcpy(l->magic, "Z63LIVE1", 8);
    return l;
}

static void z63_scanout_refuse(Z63Scanout *s, const char *fmt, ...)
{
    va_list ap;
    char *msg;

    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    /* Report each distinct reason once: the firmware programs this block in
     * stages and every stage would otherwise print on every frame. */
    if (!s->refused || strcmp(s->refused, msg)) {
        g_free(s->refused);
        s->refused = msg;
        error_report("Z63_SCANOUT: no frame -- %s", msg);
        if (s->live) {
            z63_live_begin(s->live);
            pstrcpy(s->live->note, sizeof(s->live->note), msg);
            z63_live_end(s->live);
        }
    } else {
        g_free(msg);
    }
}

static inline uint32_t z63_disp_reg(Z63Scanout *s, unsigned off)
{
    if (s->latched && off >= 0x2400 && off < 0x2700) {
        return s->layer[(off - 0x2400) / 4];
    }
    return s->disp->unit_regs[s->unit][off / 4];
}

/* Own Zf writes +1280 when selecting mode1 (402df6e0 ->402b2b4c).
 * These outputs share composition registers, but have distinct command and
 * pending/mask words. Never start an output from the host. */
static unsigned zf_scanout_output_delta(Z63Scanout *s)
{
    return s->zf_outputs && (s->disp->unit_regs[s->unit][Z63_DISP_R_OUTMODE / 4] & 1)
           ? 0x80 : 0;
}

/* A frame boundary: take the slot registers if a switch was requested. */
static bool z63_scanout_latch(Z63Scanout *s)
{
    uint32_t *r = s->disp->unit_regs[s->unit];
    unsigned output = zf_scanout_output_delta(s);
    if (s->output != output) {
        s->output = output;
        s->latched = false;
    }

    /* Until a slot is enabled there is no frame to keep, so follow the
     * registers; after that, only a request moves the latch. */
    if (s->latched && !r[0x2684 / 4] &&
        (s->layer[(Z63_DISP_R_ROIEN - 0x2400) / 4] & 0x3ff)) {
        return false;
    }
    memcpy(s->layer, &r[0x2400 / 4], sizeof(s->layer));
    s->latched = true;
    return true;
}

/* The unit with an output size, an enabled plane first. */
static void z63_scanout_pick(Z63Scanout *s)
{
    int best = -1, sized = -1;

    for (unsigned u = 0; u < Z63_SCANOUT_UNITS; u++) {
        uint32_t *r = s->disp->unit_regs[u];
        unsigned mode = r[Z63_DISP_R_OUTMODE / 4] & 1;
        uint32_t w = r[(mode ? Z63_DISP_R_W1 : Z63_DISP_R_W0) / 4];
        uint32_t h = r[(mode ? Z63_DISP_R_H1 : Z63_DISP_R_H0) / 4];

        if (!w || !h) {
            continue;
        }
        if (sized < 0) {
            sized = u;
        }
        if (best < 0 && (r[Z63_DISP_R_ROIEN / 4] & 0x3ff)) {
            best = u;
        }
    }
    if (best < 0) {
        best = sized;
    }
    if (best >= 0 && (unsigned)best != s->unit) {
        s->unit = best;
        s->latched = false;
        error_report("Z63_SCANOUT: following display unit %u (resource "
                     "0x%x, 0x%x)", s->unit, 0x1c7 + s->unit,
                     0x15480000 + s->unit * 0x4000);
    }
}

/* Host-side only: the composed frame as a binary PPM, about once a second
 * while frames are being produced. Nothing the guest sees changes. */
static void z63_scanout_dump(Z63Scanout *s, const uint32_t *px,
                             unsigned w, unsigned h)
{
    g_autofree char *tmp = NULL;
    g_autofree uint8_t *rgb = NULL;
    FILE *f;

    if (!s->dump || (s->frames++ % 30)) {
        return;
    }
    tmp = g_strdup_printf("%s.tmp", s->dump);
    rgb = g_malloc((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3] = px[i] >> 16;
        rgb[i * 3 + 1] = px[i] >> 8;
        rgb[i * 3 + 2] = px[i];
    }
    f = fopen(tmp, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    fclose(f);
    rename(tmp, s->dump);
}

/* The controller packs a width/height or x/y pair into one word: the first
 * in the low 16 bits, the second in bits 16..29. */
static inline void z63_disp_pair(uint32_t v, unsigned *a, unsigned *b)
{
    *a = v & 0xffff;
    *b = (v >> 16) & 0x3fff;
}

/* A plane address as the controller holds it, to a guest physical one;
 * see "Format 6" above. 0 when it does not land in DRAM. */
static hwaddr z63_scanout_phys(uint64_t bus, uint64_t len)
{
    if (bus >> 34 == 3) {
        bus -= 0x400000000ULL;
    }
    if (bus < Z63_DRAM_BASE || bus + len > Z63_DRAM_BASE + Z63_DRAM_SIZE) {
        return 0;
    }
    return bus;
}

/* Format 6, one slot: 4:2:2 Y0 Y1 U V plus an A8 plane, through the
 * matrix, over what is already in dst -- the direct input's picture, or
 * black. Returns false (after saying why) if it cannot. */
static bool z63_scanout_yuva(Z63Scanout *s, unsigned slot, uint64_t base,
                             unsigned sw, unsigned sh, unsigned stride,
                             unsigned dx0, unsigned dy0, unsigned w,
                             unsigned h, uint32_t *dst)
{
    unsigned o = Z63_DISP_R_ALPHA_LO + 8 * slot;
    uint64_t abus = z63_disp_reg(s, o) |
                    ((uint64_t)(z63_disp_reg(s, o + 4) & 0xf) << 32);
    unsigned astride = z63_disp_reg(s, Z63_DISP_R_ASTRIDE + 4 * slot) & 0xffff;
    hwaddr color = z63_scanout_phys(base, (uint64_t)stride * sh);
    hwaddr alpha = z63_scanout_phys(abus, (uint64_t)astride * sh);
    g_autofree uint8_t *crow = NULL, *arow = NULL;
    int m[9];

    if (!color || !alpha || stride < sw * 2 || astride < sw || (sw & 1)) {
        z63_scanout_refuse(s, "slot %u, format 6: colour %#" PRIx64 " stride "
                           "%u, alpha %#" PRIx64 " stride %u, width %u -- not "
                           "a pair of planes in DRAM", slot, base, stride,
                           abus, astride, sw);
        return false;
    }
    for (unsigned r = 0; r < 3; r++) {
        uint32_t word = z63_disp_reg(s, Z63_DISP_R_MATRIX + 4 * r);

        for (unsigned c = 0; c < 3; c++) {
            m[r * 3 + c] = (int8_t)(word >> (c * 8));
        }
    }
    crow = g_malloc(stride);
    arow = g_malloc(astride);
    for (unsigned y = 0; y < sh && dy0 + y < h; y++) {
        cpu_physical_memory_read(color + (uint64_t)y * stride, crow, stride);
        cpu_physical_memory_read(alpha + (uint64_t)y * astride, arow, astride);
        for (unsigned x = 0; x < sw && dx0 + x < w; x++) {
            const uint8_t *pair = crow + (x & ~1u) * 2;
            int in[3] = { pair[x & 1], pair[2] - 128, pair[3] - 128 };
            unsigned a = arow[x];
            uint32_t *out = &dst[(dy0 + y) * w + dx0 + x];
            uint32_t below = *out, px = 0;

            for (unsigned r = 0; r < 3; r++) {
                int v = CLAMP((m[r * 3] * in[0] + m[r * 3 + 1] * in[1] +
                               m[r * 3 + 2] * in[2] + 32) >> 6, 0, 255);
                unsigned b = (below >> (16 - 8 * r)) & 0xff;

                px |= (uint32_t)((v * a + b * (255 - a) + 127) / 255)
                      << (16 - 8 * r);
            }
            *out = px;
        }
    }
    return true;
}

/*
 * The direct input: the picture under the slots -- live view. The display
 * driver's direct-source record, sub_402B172C (this image; ../z9-qemu's
 * 402ab478 for the Z 9, same layout), writes +0x200 format (low 5 bits),
 * +0x204, +0x208 size (width low 16, height bits 16..29), +0x250/+0x254
 * the Y and C strides, +0x800/+0x804, +0x268/+0x270/+0x274; sub_402B14F8
 * and sub_402B1588 the Y (+0x210/+0x214) and C (+0x230/+0x234) addresses;
 * +0x100 bits 0 and 1 enable it. The live-view display path programs it
 * from the frame it posts (sub_40CD94B4 -> sub_40CD9A68 -> sub_402BCCB8):
 * format 4, 1024 x 682, strides 1024, Y and C in the RSZ output ring.
 *
 * Format 4 is read as NV12 -- a Y plane and an interleaved U, V plane at
 * half height -- converted with ../z9-qemu's BT.601 integers
 * (engine1548_scanout_fetch), placed at the top left and clipped, without
 * scaling. Other formats are named once and not drawn. The slots are then
 * blended over it.
 */
static bool z63_scanout_video(Z63Scanout *s, unsigned w, unsigned h,
                              uint32_t *dst)
{
    uint32_t fmt = z63_disp_reg(s, 0x200) & 31;
    uint32_t size = z63_disp_reg(s, 0x208);
    unsigned vw = size & 0xffff, vh = (size >> 16) & 0x3fff;
    unsigned ys = z63_disp_reg(s, 0x250) & 0xffff;
    unsigned cs = z63_disp_reg(s, 0x254) & 0xffff;
    uint64_t ybus = z63_disp_reg(s, 0x210) |
                    ((uint64_t)(z63_disp_reg(s, 0x214) & 0xf) << 32);
    uint64_t cbus = z63_disp_reg(s, 0x230) |
                    ((uint64_t)(z63_disp_reg(s, 0x234) & 0xf) << 32);
    hwaddr yp, cp;
    g_autofree uint8_t *yrow = NULL, *crow = NULL;
    g_autofree char *why = NULL;

    if ((z63_disp_reg(s, 0x100) & 3) != 3) {
        return false;                           /* not enabled: black */
    }
    yp = z63_scanout_phys(ybus, (uint64_t)ys * vh);
    cp = z63_scanout_phys(cbus, (uint64_t)cs * ((vh + 1) / 2));
    if (fmt != 4) {
        why = g_strdup_printf("format %u (+0x200), not 4", fmt);
    } else if (!vw || !vh || (vw & 1) || ys < vw || cs < vw || !yp || !cp) {
        why = g_strdup_printf("%ux%u, strides %u/%u, Y %#" PRIx64 " C %#"
                              PRIx64 " -- not a pair of planes in DRAM",
                              vw, vh, ys, cs, ybus, cbus);
    }
    if (why) {
        if (!s->video_refused || strcmp(s->video_refused, why)) {
            error_report("Z63_SCANOUT: direct input enabled but not drawn: "
                         "%s", why);
            g_free(s->video_refused);
            s->video_refused = g_steal_pointer(&why);
        }
        return false;
    }
    yrow = g_malloc(ys);
    crow = g_malloc(cs);
    for (unsigned y = 0; y < vh && y < h; y++) {
        cpu_physical_memory_read(yp + (uint64_t)y * ys, yrow, ys);
        if (!(y & 1)) {
            cpu_physical_memory_read(cp + (uint64_t)(y / 2) * cs, crow, cs);
        }
        for (unsigned x = 0; x < vw && x < w; x++) {
            int vy = yrow[x];
            int vu = crow[x & ~1u] - 128, vv = crow[(x & ~1u) + 1] - 128;
            int r = CLAMP(vy + ((359 * vv + 128) >> 8), 0, 255);
            int g = CLAMP(vy - ((88 * vu + 183 * vv + 128) >> 8), 0, 255);
            int b = CLAMP(vy + ((454 * vu + 128) >> 8), 0, 255);

            dst[y * w + x] = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        }
    }
    if (!s->video_reported) {
        s->video_reported = true;
        error_report("Z63_SCANOUT: direct input (live view) under the slots "
                     "-- %ux%u NV12, Y %#" PRIx64 " C %#" PRIx64 ", strides "
                     "%u/%u", vw, vh, ybus, cbus, ys, cs);
    }
    s->frames_video++;
    return true;
}

/* Host presentation only. A stopped output is black, not a frozen frame.
 * The frame counter does not advance when blanking an inactive panel. */
static void zf_panel_blank(QemuConsole *con, Z63LiveHeader *live)
{
    DisplaySurface *surface = qemu_console_surface(con);
    unsigned w = surface_width(surface), h = surface_height(surface);
    memset(surface_data(surface), 0, (size_t)surface_stride(surface) * h);
    dpy_gfx_update_full(con);
    if (live) {
        z63_live_begin(live);
        size_t bytes = (size_t)w * h * 4;
        bool fits = bytes <= live->capacity;
        live->width = fits ? w : 0;
        live->height = fits ? h : 0;
        if (fits) {
            memset((uint8_t *)live + Z63_LIVE_HEADER, 0, bytes);
        }
        pstrcpy(live->note, sizeof(live->note),
                fits ? "inactive output" : "inactive output exceeds live capacity");
        z63_live_end(live);
    }
}

static void zf_panel_select(Z63Scanout *s, unsigned mode)
{
    bool running;
    if (!s->zf_outputs) {
        return;
    }
    running = (z63_disp_reg(s, Z63_DISP_R_CONTROL +
                           zf_scanout_output_delta(s)) & 3) == 3;
    s->con = s->panel_con[mode];
    for (unsigned i = 0; i < 2; i++) {
        bool active = running && i == mode;
        if (!active && !s->panel_blank[i]) {
            zf_panel_blank(s->panel_con[i], s->panel_live[i]);
        }
        s->panel_blank[i] = !active;
    }
    if (!running && !s->preview_blank) {
        zf_panel_blank(s->con, s->live);
    }
    s->preview_blank = !running;
}

static void zf_panel_frame(Z63Scanout *s, unsigned mode, const uint32_t *px,
                           unsigned w, unsigned h)
{
    Z63LiveHeader *preview;
    if (!s->zf_outputs) {
        return;
    }
    s->panel_frames[mode]++;
    if (!s->panel_live[mode]) {
        return;
    }
    preview = s->live;
    s->live = s->panel_live[mode];
    z63_scanout_live(s, px, w, h);
    qatomic_set(&s->live->frames, s->panel_frames[mode]);
    s->live = preview;
}

static void z63_scanout_update(void *opaque)
{
    Z63Scanout *s = opaque;
    DisplaySurface *surf;
    unsigned mode, w, h, fmt, enable, slot, drawn = 0;
    unsigned ox, oy;
    uint32_t *dst;

    z63_scanout_pick(s);
    mode = z63_disp_reg(s, Z63_DISP_R_OUTMODE) & 1;
    zf_panel_select(s, mode);
    w = z63_disp_reg(s, mode ? Z63_DISP_R_W1 : Z63_DISP_R_W0);
    h = z63_disp_reg(s, mode ? Z63_DISP_R_H1 : Z63_DISP_R_H0);

    if (!w || !h || w > 4096 || h > 4096) {
        z63_scanout_refuse(s, "output size is %ux%u (mode %u, from +%#x/+%#x)",
                           w, h, mode,
                           mode ? Z63_DISP_R_W1 : Z63_DISP_R_W0,
                           mode ? Z63_DISP_R_H1 : Z63_DISP_R_H0);
        return;
    }
    if ((z63_disp_reg(s, Z63_DISP_R_CONTROL + zf_scanout_output_delta(s)) & 3) != 3) {
        z63_scanout_refuse(s, "the output is not running (+%#x is %#x)",
                           Z63_DISP_R_CONTROL + zf_scanout_output_delta(s),
                           z63_disp_reg(s, Z63_DISP_R_CONTROL + zf_scanout_output_delta(s)));
        return;
    }
    enable = z63_disp_reg(s, Z63_DISP_R_ROIEN) & 0x3ff;
    if (!enable) {
        z63_scanout_refuse(s, "no ROI slot is enabled (+%#x is 0)",
                           Z63_DISP_R_ROIEN);
        return;
    }
    fmt = z63_disp_reg(s, Z63_DISP_R_FORMAT) & 7;
    if (fmt != 0 && fmt != 2 && fmt != 6) {
        z63_scanout_refuse(s, "pixel format %u (+%#x) is not one of the "
                           "three established (0, 2, 6)", fmt,
                           Z63_DISP_R_FORMAT);
        return;
    }
    /*
     * The output LUT stage (+0x1530 bits 0 and 8; the three RGB tables and
     * two common tables in LUT RAM, nikon-z63-display.h) is not applied.
     * DECLARED CHOICE: this used to refuse the frame. ../z9-qemu's scanout
     * does not apply it either; ../n-research/z8-qemu has an opt-in model
     * of it that its own notes call a hypothesis. A frame without it shows
     * what the planes hold, before the panel's gamma; say so once.
     */
    if (z63_disp_reg(s, Z63_DISP_R_OUTLUT) && !s->lut_reported) {
        s->lut_reported = true;
        error_report("Z63_SCANOUT: output LUT enable at +%#x is %#x; that "
                     "stage is not applied (see nikon-z63-scanout.h)",
                     Z63_DISP_R_OUTLUT, z63_disp_reg(s, Z63_DISP_R_OUTLUT));
    }

    if (s->width != w || s->height != h) {
        qemu_console_resize(s->con, w, h);
        s->width = w;
        s->height = h;
    }
    surf = qemu_console_surface(s->con);
    if (!surf || surface_format(surf) != PIXMAN_x8r8g8b8) {
        return;
    }
    if (s->back_size != (size_t)w * h) {
        s->back_size = (size_t)w * h;
        s->back = g_renew(uint32_t, s->back, s->back_size);
    }
    dst = s->back;
    memset(dst, 0, (size_t)w * h * 4);
    z63_scanout_video(s, w, h, dst);

    z63_disp_pair(z63_disp_reg(s, Z63_DISP_R_ORIGIN), &ox, &oy);

    for (slot = 0; slot < Z63_DISP_SLOTS; slot++) {
        unsigned sw, sh, px, py, stride, y;
        uint64_t base;
        g_autofree uint8_t *row = NULL;

        if (!(enable & (1u << slot))) {
            continue;
        }
        z63_disp_pair(z63_disp_reg(s, Z63_DISP_R_ROIEXT + 4 * slot), &sw, &sh);
        z63_disp_pair(z63_disp_reg(s, Z63_DISP_R_PLACE + 4 * slot), &px, &py);
        stride = z63_disp_reg(s, Z63_DISP_R_STRIDE + 4 * slot) & 0xffff;
        if (slot == 0) {
            base = z63_disp_reg(s, Z63_DISP_R_ADDR0_LO) |
                   ((uint64_t)z63_disp_reg(s, Z63_DISP_R_ADDR0_HI) << 32);
        } else {
            unsigned o = Z63_DISP_R_ADDRN_LO + 8 * (slot - 1);

            base = z63_disp_reg(s, o) | ((uint64_t)z63_disp_reg(s, o + 4) << 32);
        }
        if (!sw || !sh || !stride || !base) {
            z63_scanout_refuse(s, "slot %u is enabled but incomplete: "
                               "%ux%u, stride %u, address %#" PRIx64,
                               slot, sw, sh, stride, base);
            continue;
        }

        if (fmt == 6) {
            if (z63_scanout_yuva(s, slot, base, sw, sh, stride, ox + px,
                                 oy + py, w, h, dst)) {
                drawn++;
            }
            continue;
        }
        row = g_malloc(stride);
        for (y = 0; y < sh; y++) {
            unsigned dy = oy + py + y, x;

            if (dy >= h) {
                break;
            }
            cpu_physical_memory_read(base + (uint64_t)y * stride, row, stride);
            for (x = 0; x < sw; x++) {
                unsigned dx = ox + px + x;
                uint32_t p;

                if (dx >= w) {
                    break;
                }
                if (fmt == 0) {
                    /* bytes R, G, B, A -- the selectors at +0x2210 are
                     * 03020100 for this format in the Z 8 work. */
                    if ((x + 1) * 4 > stride) {
                        break;
                    }
                    p = ((uint32_t)row[x * 4 + 0] << 16) |
                        ((uint32_t)row[x * 4 + 1] << 8) |
                        row[x * 4 + 2];
                } else {
                    /* little-endian u16 0xABGR, four bits per channel */
                    uint16_t v;

                    if ((x + 1) * 2 > stride) {
                        break;
                    }
                    v = row[x * 2] | ((uint16_t)row[x * 2 + 1] << 8);
                    p = (((uint32_t)(v & 0xf) * 0x11) << 16) |
                        (((uint32_t)((v >> 4) & 0xf) * 0x11) << 8) |
                        ((uint32_t)((v >> 8) & 0xf) * 0x11);
                }
                dst[dy * w + dx] = p;
            }
        }
        drawn++;
    }

    if (drawn > 1 && !s->multi_slot_reported) {
        s->multi_slot_reported = true;
        error_report("Z63_SCANOUT: %u ROI slots are enabled at once. They are "
                     "drawn in index order with no blending and no scaling, "
                     "so this shows what the planes contain, not what the "
                     "panel would show.", drawn);
    }
    if (drawn) {
        if (!s->first_frame_reported) {
            s->first_frame_reported = true;
            error_report("Z63_SCANOUT: first frame -- %ux%u mode %u format %u,"
                         " %u slot(s), slot 0 at %#" PRIx64 " stride %u",
                         w, h, mode, fmt, drawn,
                         z63_disp_reg(s, Z63_DISP_R_ADDR0_LO) |
                         ((uint64_t)z63_disp_reg(s, Z63_DISP_R_ADDR0_HI) << 32),
                         z63_disp_reg(s, Z63_DISP_R_STRIDE) & 0xffff);
        }
        g_free(s->refused);
        s->refused = NULL;
        s->frames_fetched++;
        memcpy(surface_data(surf), dst, (size_t)w * h * 4);
        z63_scanout_dump(s, dst, w, h);
        z63_scanout_live(s, dst, w, h);
        zf_panel_frame(s, mode, dst, w, h);
        dpy_gfx_update_full(s->con);
    }
}

static void z63_scanout_tick(void *opaque)
{
    Z63Scanout *s = opaque;
    Z63DispWin *u = &s->disp->unit[s->unit];
    uint64_t before = s->frames_fetched;
    bool latched;
    unsigned delta;

    z63_scanout_pick(s);
    u = &s->disp->unit[s->unit];
    delta = zf_scanout_output_delta(s);
    latched = z63_scanout_latch(s);
    z63_scanout_update(s);
    if ((z63_disp_reg(s, Z63_DISP_R_CONTROL + zf_scanout_output_delta(s)) & 3) == 3) {
        u->words[(Z63_DISP_R_PEND0 + delta) / 4] |= 1;
        if (s->frames_fetched != before) {
            u->words[(Z63_DISP_R_PEND0 + delta) / 4] |= 0x10000;
            if (latched) {
                u->words[Z63_DISP_R_LAYERREQ / 4] = 0;
            }
        }
        z63_disp_irq_update(u);
    }
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                        Z63_SCANOUT_PERIOD_NS);
}

/* The console's refresh: the timer has already put the last frame in the
 * surface, so there is nothing to compose here. */
static void z63_scanout_refresh(void *opaque)
{
}

static const GraphicHwOps z63_scanout_ops = {
    .gfx_update = z63_scanout_refresh,
};

static void z63_scanout_init(Z63Display *disp, Z63Scanout *s,
                             const char *dump, const char *live)
{
    s->disp = disp;
    s->dump = g_strdup(dump);
    if (live && *live) {
        s->live = z63_live_open(live, Z63_LIVE_CAPACITY, "scanout-live");
    }
    /* No DeviceState: this board builds its blocks as plain memory regions
     * rather than as qdev devices, and the console does not need one. */
    s->con = graphic_console_init(NULL, 0, &z63_scanout_ops, s);
    /* Until the firmware programs a size, the console is the Z 8's mode-0
     * geometry so there is a window to look at; the first update replaces it
     * with whatever the registers actually say. */
    qemu_console_resize(s->con, 1024, 682);
    s->width = 1024;
    s->height = 682;

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_scanout_tick, s);
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                        Z63_SCANOUT_PERIOD_NS);
}

static char *zf_panel_state(Z63Scanout *s)
{
    DisplaySurface *a = qemu_console_surface(s->panel_con[0]);
    DisplaySurface *b = qemu_console_surface(s->panel_con[1]);
    return g_strdup_printf("{\"unit\":%u,\"mode\":%u,"
        "\"lcd\":{\"console\":%d,\"width\":%d,\"height\":%d,"
        "\"blank\":%s,\"frames\":%" PRIu64 "},"
        "\"evf\":{\"console\":%d,\"width\":%d,\"height\":%d,"
        "\"blank\":%s,\"frames\":%" PRIu64 "}}",
        s->unit, z63_disp_reg(s, Z63_DISP_R_OUTMODE) & 1,
        qemu_console_get_index(s->panel_con[0]), surface_width(a), surface_height(a),
        s->panel_blank[0] ? "true" : "false", s->panel_frames[0],
        qemu_console_get_index(s->panel_con[1]), surface_width(b), surface_height(b),
        s->panel_blank[1] ? "true" : "false", s->panel_frames[1]);
}

static bool zf_panel_snapshot(Z63Scanout *s, const char *prefix, Error **errp)
{
    for (unsigned i = 0; i < 2; i++) {
        DisplaySurface *surface = qemu_console_surface(s->panel_con[i]);
        unsigned w = surface_width(surface), h = surface_height(surface);
        g_autofree char *path = g_strdup_printf("%s.%s.ppm", prefix, i ? "evf" : "lcd");
        if (surface_format(surface) != PIXMAN_x8r8g8b8) {
            error_setg(errp, "panel %u has unsupported host pixel format", i);
            return false;
        }
        FILE *f = fopen(path, "wbx");
        if (!f) {
            error_setg_errno(errp, errno, "cannot create panel snapshot %s", path);
            return false;
        }
        g_autofree uint8_t *row = g_malloc((size_t)w * 3);
        bool ok = fprintf(f, "P6\n%u %u\n255\n", w, h) > 0;
        for (unsigned y = 0; ok && y < h; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)surface_data(surface) +
                                                     (size_t)y * surface_stride(surface));
            for (unsigned x = 0; x < w; x++) {
                row[x * 3] = src[x] >> 16;
                row[x * 3 + 1] = src[x] >> 8;
                row[x * 3 + 2] = src[x];
            }
            ok = fwrite(row, 3, w, f) == w;
        }
        if (fclose(f)) {
            ok = false;
        }
        if (!ok) {
            error_setg(errp, "panel snapshot write failed: %s (partial file retained)", path);
            return false;
        }
    }
    g_autofree char *path = g_strdup_printf("%s.json", prefix);
    FILE *f = fopen(path, "wbx");
    if (!f) {
        error_setg_errno(errp, errno, "cannot create panel snapshot state %s", path);
        return false;
    }
    g_autofree char *state = zf_panel_state(s);
    bool ok = fprintf(f, "%s\n", state) > 0;
    if (fclose(f)) {
        ok = false;
    }
    if (!ok) {
        error_setg(errp, "panel snapshot state write failed: %s", path);
    }
    return ok;
}

/* Independent consoles and optional files, derived from scanout-live.
 * Initial sizes are host placeholders; original timing registers determine
 * the size of every produced frame. No output is enabled by this function. */
static void zf_scanout_panels_init(Z63Scanout *s, const char *live)
{
    if (!s->zf_outputs) {
        return;
    }
    s->panel_con[0] = s->con;
    s->panel_con[1] = graphic_console_init(NULL, 1, &z63_scanout_ops, s);
    qemu_console_resize(s->panel_con[1], 1280, 960);
    for (unsigned i = 0; i < 2; i++) {
        if (live && *live) {
            g_autofree char *path = g_strdup_printf("%s.%s", live,
                                                   i ? "evf" : "lcd");
            s->panel_live[i] = z63_live_open(path, Z63_LIVE_CAPACITY,
                                           "scanout-live panel");
        }
        zf_panel_blank(s->panel_con[i], s->panel_live[i]);
        s->panel_blank[i] = true;
    }
    s->preview_blank = true;
    error_report("ZF_PANELS: LCD console %d, EVF console %d; independent "
                 "scanout-live.lcd/.evf, inactive outputs black",
                 qemu_console_get_index(s->panel_con[0]),
                 qemu_console_get_index(s->panel_con[1]));
}

#endif /* HW_ARM_NIKON_Z63_SCANOUT_H */
