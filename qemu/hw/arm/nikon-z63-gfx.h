/*
 * Nikon Z 6III -- the graphics engine at 0x15540000: its two command FIFOs
 * and its DRAM command ring.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * What it is
 * ----------
 * The 2D engine the RTOS draws with. ../z9-qemu (WonderLantern,
 * GPL-2.0-or-later) models the same block on the Z 9 -- same base, same
 * register file, same 0xDECAFACE trailer -- and calls it "graphics". Its
 * reading was checked against this image rather than taken on trust: every
 * behaviour below cites the Z 6III code that demands it.
 *
 * The driver reaches it through one accessor pair whose base sub_4026B46C
 * hard-codes as 0x15540000: sub_4026B364(off) reads and sub_4026B30C(off,
 * val) writes. The register file:
 *
 *   +0x00  W: push a word into FIFO A (sub_4026B384). R: free entries --
 *          sub_4026AFC0 reads it until enough are free, then counts down
 *   +0x08  R: capability; bit 4 selects a 512- or 256-word FIFO
 *          (sub_4026683C)
 *   +0x0c  event enable; +0x10 event acknowledge, write one to clear
 *          (sub_4026B24C / sub_4026B288); +0x14 events pending, which
 *          sub_4026B214 spins on until pending & enabled is non-zero
 *   +0x18  stream mode: bit 0 ends every FIFO A packet with 0xDECAFACE,
 *          bit 1 every FIFO B packet (sub_40266774 reads it into
 *          0xa0280dcec / 0xa0280dcf0, and the write accessor mirrors them)
 *   +0x1c  read-back data, returned after a read packet (sub_40268510)
 *   +0x20  busy bits; callers spin while (read & mask) != 0, e.g. the
 *          `& 0x2a4` after every ring submission (sub_4020CBE0)
 *   +0x34  capability, read once by sub_4021FB3C (>> 2)
 *   +0x60  W: push a word into FIFO B (sub_4026B428). R: free entries
 *          (sub_4026B06C)
 *   +0x70  ring base, a bus address; +0x74 ring size - 1 (31); +0x78 W:
 *          doorbell, R: descriptors outstanding; +0x80 ring enable (1)
 *   +0x58, +0x5c, +0x7c, +0x84, +0xe4, +0x164  written 0 by the init table
 *          at 0x4184f42c and by nothing else
 *
 * FIFO A
 * ------
 * A packet is `op << 24 | reg << 12 | n`, n payload words, and 0xDECAFACE
 * when +0x18 bit 0 is set. The builders are all in 0x40265d8c-0x4026ae44:
 *
 *   0x42 sub_40265E94  n words (sub_402696D8, sub_40269818 ...)
 *   0x43 sub_402698D8  n words, reg in bits 23:14   0x44 sub_40269A4C
 *   0x45 sub_40269568  exactly one word             0x46 sub_4026AE44
 *   0x47 sub_40266600  n words of zero              0x48 sub_40267BAC
 *   0x49 sub_40267D0C, 0x4a sub_40267DA0  data streamed to a table
 *   0x51-0x55, 0x58    read-back (sub_40269AEC ... sub_40267B4C)
 *
 * Writes are kept per opcode, one 4096-register bank each. Streams are
 * counted and not kept: nothing here reads a table back.
 *
 * ../z9-qemu treats every 0x42 packet as the geometry of a clear. Here
 * 0x42 is plainly a register bank: sub_40265E94 is the same builder as
 * 0x48's, and the first one the firmware sends is reg 4, one word, from
 * sub_402696D8 -- the selector every init sequence starts with. The clear
 * itself starts at 0x48 reg 0xb00, which still stops.
 *
 * Instructions. A word `0xa0000000 | selector << 7` (bit 27 clear) selects
 * a program in the engine's microcode, an opcode-0x80 packet hands it
 * parameters, and 0xa8000000 runs it. The selectors are the same as the
 * Z 9's -- the first this firmware sends is 0x123b, which ../z9-qemu
 * observed as the setup transaction -- so its reading of them is used, and
 * only the ones it established as configuration are accepted: 0x123b
 * (setup: an empty geometry queue), 0x8d9 / 0x8ca (image controls, one
 * word each), the stroke caps 0x7cd / 0x7d0 / 0x7ca and joins 0x935 /
 * 0x937 / 0x939. The ones that draw -- 0x281 image, 0x826 glyphs, 0x5e5 /
 * 0x5ec paths and the rest -- stop, naming the selector.
 *
 * Three writes are fences. The firmware enables an event bit, issues the
 * write, and spins on that bit:
 *
 *   0x48 reg 0 = 1    bit 24   sub_40267C70(0, 1, 2)
 *   0x45 reg 28 = 1   bit 25   sub_4026645C
 *   0x46 reg 113 = 1  bit 3    sub_40266528
 *
 * FIFO B
 * ------
 * `reg << 16 | op << 12 | (n - 1)`, little-endian halves:
 *
 *   0xb  up to four (register, value) pairs; after the first pair each
 *        register comes alone in the high half (sub_40269370)
 *   0xc  read-back (sub_402694A8)
 *   0xd  fence: bit 28 (sub_402692B8 -- `0xD000` after the write)
 *
 * The ring
 * --------
 * The firmware records FIFO A packets into a DRAM buffer instead of the
 * FIFO (mode 2: sub_40266090), ends it with the bit-24 fence, writes one
 * descriptor {words - 1, buffer >> 2 | 0x40000000} into the 32-slot ring at
 * +0x70, writes 1 to +0x78, and waits for bit 24 (sub_4020CBE0). When the
 * ring is full it waits for bit 11 instead, so that is what a finished
 * descriptor raises.
 *
 * Ring and buffer addresses are bus addresses, translated by the NoC
 * window group the firmware programs for this engine: group 15, read ports
 * on the even slots (window ids 180-185 -> slots 0, 2 ... 10, registered by
 * sub_402EEF20). nikon-z63-noc.h has the formula and the provisioning.
 *
 * What this does NOT do: draw
 * ---------------------------
 * A fence says that everything before it has been done. Configuration is
 * done the moment it is stored, so a fence after configuration is honest.
 * A fence after a drawing command would claim pixels that were never
 * written, so drawing stops the machine instead, naming the command:
 * 0x48 reg 0xb00 (clear start) and reg 0x640
 * (draw start), the 0xa0..0xaf instruction family, the 0x80000001 /
 * 0x80000005 words, path-program descriptors, and every read-back. Those
 * are the operations ../z9-qemu found it had to execute for a menu, and
 * each will be modelled here when the firmware reaches it -- not before.
 *
 * DECLARED CHOICES: +0x08 and +0x34 read 0 (no capabilities: a 256-word
 * FIFO A), FIFO B reports 128 free words, +0x18 resets to 0, every
 * packet is executed synchronously so +0x20 always reads idle, and there
 * is no interrupt line -- the driver only ever polls.
 */

#ifndef HW_ARM_NIKON_Z63_GFX_H
#define HW_ARM_NIKON_Z63_GFX_H

#include <math.h>
#include "qemu/bitops.h"

#define Z63_GFX_BASE         0x15540000ULL
#define Z63_GFX_SIZE         0x10000
#define Z63_GFX_TRAILER      0xdecafaceu
#define Z63_GFX_FIFO_A       256
#define Z63_GFX_FIFO_B       128
#define Z63_GFX_RING_SLOTS   32
#define Z63_GFX_MAX_LIST     4096     /* words in one ring descriptor */

#define Z63_GFX_EV_RING      (1u << 11)
#define Z63_GFX_EV_A48       (1u << 24)
#define Z63_GFX_EV_A45       (1u << 25)
#define Z63_GFX_EV_A46       (1u << 3)
#define Z63_GFX_EV_B         (1u << 28)

#define Z63_GFX_NOC_GROUP    15
#define Z63_GFX_PATH_EDGES   32768u

typedef struct Z63GfxEdge {
    double x0, y0, x1, y1;
} Z63GfxEdge;

typedef struct Z63Gfx {
    MemoryRegion mr;
    Z63Noc *noc;
    uint32_t enable, pending, mode, ring_base, ring_size, ring_on;
    uint32_t idle_regs[6];             /* the init table's zero writes */
    /* FIFO A */
    uint32_t header, need, got;
    bool trailer;
    uint32_t payload[4096];
    uint32_t bank[7][4096];            /* opcodes 0x42..0x48 */
    bool written48[4096];              /* which 0x48 registers were set */
    uint32_t selector;                 /* the program an 0xa0 word chose */
    bool set_up;                       /* selector 0x123b has run */
    uint32_t image_control[2];
    uint32_t stroke_cap, stroke_join;
    /* tables streamed by 0x49 through the address in 0x48 reg 0x30 */
    uint32_t table_address;
    uint32_t tables[2][192];
    uint32_t color_tables[6][256];
    bool color_written[6][256];
    /* the image program, 0x281 */
    uint32_t image_geometry[5], image_matrix[9];
    bool image_pending;
    uint64_t image_jobs, image_pixels;
    uint64_t packets, stream_words;
    /* paths: see "Drawing: paths" below */
    uint32_t path_direction, path_matrix[9], path_paint_matrix[2][6];
    uint32_t path_paint_mask, path_paint_valid, glyph_translation[2];
    bool path_controls[2], path_glyph, path_pending;
    bool stroke_cap_valid, stroke_join_valid;
    unsigned inline_path_phase, inline_path_end;
    unsigned path_kind;                /* 1 fill, 2 stroke: the E0 program */
    uint64_t path_program;             /* where that program is */
    Z63GfxEdge *path_edges;
    unsigned path_edge_count;
    uint64_t path_jobs, path_pixels;
    /* the clear program, 0x48 0xb10 .. 0x80b */
    unsigned clear_phase;
    uint32_t clear_geometry[5], clear_color;
    uint64_t clear_jobs;
    /* FIFO B */
    uint32_t b_header, b_reg, b_pairs, b_phase;
    bool b_trailer;
    uint32_t b_regs[4096];
    /* the ring */
    uint32_t ring_head;
    uint64_t descriptors;
    uint32_t reported;                 /* first use of each kind */
    /* -machine gfx-skip-unmodelled=on: a draw this model refuses at its
     * commit is dropped instead of stopping the machine -- no pixels are
     * written, the fences still complete -- and each distinct reason is
     * named once. A DECLARED CHOICE for runs that need to get past the GUI
     * to something else; the picture is then knowingly incomplete. */
    bool skip_unmodelled;
    GHashTable *skipped;               /* reason -> count */
} Z63Gfx;

static void z63_gfx_first(Z63Gfx *g, unsigned bit, const char *fmt, ...)
    G_GNUC_PRINTF(3, 4);
static void z63_gfx_first(Z63Gfx *g, unsigned bit, const char *fmt, ...)
{
    va_list ap;
    g_autofree char *msg = NULL;

    if (g->reported & (1u << bit)) {
        return;
    }
    g->reported |= 1u << bit;
    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    error_report("Z63_GFX: first %s. Synchronous; see nikon-z63-gfx.h.", msg);
}

/* Refuse: name what was asked for, then stop with the PC. */
static void z63_gfx_refuse(hwaddr off, uint32_t word, const char *why)
{
    error_report("Z63_GFX: %s (word 0x%08x) -- not modelled; see "
                 "nikon-z63-gfx.h", why, word);
    z63_stop_write("GFX", Z63_GFX_BASE + off, 4, word);
}

/* A bus address the engine reads, through its NoC window group. */
static bool z63_gfx_bus(Z63Gfx *g, uint32_t bus, uint64_t len, hwaddr *pa)
{
    return z63_noc_translate(g->noc, Z63_GFX_NOC_GROUP, false, bus, len, pa) &&
           *pa >= Z63_DRAM_BASE && *pa + len <= Z63_DRAM_BASE + Z63_DRAM_SIZE;
}

/*
 * Drawing: the image program (0x281)
 * ----------------------------------
 * Ported from ../z9-qemu's qemu/nikon-z9-lab.c (WonderLantern,
 * GPL-2.0-or-later: graphics_color_pixel, graphics_image_sample/_blend,
 * graphics_yuv_*, graphics_surface_*, graphics_render_yuva/_image), and
 * checked against this image's producer sub_4020E87C (also sub_4020B8F0,
 * sub_4022FDF0, sub_4023167C), which emits
 *
 *   0x46006001 0, 0x46022001 0, [0xa0014080 -- select 0x281],
 *   0x45020001 0, 0x80000005 {w, h, 1/w, 1/h, 1}
 *
 * and the draw is committed by 0x46 reg 0x4a = 1. One difference from the
 * Z 9: the fifth parameter word is 1 here where z9 measured 7; both are
 * accepted, and which one a draw carried is logged, because nothing yet
 * says what it selects.
 *
 * As in z9 this is a functional model, not the engine's instruction set:
 * an affine-mapped source image (RGBA8888 or A8, optionally filtered,
 * through the optional color chain) is written SRC or SRC_OVER into an
 * RGBA8888 surface, or into the packed YYUV + alpha / Y + UV surfaces the
 * menu uses. Every register the draw depends on is checked first, and a
 * value outside what z9 established stops the machine naming the check --
 * a different value means a different operation, not the same one drawn
 * slightly wrong.
 */
static float z63_gfx_float(uint32_t w)
{
    float f;

    memcpy(&f, &w, sizeof(f));
    return f;
}

static bool z63_gfx_table_range(uint32_t a, unsigned n, unsigned *bank,
                                unsigned *idx)
{
    *bank = (a >> 16) - 0x60;
    *idx = a & 0xffff;
    return *bank < 2 && *idx < 192 && n && n <= 192 - *idx;
}

static bool z63_gfx_color_range(uint32_t a, unsigned n, unsigned *bank,
                                unsigned *idx)
{
    unsigned family = (a >> 16) - 0x20, channel = (a >> 8) & 0xff;
    unsigned limit = family == 0 ? 256 : 32;

    *bank = family * 3 + channel;
    *idx = a & 0xff;
    return family < 2 && channel < 3 && n && *idx < limit &&
           n <= limit - *idx;
}

static bool z63_gfx_written(Z63Gfx *g, unsigned reg, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (!g->written48[reg + i]) {
            return false;
        }
    }
    return true;
}

/* The color chain is off (0x48 reg 0x200 = 0) or fully loaded (15). */
static bool z63_gfx_color_ready(Z63Gfx *g)
{
    uint32_t *c48 = g->bank[6];

    if (c48[0x200] == 0) {
        return true;
    }
    if (c48[0x200] != 15 || !z63_gfx_written(g, 0x201, 9) ||
        !z63_gfx_written(g, 0x20a, 3) || c48[0x20a] || c48[0x20b] ||
        c48[0x20c]) {
        return false;
    }
    for (unsigned i = 0; i < 9; i++) {
        if (c48[0x201 + i] > 0xfff) {
            return false;
        }
    }
    for (unsigned b = 0; b < 6; b++) {
        for (unsigned i = 0; i < (b < 3 ? 256 : 32); i++) {
            if (!g->color_written[b][i]) {
                return false;
            }
        }
    }
    return true;
}

static void z63_gfx_color_pixel(Z63Gfx *g, const uint8_t *in, uint8_t *out)
{
    /* z9: byte-indexed 12-bit input LUT, signed Q10 matrix, piecewise
     * output LUT on the ROM identity table's knots. Alpha preserved. */
    static const unsigned knots[33] = {
        0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
        256, 384, 512, 768, 1024, 1280, 1536, 1792, 2048, 2304,
        2560, 2816, 3072, 3328, 3584, 3840, 4096,
    };
    uint32_t *c48 = g->bank[6];

    memcpy(out, in, 4);
    if (!c48[0x200]) {
        return;
    }
    for (unsigned c = 0; c < 3; c++) {
        double v = 0;
        unsigned seg = 0;
        uint32_t entry;
        double value;

        for (unsigned k = 0; k < 3; k++) {
            v += sextract32(c48[0x201 + c * 3 + k], 0, 12) *
                 (double)g->color_tables[k][in[k]] / 1024.0;
        }
        v = MIN(4096.0, MAX(0.0, v));
        while (seg < 31 && v >= knots[seg + 1]) {
            seg++;
        }
        entry = g->color_tables[c + 3][seg];
        value = (entry & 0xfff) + sextract32(entry, 12, 12) *
                (v - knots[seg]) / (knots[seg + 1] - knots[seg]);
        out[c] = MIN(255, MAX(0, (int)floor(value / 16.0 + 0.5)));
    }
}

/* Target height from 0x43 slot 0x12c; image control bit 31 = rows down. */
static bool z63_gfx_surface_height(Z63Gfx *g, uint32_t dir, unsigned y0,
                                   unsigned y1, unsigned *height)
{
    float h = z63_gfx_float(g->bank[1][0x12c]);

    if ((dir != 1 && dir != 0x80000001) || !isfinite(h) || h < 1 ||
        h > 8192 || h != floorf(h) || y0 > y1 || y1 >= h) {
        return false;
    }
    *height = h;
    return true;
}

static unsigned z63_gfx_surface_row(uint32_t dir, unsigned height, unsigned y)
{
    return dir & 0x80000000 ? y : height - 1 - y;
}

typedef struct Z63GfxImage {
    const uint8_t *bytes;
    unsigned width, height, stride;
    bool a8, stencil, filter;
    uint8_t paint[4];
    double m[9], det;
} Z63GfxImage;

static bool z63_gfx_image_pos(const Z63GfxImage *in, unsigned x, unsigned y,
                              double *sx, double *sy)
{
    const double *m = in->m;
    double tx = x + 0.5 - m[2], ty = y + 0.5 - m[5];

    *sx = (m[4] * tx - m[1] * ty) / in->det;
    *sy = (m[0] * ty - m[3] * tx) / in->det;
    return *sx >= 0 && *sx < in->width && *sy >= 0 && *sy < in->height;
}

static void z63_gfx_image_sample(Z63Gfx *g, const Z63GfxImage *in,
                                 double sx, double sy, uint8_t *out)
{
    unsigned bpp = in->a8 ? 1 : 4;
    uint8_t rgba[4];

    if (!in->filter) {
        const uint8_t *p = in->bytes + (unsigned)sy * in->stride +
                           (unsigned)sx * bpp;

        if (in->a8) {
            memset(rgba, 255, 3);
            rgba[3] = p[0];
        } else {
            memcpy(rgba, p, 4);
        }
    } else {
        /* z9: bilinear at texel centres, edge clamp, premultiplied. */
        int ix = floor(sx - 0.5), iy = floor(sy - 0.5);
        double fx = sx - 0.5 - ix, fy = sy - 0.5 - iy;
        double alpha = 0, rgb[3] = { 0 };

        for (unsigned dy = 0; dy < 2; dy++) {
            for (unsigned dx = 0; dx < 2; dx++) {
                unsigned xx = CLAMP(ix + (int)dx, 0, (int)in->width - 1);
                unsigned yy = CLAMP(iy + (int)dy, 0, (int)in->height - 1);
                const uint8_t *p = in->bytes + yy * in->stride + xx * bpp;
                double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                double a = p[in->a8 ? 0 : 3] * w;

                alpha += a;
                if (!in->a8) {
                    for (unsigned c = 0; c < 3; c++) {
                        rgb[c] += p[c] * a;
                    }
                }
            }
        }
        rgba[3] = CLAMP((int)floor(alpha + 0.5), 0, 255);
        for (unsigned c = 0; c < 3; c++) {
            rgba[c] = in->a8 ? 255 : alpha ?
                      CLAMP((int)floor(rgb[c] / alpha + 0.5), 0, 255) : 0;
        }
    }
    if (in->stencil) {
        memcpy(out, in->paint, 4);
        out[3] = (rgba[3] * in->paint[3] + 127) / 255;
    } else {
        z63_gfx_color_pixel(g, rgba, out);
    }
}

static void z63_gfx_blend(const uint8_t *a, uint8_t *b, bool over)
{
    if (!over) {
        memcpy(b, a, 4);
    } else {
        unsigned alpha = a[3] * 255 + b[3] * (255 - a[3]);

        for (unsigned c = 0; c < 3; c++) {
            unsigned v = a[c] * a[3] * 255 + b[c] * b[3] * (255 - a[3]);

            b[c] = alpha ? (v + alpha / 2) / alpha : 0;
        }
        b[3] = (alpha + 127) / 255;
    }
}

static int z63_gfx_yuv_round(int v, unsigned shift)
{
    return (v >> shift) + ((v >> (shift - 1)) & 1);
}

/* z9: the CPU's integer form for control 0x4e4e0002 -- Y0, Y1, U, V. */
static void z63_gfx_yuv_encode(Z63Gfx *g, const uint8_t rgba[2][4],
                               uint8_t *out)
{
    for (unsigned p = 0; p < 2; p++) {
        for (unsigned row = 0; row < (p ? 1 : 3); row++) {
            int v = 0;

            for (unsigned c = 0; c < 3; c++) {
                v += (sextract32(g->bank[6][0x901 + row * 3 + c], 0, 11) *
                      rgba[p][c]) >> 6;
            }
            out[row ? row + 1 : p] = CLAMP(z63_gfx_yuv_round(v, 4) +
                                           (row ? 128 : 0), 0, 255);
        }
    }
}

static void z63_gfx_yuv_decode(Z63Gfx *g, const uint8_t *packed,
                               const uint8_t *alpha, uint8_t rgba[2][4])
{
    for (unsigned p = 0; p < 2; p++) {
        int ch[3] = { packed[p], packed[2] - 128, packed[3] - 128 };

        for (unsigned row = 0; row < 3; row++) {
            int v = 0;

            for (unsigned c = 0; c < 3; c++) {
                v += (sextract32(g->bank[6][0x911 + row * 3 + c], 0, 11) *
                      ch[c]) >> 6;
            }
            rgba[p][row] = CLAMP(z63_gfx_yuv_round(v, 2), 0, 255);
        }
        rgba[p][3] = alpha[p];
    }
}

static bool z63_gfx_overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{
    return a < b + m && b < a + n;
}

static bool z63_gfx_yuv_valid(Z63Gfx *g, unsigned reg)
{
    if (!z63_gfx_written(g, reg, 9)) {
        return false;
    }
    for (unsigned i = 0; i < 9; i++) {
        if (g->bank[6][reg + i] > 0x7ff) {
            return false;
        }
    }
    return true;
}

typedef struct Z63GfxSpan {
    unsigned row, begin, end;
} Z63GfxSpan;

/* Into packed YYUV + alpha (0x72) or Y + interleaved UV (0x71). */
static const char *z63_gfx_render_yuva(Z63Gfx *g, const Z63GfxImage *in,
                                       bool over, hwaddr src,
                                       uint64_t src_bytes, unsigned height,
                                       uint64_t *pixels)
{
    uint32_t *c47 = g->bank[5], *c48 = g->bank[6];
    bool planar = (c48[0x21] & 0xffff) == 0x71;
    unsigned cbpp = planar ? 1 : 2;
    unsigned pitch = c48[0x21] >> 16, stride = pitch * cbpp;
    unsigned x0 = c47[0xe0] >> 16, y0 = c47[0xe0] & 0xffff;
    unsigned x1 = c47[0xe1] >> 16, y1 = c47[0xe1] & 0xffff;
    uint64_t color_end = planar ? 2 : 4, alpha_end = 2;
    uint64_t color_begin = UINT64_MAX, alpha_begin = UINT64_MAX;
    hwaddr color, alpha;
    g_autofree Z63GfxSpan *spans = NULL;
    g_autofree uint8_t *colors = NULL, *alphas = NULL;

    if (pitch & 1) {
        return "an odd YUV pitch";
    }
    if (!z63_gfx_yuv_valid(g, 0x901) || (over && !z63_gfx_yuv_valid(g, 0x911))) {
        return "YUV matrices (0x48 0x901 / 0x911) not loaded";
    }
    if (c48[0x900] != 0x4e4e0002) {
        return "a YUV control (0x48 0x900) other than 0x4e4e0002";
    }
    if (c48[0x2a] != c48[0x2b]) {
        return "two different alpha planes (0x48 0x2a / 0x2b)";
    }
    if (over && (c48[0x912] || c48[0x919] || c48[0x911] != c48[0x914] ||
                 c48[0x911] != c48[0x917])) {
        return "an inverse YUV matrix of a shape z9 did not see";
    }
    spans = g_new0(Z63GfxSpan, y1 - y0 + 1);
    /* Coverage first: never write the nominal extent (z9 found the menu's
     * 1024x688 descriptor covers 682 physical rows). */
    for (unsigned y = y0; y <= y1; y++) {
        Z63GfxSpan *sp = &spans[y - y0];

        sp->row = z63_gfx_surface_row(g->image_control[0], height, y);
        sp->begin = x1 + 1;
        for (unsigned x = x0; x <= x1; x++) {
            double sx, sy;

            if (z63_gfx_image_pos(in, x, y, &sx, &sy)) {
                sp->begin = MIN(sp->begin, x);
                sp->end = x + 1;
            }
        }
        if (sp->end) {
            unsigned first = sp->begin & ~1u, last = (sp->end + 1) & ~1u;

            color_begin = MIN(color_begin,
                              (uint64_t)sp->row * stride + first * cbpp);
            color_end = MAX(color_end,
                            (uint64_t)sp->row * stride + last * cbpp);
            alpha_begin = MIN(alpha_begin, (uint64_t)sp->row * pitch + first);
            alpha_end = MAX(alpha_end, (uint64_t)sp->row * pitch + last);
        }
    }
    if (color_end > 64 * MiB || alpha_end > 32 * MiB ||
        !z63_gfx_bus(g, c48[0x20], color_end, &color) ||
        !z63_gfx_bus(g, c48[0x2a], alpha_end, &alpha)) {
        return "a YUV target the engine's NoC window does not reach";
    }
    if (color_begin != UINT64_MAX &&
        (z63_gfx_overlap(src, src_bytes, color + color_begin,
                         color_end - color_begin) ||
         z63_gfx_overlap(src, src_bytes, alpha + alpha_begin,
                         alpha_end - alpha_begin) ||
         z63_gfx_overlap(color + color_begin, color_end - color_begin,
                         alpha + alpha_begin, alpha_end - alpha_begin))) {
        return "overlapping source and target";
    }
    colors = g_malloc(color_end);
    alphas = g_malloc(alpha_end);
    cpu_physical_memory_read(color, colors, color_end);
    cpu_physical_memory_read(alpha, alphas, alpha_end);
    for (unsigned y = y0; y <= y1; y++) {
        Z63GfxSpan *sp = &spans[y - y0];

        for (unsigned x = sp->begin & ~1u; x < sp->end; x += 2) {
            uint8_t *pair = colors + sp->row * stride + x * cbpp;
            uint8_t *aa = alphas + sp->row * pitch + x;  /* UV in 0x71 */
            uint8_t rgba[2][4] = { { 0 } }, packed[4];

            if (over) {
                if (planar) {
                    uint8_t yyuv[4] = { pair[0], pair[1], aa[0], aa[1] };
                    const uint8_t opaque[2] = { 255, 255 };

                    z63_gfx_yuv_decode(g, yyuv, opaque, rgba);
                } else {
                    z63_gfx_yuv_decode(g, pair, aa, rgba);
                }
            }
            for (unsigned p = 0; p < 2; p++) {
                double sx, sy;
                uint8_t sample[4];

                if (x + p < sp->begin || x + p >= sp->end) {
                    continue;
                }
                if (!z63_gfx_image_pos(in, x + p, y, &sx, &sy)) {
                    return "affine coverage that is not one interval a row";
                }
                z63_gfx_image_sample(g, in, sx, sy, sample);
                z63_gfx_blend(sample, rgba[p], over);
                if (!planar) {
                    aa[p] = rgba[p][3];
                }
                (*pixels)++;
            }
            z63_gfx_yuv_encode(g, rgba, packed);
            /* Odd clips keep untouched luma/alpha; the even pixel owns U/V. */
            if (x >= sp->begin) {
                pair[0] = packed[0];
                if (planar) {
                    aa[0] = packed[2];
                    aa[1] = packed[3];
                } else {
                    pair[2] = packed[2];
                    pair[3] = packed[3];
                }
            }
            if (x + 1 < sp->end) {
                pair[1] = packed[1];
            }
        }
    }
    for (unsigned y = y0; y <= y1; y++) {
        Z63GfxSpan *sp = &spans[y - y0];

        if (sp->end) {
            unsigned first = sp->begin & ~1u, last = (sp->end + 1) & ~1u;
            uint64_t o = (uint64_t)sp->row * stride + first * cbpp;

            cpu_physical_memory_write(color + o, colors + o,
                                      (last - first) * cbpp);
            o = (uint64_t)sp->row * pitch + (planar ? first : sp->begin);
            cpu_physical_memory_write(alpha + o, alphas + o, planar ?
                                      last - first : sp->end - sp->begin);
        }
    }
    return NULL;
}

/*
 * 0x48 reg 0x640 = 1 with eight words at 0x641..0x648: an image state the
 * Z 9 never used (it only ever wrote 0x640 = 0). The GUI's image-state
 * builder sub_40243FF0 sends it, through sub_402226B0, when its flag
 * 0xA02E5CD84 or 0xA02E5CD8C is set: eight float32 words of its state at
 * +0x4ac..+0x4c8, each >> 13 -- float32 cut to 19 bits, 1.0 becoming
 * 0x1fc00 -- then 0x640 = 1. The first ones the GUI sends are four pairs
 * of (1.0, 0.0), which reads as a per-channel scale and offset: a colour
 * transform, and that one the identity. The identity changes no pixel, so
 * it is drawn as if 0x640 were 0; any other set of words still stops the
 * draw, naming them (z63_gfx_quad_report). That the pairs are scale/offset
 * is a reading of the values, not established; only the identity case
 * depends on it.
 */
static bool z63_gfx_identity_transform(const uint32_t *c48)
{
    for (unsigned i = 0; i < 8; i++) {
        if (c48[0x641 + i] != (i & 1 ? 0 : 0x1fc00)) {
            return false;
        }
    }
    return true;
}

static void z63_gfx_quad_report(Z63Gfx *g)
{
    uint32_t *c48 = g->bank[6];
    double m[9];

    for (unsigned i = 0; i < 9; i++) {
        m[i] = z63_gfx_float(g->image_matrix[i]);
    }
    error_report("Z63_GFX: image with 0x640 = %u: quad 0x641..0x648 = "
                 "%d,%d %d,%d %d,%d %d,%d (as words: %08x %08x %08x %08x "
                 "%08x %08x %08x %08x); 0x610 = 0x%x 0x611 = 0x%x "
                 "0x630 = 0x%x 0x650 = 0x%x 0x4b4 = 0x%x 0x21 = 0x%x "
                 "0x4f8 = 0x%x 0x620 = 0x%x; image %gx%g; matrix "
                 "[%g %g %g; %g %g %g; %g %g %g]; clip %u,%u..%u,%u",
                 c48[0x640], (int32_t)c48[0x641], (int32_t)c48[0x642],
                 (int32_t)c48[0x643], (int32_t)c48[0x644],
                 (int32_t)c48[0x645], (int32_t)c48[0x646],
                 (int32_t)c48[0x647], (int32_t)c48[0x648],
                 c48[0x641], c48[0x642], c48[0x643], c48[0x644],
                 c48[0x645], c48[0x646], c48[0x647], c48[0x648],
                 c48[0x610], c48[0x611], c48[0x630], c48[0x650],
                 c48[0x4b4], c48[0x21], c48[0x4f8], c48[0x620],
                 z63_gfx_float(g->image_geometry[0]),
                 z63_gfx_float(g->image_geometry[1]),
                 m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
                 g->bank[5][0xe0] >> 16, g->bank[5][0xe0] & 0xffff,
                 g->bank[5][0xe1] >> 16, g->bank[5][0xe1] & 0xffff);
}

/* Draw the pending image. NULL on success, else the check that failed. */
static const char *z63_gfx_render_image(Z63Gfx *g)
{
    static const uint32_t src_coeff[8] = { 0, 0x10000, 0, 0x10000, 0,
                                           0x10000, 0, 0 };
    static const uint32_t over_coeff[8] = { 0, 0x81100, 0, 0x81100, 0,
                                            0x81100, 1, 0x80100 };
    uint32_t *c46 = g->bank[4], *c47 = g->bank[5], *c48 = g->bank[6];
    bool over = !memcmp(&c48[0x780], over_coeff, sizeof(over_coeff));
    bool a8 = c48[0x4b4] == 0x21;
    bool planar = (c48[0x21] & 0xffff) == 0x71;
    bool yuva = planar || (c48[0x21] & 0xffff) == 0x72;
    bool stencil = a8 && c48[0x610] == 0x39 && c48[0x611] == 2 &&
                   c48[0x630] == 0 && c48[0x650] == 0;
    bool normal = c48[0x610] == 0 && c48[0x611] == 0 &&
                  c48[0x630] == (a8 ? 3 : 0) && c48[0x650] == (a8 ? 2 : 0);
    /* Zf C3.01 movie path: observed RGBA8888 sampler, not a generic bypass. */
    bool video_sampler = c48[0x610] == 0x39 && c48[0x611] == 2 &&
                         c48[0x630] == 3 && c48[0x650] == 0 &&
                         c48[0x4b4] == 3 && (c48[0x21] & 0xffff) == 3;
    unsigned sbpp = a8 ? 1 : 4;
    unsigned width = z63_gfx_float(g->image_geometry[0]);
    unsigned height = z63_gfx_float(g->image_geometry[1]);
    unsigned sx0 = c48[0x4c3] >> 16, sy0 = c48[0x4c3] & 0xffff;
    unsigned sstride = (c48[0x4b3] & 0xffff) * sbpp;
    unsigned tbpp = planar ? 1 : yuva ? 2 : 4;
    unsigned tstride = (c48[0x21] >> 16) * tbpp;
    unsigned x0 = c47[0xe0] >> 16, y0 = c47[0xe0] & 0xffff;
    unsigned x1 = c47[0xe1] >> 16, y1 = c47[0xe1] & 0xffff;
    uint64_t soff = (uint64_t)sy0 * sstride + sx0 * sbpp;
    uint64_t sbytes = soff + (uint64_t)(height - 1) * sstride + width * sbpp;
    unsigned sheight, last_row;
    uint64_t tbytes, pixels = 0;
    hwaddr src, dst = 0;
    double m[9], det;
    uint8_t paint[4] = { 255, 255, 255, 255 };
    g_autofree uint8_t *source = NULL;
    Z63GfxImage in;
    const char *why;

    if (c48[0x640] && !z63_gfx_identity_transform(c48)) {
        z63_gfx_quad_report(g);
    }
#define NEED(cond, what) do { if (!(cond)) { return what; } } while (0)
    NEED(z63_gfx_surface_height(g, g->image_control[0], y0, y1, &sheight),
         "the target height (0x43 slot 0x12c), image control or clip");
    last_row = MAX(z63_gfx_surface_row(g->image_control[0], sheight, y0),
                   z63_gfx_surface_row(g->image_control[0], sheight, y1));
    tbytes = (uint64_t)last_row * tstride + (x1 + 1) * tbpp;
    for (unsigned i = 0; i < 9; i++) {
        m[i] = z63_gfx_float(g->image_matrix[i]);
        NEED(isfinite(m[i]) && fabs(m[i]) <= 8192, "the image matrix");
    }
    det = m[0] * m[4] - m[1] * m[3];
    NEED(g->set_up, "no setup (program 0x123b) before the draw");
    NEED(z63_gfx_color_ready(g), "the color chain (0x48 0x200) half-loaded");
    NEED(c46[0x70] == 0 && c46[0x60] == 0x48, "0x46 0x70 / 0x60 not 0 / 0x48");
    NEED(g->image_control[1] == 0, "image control 1 (program 0x8ca) not 0");
    NEED(c47[0xc0] == 0x10004, "0x47 0xc0 not 0x10004");
    NEED(stencil || video_sampler || c47[0xc1] == 255,
         "0x47 0xc1 not 255 for a normal draw");
    NEED(c48[0x4b1] == 1, "0x48 0x4b1 not 1");
    NEED(stencil || normal || video_sampler,
         "a sampler mode (0x48 0x610/0x611/0x630/0x650)");
    NEED(c48[0x620] == 0, "0x48 0x620 not 0");
    NEED(c48[0x640] == 0 ||
         (c48[0x640] == 1 && z63_gfx_identity_transform(c48)),
         "a colour transform (0x48 0x640..0x648) other than the identity");
    NEED(m[6] == 0 && m[7] == 0 && m[8] == 1 && fabs(det) >= 1e-8,
         "a projective or singular image matrix");
    NEED(over || !memcmp(&c48[0x780], src_coeff, sizeof(src_coeff)),
         "a blend (0x48 0x780) other than SRC or SRC_OVER");
    NEED(a8 || c48[0x4b4] == 3, "a source format (0x48 0x4b4) other than "
         "RGBA8888 or A8");
    NEED(yuva || (c48[0x21] & 0xffff) == 3, "a target format (0x48 0x21)");
    NEED(c48[0x23] == c48[0x21] && c48[0x20] == c48[0x22],
         "two different target descriptors (0x48 0x20-0x23)");
    NEED(c48[0x4c0] == 0x101 && c48[0x4c2] == 1, "0x48 0x4c0 / 0x4c2");
    NEED(c48[0x350] == 0 && c48[0x4b8] == 0, "0x48 0x350 / 0x4b8 not 0");
    NEED(c48[0x4c4] == ((sx0 + width - 1) << 16 | (sy0 + height - 1)),
         "a source crop (0x48 0x4c4) that is not the image");
    NEED(c47[0x72] == width && c47[0x73] == height,
         "0x47 0x72 / 0x73 not the image size");
    NEED(c47[0xdf] == 1 && x0 <= x1 && y0 <= y1, "the clip (0x47 0xdf-0xe1)");
    NEED(sx0 + width <= 8192 && sy0 + height <= 8192 &&
         sstride >= (sx0 + width) * sbpp && (c48[0x4b3] >> 16) >= sy0 + height,
         "a source layout (0x48 0x4b3) smaller than the image");
    NEED(tstride >= (x1 + 1) * tbpp, "a target pitch narrower than the clip");
    NEED(c48[0x4b7] <= 1, "a filter mode (0x48 0x4b7) above 1");
    NEED(sbytes <= 64 * MiB && tbytes <= 64 * MiB, "a draw over 64 MiB");
    NEED(z63_gfx_bus(g, c48[0x4b2], sbytes, &src),
         "a source the engine's NoC window does not reach");
    NEED(yuva || (z63_gfx_bus(g, c48[0x20], tbytes, &dst) &&
                  !z63_gfx_overlap(src, sbytes, dst, tbytes)),
         "a target the NoC window does not reach, or one over the source");
    if (stencil) {
        uint32_t packed = c47[0xc1];
        uint8_t rgba[4] = { 0, 0, 0, packed & 255 };

        for (unsigned c = 0; c < 3; c++) {
            unsigned v = extract32(packed, 24 - 8 * c, 8);

            NEED(v <= rgba[3], "a stencil paint that is not premultiplied");
            rgba[c] = rgba[3] ? (v * 255 + rgba[3] / 2) / rgba[3] : 0;
        }
        z63_gfx_color_pixel(g, rgba, paint);
    }
#undef NEED
    source = g_malloc(sbytes);
    cpu_physical_memory_read(src, source, sbytes);
    in = (Z63GfxImage) {
        .bytes = source + soff, .width = width, .height = height,
        .stride = sstride, .a8 = a8, .stencil = stencil,
        .filter = c48[0x4b7] == 1, .det = det,
    };
    memcpy(in.m, m, sizeof(m));
    memcpy(in.paint, paint, sizeof(paint));
    if (yuva) {
        why = z63_gfx_render_yuva(g, &in, over, src, sbytes, sheight, &pixels);
        if (why) {
            return why;
        }
    } else {
        g_autofree uint8_t *target = g_malloc(tbytes);

        cpu_physical_memory_read(dst, target, tbytes);
        for (unsigned y = y0; y <= y1; y++) {
            for (unsigned x = x0; x <= x1; x++) {
                double sx, sy;
                uint8_t px[4];
                unsigned row;

                if (!z63_gfx_image_pos(&in, x, y, &sx, &sy)) {
                    continue;
                }
                z63_gfx_image_sample(g, &in, sx, sy, px);
                row = z63_gfx_surface_row(g->image_control[0], sheight, y);
                z63_gfx_blend(px, target + row * tstride + x * 4, over);
                pixels++;
            }
        }
        cpu_physical_memory_write(dst, target, tbytes);
    }
    g->image_pending = false;
    g->image_jobs++;
    g->image_pixels += pixels;
    if (g->image_jobs <= 8 || !(g->image_jobs & (g->image_jobs - 1))) {
        error_report("Z63_GFX: image %" PRIu64 ": %ux%u %s %s -> %s, %"
                     PRIu64 " pixels, parameter 5 = %u", g->image_jobs,
                     width, height, a8 ? "A8" : "RGBA8888",
                     over ? "SRC_OVER" : "SRC",
                     planar ? "Y+UV" : yuva ? "YYUV+A" : "RGBA8888", pixels,
                     g->image_geometry[4]);
    }
    return NULL;
}

/*
 * Drawing: paths, glyphs and clears
 * ---------------------------------
 * Ported from ../z9-qemu's qemu/nikon-z9-lab.c (WonderLantern,
 * GPL-2.0-or-later: graphics_path_edge/_line/_polygon/_disc/_arc/_bezier/
 * _stroke, graphics_path_queue, graphics_render_path,
 * graphics_zero_path_yuva, graphics_clear_packet and the selector/parameter
 * glue in graphics_commit). As with the image program it is z9's
 * functional reading of the engine, not its instruction set:
 *
 *   0x97f   path control: [0] = surface direction (1 / 0x80000001), the
 *           geometry matrix from 0x43 slots 0xac/0xb0/0xb4
 *   0x93b   paint: [0] bits 0/1 = fill/stroke, their 2x3 paint matrices
 *           from slots 0xd0/0xd4 and 0xd8/0xdc
 *   0x826   a glyph: five words {x, y, 0, 0, direction}, the glyph matrix
 *           from slots 0xc4/0xc8/0xcc; 0x7df then picks its paint
 *   0x5e5 / 0x5ec  run back to back: an empty inline path. This image
 *           never emits that pair; it uses 0xe73 / 0xe85, the pair z9 found
 *           on the ZR (its 4022b214). Here it is sub_4022A4F0: 0xa0073980
 *           and A8 at 0x4022a588, then the segment count [x26 + 0x14] < 1
 *           branches to 0x4022b4a0, which emits 0xa0074280 (0xe85) and A8.
 *           Both pairs are accepted; a non-empty one is still refused.
 *   ring E0000000  binds the fill or the stroke program; identified, as z9
 *           does, by the SHA-256 of its first 96 bytes -- never by content
 *   ring C0000000  a B3 list: OpenVG path segments and binary32
 *           coordinates, turned into edges through the matrix (arcs and
 *           Beziers flattened to 1/256 pixel, strokes from the radius and
 *           miter in slots 0x120/0x121 with the cap/join programs)
 *   0x46 reg 0x4a = 1  commits: regular sub-sample coverage (1, 2 or 4 per
 *           axis from 0x46 reg 0x60), nonzero or even-odd, into RGBA8888,
 *           or zero paint into the YUV planes
 *   0x48 0xb10, 0xb07, 0xb00 = 1, 0xb09 x3, 0x80a, 0x80b  a raw fill
 *
 * Every value z9 did not establish is still refused, naming the check.
 * What this image's producers send is re-checked as the firmware reaches
 * each piece; the selectors and register slots above are the ones this
 * firmware's own run used, or it would have stopped here.
 */
static bool z63_gfx_edge(Z63GfxEdge *edges, unsigned *count, const double *m,
                         double x0, double y0, double x1, double y1)
{
    Z63GfxEdge e = {
        m[0] * x0 + m[1] * y0 + m[2], m[3] * x0 + m[4] * y0 + m[5],
        m[0] * x1 + m[1] * y1 + m[2], m[3] * x1 + m[4] * y1 + m[5],
    };

    if (*count >= Z63_GFX_PATH_EDGES || !isfinite(e.x0) || !isfinite(e.y0) ||
        !isfinite(e.x1) || !isfinite(e.y1) || fabs(e.x0) > 32768 ||
        fabs(e.y0) > 32768 || fabs(e.x1) > 32768 || fabs(e.y1) > 32768) {
        return false;
    }
    edges[(*count)++] = e;
    return true;
}

static const double z63_gfx_identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

typedef struct Z63GfxContour {
    unsigned first, end;
    bool closed;
} Z63GfxContour;

/* Zero-length segments add nothing (no zero-length cap is claimed). */
static bool z63_gfx_line(Z63GfxEdge *edges, unsigned *count,
                         double x0, double y0, double x1, double y1)
{
    return (x0 == x1 && y0 == y1) ||
           z63_gfx_edge(edges, count, z63_gfx_identity, x0, y0, x1, y1);
}

/* Stroke pieces all wound the same way, so a nonzero fill unions them. */
static bool z63_gfx_polygon(Z63GfxEdge *edges, unsigned *count,
                            const double *m, const double v[][2], unsigned n)
{
    double area = 0;

    for (unsigned i = 0; i < n; i++) {
        unsigned j = (i + 1) % n;

        area += v[i][0] * v[j][1] - v[j][0] * v[i][1];
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned a = area >= 0 ? i : n - 1 - i;
        unsigned b = area >= 0 ? (i + 1) % n : (2 * n - 2 - i) % n;

        if (!z63_gfx_edge(edges, count, m, v[a][0], v[a][1], v[b][0], v[b][1])) {
            return false;
        }
    }
    return true;
}

static unsigned z63_gfx_arc_steps(double radius, double angle, double tol)
{
    double step = 2 * acos(CLAMP(1 - tol / MAX(radius, tol), -1, 1));
    double n = ceil(fabs(angle) / MAX(step, 1e-9));

    return n > 8192 ? 0 : MAX(1, (unsigned)n);
}

static bool z63_gfx_disc(Z63GfxEdge *edges, unsigned *count, const double *m,
                         double x, double y, double r, double tol)
{
    unsigned n = z63_gfx_arc_steps(r, 2 * M_PI, tol);
    double px = x + r, py = y;

    if (!n) {
        return false;
    }
    n = MAX(n, 8);
    for (unsigned i = 1; i <= n; i++) {
        double a = i * (2 * M_PI / n);
        double nx = i == n ? x + r : x + r * cos(a);
        double ny = i == n ? y : y + r * sin(a);

        if (!z63_gfx_edge(edges, count, m, px, py, nx, ny)) {
            return false;
        }
        px = nx;
        py = ny;
    }
    return true;
}

/* OpenVG endpoint arcs, centre conversion as in SVG's implementation
 * notes (radius correction, rotation). Not the engine's tessellator. */
static bool z63_gfx_arc(Z63GfxEdge *edges, unsigned *count, double tol,
                        unsigned op, double x, double y, const double *v)
{
    double rx = fabs(v[0]), ry = fabs(v[1]), phi = v[2] * M_PI / 180;
    double nx = v[3], ny = v[4], c = cos(phi), sn = sin(phi);
    bool large = op >= 22, ccw = op == 18 || op == 22;
    double dx, dy, xp, yp, lambda, denom, k, cxp, cyp, cx, cy;
    double ux, uy, vx, vy, start, delta, px = x, py = y;
    unsigned steps;

    if (nx == x && ny == y) {
        return true;
    }
    if (!rx || !ry) {
        return z63_gfx_line(edges, count, x, y, nx, ny);
    }
    dx = (x - nx) / 2;
    dy = (y - ny) / 2;
    xp = c * dx + sn * dy;
    yp = -sn * dx + c * dy;
    lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lambda > 1) {
        double scale = sqrt(lambda);

        rx *= scale;
        ry *= scale;
    }
    denom = rx * rx * yp * yp + ry * ry * xp * xp;
    if (!isfinite(denom) || denom <= 0) {
        return false;
    }
    k = sqrt(MAX(0, (rx * rx * ry * ry - denom) / denom));
    if (large == ccw) {
        k = -k;
    }
    cxp = k * rx * yp / ry;
    cyp = -k * ry * xp / rx;
    cx = c * cxp - sn * cyp + (x + nx) / 2;
    cy = sn * cxp + c * cyp + (y + ny) / 2;
    ux = (xp - cxp) / rx;
    uy = (yp - cyp) / ry;
    vx = (-xp - cxp) / rx;
    vy = (-yp - cyp) / ry;
    start = atan2(uy, ux);
    delta = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (ccw && delta < 0) {
        delta += 2 * M_PI;
    } else if (!ccw && delta > 0) {
        delta -= 2 * M_PI;
    }
    steps = z63_gfx_arc_steps(MAX(rx, ry), delta, tol);
    if (!steps) {
        return false;
    }
    for (unsigned i = 1; i <= steps; i++) {
        double a = start + delta * i / steps;
        double qx = i == steps ? nx : cx + c * rx * cos(a) - sn * ry * sin(a);
        double qy = i == steps ? ny : cy + sn * rx * cos(a) + c * ry * sin(a);

        if (!z63_gfx_line(edges, count, px, py, qx, qy)) {
            return false;
        }
        px = qx;
        py = qy;
    }
    return true;
}

static double z63_gfx_point_distance(double x, double y, const double a[2],
                                     const double b[2])
{
    double dx = b[0] - a[0], dy = b[1] - a[1], d = dx * dx + dy * dy;
    double t = d ? CLAMP(((x - a[0]) * dx + (y - a[1]) * dy) / d, 0, 1) : 0;

    return hypot(x - a[0] - t * dx, y - a[1] - t * dy);
}

static bool z63_gfx_bezier(Z63GfxEdge *edges, unsigned *count, double tol,
                           const double p[4][2], unsigned depth)
{
    double l[4][2], r[4][2];

    if (MAX(z63_gfx_point_distance(p[1][0], p[1][1], p[0], p[3]),
            z63_gfx_point_distance(p[2][0], p[2][1], p[0], p[3])) <= tol) {
        return z63_gfx_line(edges, count, p[0][0], p[0][1], p[3][0], p[3][1]);
    }
    if (depth == 16) {
        return false;
    }
    for (unsigned i = 0; i < 2; i++) {
        double mid = (p[1][i] + p[2][i]) / 2;

        l[0][i] = p[0][i];
        r[3][i] = p[3][i];
        l[1][i] = (p[0][i] + p[1][i]) / 2;
        r[2][i] = (p[2][i] + p[3][i]) / 2;
        l[2][i] = (l[1][i] + mid) / 2;
        r[1][i] = (r[2][i] + mid) / 2;
        l[3][i] = r[0][i] = (l[2][i] + r[1][i]) / 2;
    }
    return z63_gfx_bezier(edges, count, tol, l, depth + 1) &&
           z63_gfx_bezier(edges, count, tol, r, depth + 1);
}

/* Non-dashed strokes: radius in 0x43 slot 0x120, (width * miter)^2 in
 * 0x121 (z9's stroke-parameters capture). Dash state must be clear. */
static bool z63_gfx_stroke(Z63Gfx *g, Z63GfxEdge *edges, unsigned *count,
                           const double *m, double tol, const Z63GfxEdge *center,
                           const Z63GfxContour *contours, unsigned ncontours)
{
    uint32_t *vec = g->bank[1];
    double r = z63_gfx_float(vec[0x120]);
    double miter_squared = z63_gfx_float(vec[0x121]) / 4;

    if (!g->stroke_cap_valid || !g->stroke_join_valid || !isfinite(r) ||
        r <= 0 || r > 4096 || !isfinite(miter_squared) ||
        miter_squared < r * r || vec[0x100] != 0x3f800000 ||
        vec[0x101] != 0x3f800000 || vec[0xec] || vec[0xed] || vec[0xee] ||
        vec[0xe0]) {
        return false;
    }
    for (unsigned c = 0; c < ncontours; c++) {
        unsigned first = contours[c].first, end = contours[c].end;
        bool closed = contours[c].closed;

        for (unsigned i = first; i < end; i++) {
            const Z63GfxEdge *e = &center[i], *next;
            double length = hypot(e->x1 - e->x0, e->y1 - e->y0);
            double dx = (e->x1 - e->x0) / length, dy = (e->y1 - e->y0) / length;
            double nx = -dy * r, ny = dx * r;
            double begin = !closed && i == first && g->stroke_cap == 2 ? r : 0;
            double finish = !closed && i + 1 == end && g->stroke_cap == 2 ? r : 0;
            double x0 = e->x0 - dx * begin, y0 = e->y0 - dy * begin;
            double x1 = e->x1 + dx * finish, y1 = e->y1 + dy * finish;
            double quad[4][2] = { { x0 + nx, y0 + ny }, { x0 - nx, y0 - ny },
                                  { x1 - nx, y1 - ny }, { x1 + nx, y1 + ny } };
            double elen, ex, ey, turn;

            if (!z63_gfx_polygon(edges, count, m, quad, 4)) {
                return false;
            }
            if (!closed && g->stroke_cap == 1 &&
                ((i == first &&
                  !z63_gfx_disc(edges, count, m, e->x0, e->y0, r, tol)) ||
                 (i + 1 == end &&
                  !z63_gfx_disc(edges, count, m, e->x1, e->y1, r, tol)))) {
                return false;
            }
            if (i + 1 == end && !closed) {
                continue;
            }
            next = &center[i + 1 == end ? first : i + 1];
            elen = hypot(next->x1 - next->x0, next->y1 - next->y0);
            ex = (next->x1 - next->x0) / elen;
            ey = (next->y1 - next->y0) / elen;
            turn = dx * ey - dy * ex;
            if (g->stroke_join == 1) {
                if (!z63_gfx_disc(edges, count, m, e->x1, e->y1, r, tol)) {
                    return false;
                }
            } else if (fabs(turn) > 1e-12) {
                double side = turn > 0 ? -1 : 1;
                double qx = e->x1 + side * nx, qy = e->y1 + side * ny;
                double rx = e->x1 - side * ey * r, ry = e->y1 + side * ex * r;
                double t = ((rx - qx) * ey - (ry - qy) * ex) / turn;
                double mx = qx + t * dx, my = qy + t * dy;

                if (!g->stroke_join &&
                    (mx - e->x1) * (mx - e->x1) + (my - e->y1) * (my - e->y1) <=
                    miter_squared + 1e-10) {
                    double poly[4][2] = { { e->x1, e->y1 }, { qx, qy },
                                          { mx, my }, { rx, ry } };

                    if (!z63_gfx_polygon(edges, count, m, poly, 4)) {
                        return false;
                    }
                } else {
                    double tri[3][2] = { { e->x1, e->y1 }, { qx, qy },
                                         { rx, ry } };

                    if (!z63_gfx_polygon(edges, count, m, tri, 3)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

/* A C0000000 ring descriptor: queue the path its B3 list names. */
static const char *z63_gfx_path_queue(Z63Gfx *g, hwaddr command)
{
    uint8_t header[16];
    uint32_t first, n, bytes, sbus, cbus;
    hwaddr seg_phys, coords_phys;
    double m[9], x = 0, y = 0, start_x = 0, start_y = 0;
    double control_x = 0, control_y = 0, scale, tol;
    unsigned at = 0, count = 0, nc = 0, cn = 0, previous = 0;
    bool open = false;
    g_autofree uint8_t *segments = NULL, *coords = NULL;
    g_autofree Z63GfxEdge *center = NULL;
    g_autofree Z63GfxContour *contours = NULL;
    Z63GfxEdge *edges = g->path_edges;

    cpu_physical_memory_read(command, header, sizeof(header));
    first = ldl_le_p(header);
    n = first & 0xffffff;
    sbus = ldl_le_p(header + 4);
    cbus = ldl_le_p(header + 8);
    bytes = ldl_le_p(header + 12);
    if (g->path_pending || g->image_pending) {
        return "a path while another draw is queued";
    }
    if (!g->path_program || !g->path_controls[0]) {
        return "a path before its program (E0) or control (0x97f / 0x826)";
    }
    if (!((g->path_glyph ? g->path_paint_mask : g->path_paint_valid) &
          g->path_kind)) {
        return "a path whose fill/stroke paint (0x93b / 0x7df) is not loaded";
    }
    if (first >> 24 != 0xb3 || !n || n > 1024 || !bytes || bytes > 16384 ||
        (bytes & 3)) {
        return "a path list that is not a B3 list of z9's bounds";
    }
    if (!z63_gfx_bus(g, sbus, n, &seg_phys) ||
        !z63_gfx_bus(g, cbus, bytes, &coords_phys)) {
        return "a path list the engine's NoC window does not reach";
    }
    for (unsigned i = 0; i < 9; i++) {
        m[i] = z63_gfx_float(g->path_matrix[i]);
        if (!isfinite(m[i]) || fabs(m[i]) > 8192) {
            return "the path matrix";
        }
    }
    if (m[6] != 0 || m[7] != 0 || m[8] != 1) {
        return "a projective path matrix";
    }
    if (g->path_glyph) {
        /* The glyph origin, in glyph space before the matrix. */
        double gx = z63_gfx_float(g->glyph_translation[0]);
        double gy = z63_gfx_float(g->glyph_translation[1]);

        m[2] += m[0] * gx + m[1] * gy;
        m[5] += m[3] * gx + m[4] * gy;
    }
    segments = g_malloc(n);
    coords = g_malloc(bytes);
    center = g_new(Z63GfxEdge, Z63_GFX_PATH_EDGES);
    contours = g_new0(Z63GfxContour, n);
    cpu_physical_memory_read(seg_phys, segments, n);
    cpu_physical_memory_read(coords_phys, coords, bytes);
    scale = sqrt(m[0] * m[0] + m[1] * m[1] + m[3] * m[3] + m[4] * m[4]);
    tol = 1.0 / (256 * MAX(1, scale));
    for (unsigned i = 0; i < n; i++) {
        unsigned op = segments[i] & ~1u;
        bool relative = segments[i] & 1;
        unsigned need = (op == 2 || op == 4 || op == 14) ? 2 :
                        (op == 6 || op == 8) ? 1 :
                        (op == 10 || op == 16) ? 4 : op == 12 ? 6 :
                        (op >= 18 && op <= 24) ? 5 : 0;
        double v[6] = { 0 }, next_x, next_y;

        if ((op != 0 && !need) || need > (bytes - at) / 4) {
            return "a path segment z9 did not see, or one past the coordinates";
        }
        for (unsigned j = 0; j < need; j++) {
            v[j] = z63_gfx_float(ldl_le_p(coords + at + j * 4));
            if (!isfinite(v[j]) || fabs(v[j]) > 32768) {
                return "a path coordinate out of range";
            }
        }
        at += need * 4;
        if (op == 0) {                                  /* close */
            if (!open || !z63_gfx_line(center, &cn, x, y, start_x, start_y)) {
                return "a close with no open contour";
            }
            x = start_x;
            y = start_y;
            open = false;
            contours[nc - 1].closed = true;
            contours[nc - 1].end = cn;
            previous = op;
            continue;
        }
        if (op == 2) {                                  /* move */
            if (nc) {
                contours[nc - 1].end = cn;
            }
            x = v[0] + (relative ? x : 0);
            y = v[1] + (relative ? y : 0);
            start_x = x;
            start_y = y;
            contours[nc++].first = cn;
            open = true;
            previous = op;
            continue;
        }
        if (!open) {
            return "a path segment before any move";
        }
        if (op <= 8) {                                  /* line, h, v */
            next_x = op == 8 ? x : v[0] + (relative ? x : 0);
            next_y = op == 6 ? y : (op == 8 ? v[0] : v[1]) + (relative ? y : 0);
            if (!z63_gfx_line(center, &cn, x, y, next_x, next_y)) {
                return "a path with too many edges";
            }
        } else if (op >= 18) {                          /* arcs */
            v[3] += relative ? x : 0;
            v[4] += relative ? y : 0;
            if (!z63_gfx_arc(center, &cn, tol, op, x, y, v)) {
                return "an arc z9's flattening refuses";
            }
            next_x = v[3];
            next_y = v[4];
        } else {                                        /* curves */
            double p[4][2];

            for (unsigned j = 0; j < need; j += 2) {
                v[j] += relative ? x : 0;
                v[j + 1] += relative ? y : 0;
            }
            next_x = v[need - 2];
            next_y = v[need - 1];
            p[0][0] = x;
            p[0][1] = y;
            p[3][0] = next_x;
            p[3][1] = next_y;
            if (op == 10 || op == 14) {                 /* quadratic */
                bool smooth = previous == 10 || previous == 14;
                double qx = op == 10 ? v[0] : smooth ? 2 * x - control_x : x;
                double qy = op == 10 ? v[1] : smooth ? 2 * y - control_y : y;

                p[1][0] = (x + 2 * qx) / 3;
                p[1][1] = (y + 2 * qy) / 3;
                p[2][0] = (next_x + 2 * qx) / 3;
                p[2][1] = (next_y + 2 * qy) / 3;
                control_x = qx;
                control_y = qy;
            } else {                                    /* cubic */
                bool smooth = previous == 12 || previous == 16;

                p[1][0] = op == 12 ? v[0] : smooth ? 2 * x - control_x : x;
                p[1][1] = op == 12 ? v[1] : smooth ? 2 * y - control_y : y;
                p[2][0] = v[need - 4];
                p[2][1] = v[need - 3];
                control_x = p[2][0];
                control_y = p[2][1];
            }
            if (!z63_gfx_bezier(center, &cn, tol, p, 0)) {
                return "a curve z9's flattening refuses";
            }
        }
        x = next_x;
        y = next_y;
        previous = op;
    }
    if (at != bytes || !nc) {
        return "a path list whose coordinates do not match its segments";
    }
    contours[nc - 1].end = cn;
    if (g->path_kind == 2) {
        if (!z63_gfx_stroke(g, edges, &count, m, tol, center, contours, nc)) {
            return "a stroke outside z9's (no dash, cap/join set, radius)";
        }
    } else {
        for (unsigned c = 0; c < nc; c++) {
            unsigned begin = contours[c].first, end = contours[c].end;

            for (unsigned i = begin; i < end; i++) {
                if (!z63_gfx_edge(edges, &count, m, center[i].x0, center[i].y0,
                                  center[i].x1, center[i].y1)) {
                    return "a fill with too many edges";
                }
            }
            if (end > begin && !contours[c].closed &&
                !z63_gfx_edge(edges, &count, m, center[end - 1].x1,
                              center[end - 1].y1, center[begin].x0,
                              center[begin].y0)) {
                return "a fill with too many edges";
            }
        }
    }
    g->path_edge_count = count;
    g->path_pending = true;
    return NULL;
}

typedef struct Z63GfxCrossing {
    double x;
    int winding;
} Z63GfxCrossing;

typedef struct Z63GfxActive {
    double min_y, max_y;
    unsigned index;
} Z63GfxActive;

static int z63_gfx_start_cmp(const void *a, const void *b)
{
    const Z63GfxActive *x = a, *y = b;

    if (x->min_y != y->min_y) {
        return (x->min_y > y->min_y) - (x->min_y < y->min_y);
    }
    return (x->index > y->index) - (x->index < y->index);
}

static int z63_gfx_crossing_cmp(const void *a, const void *b)
{
    double x = ((const Z63GfxCrossing *)a)->x;
    double y = ((const Z63GfxCrossing *)b)->x;

    return (x > y) - (x < y);
}

/* Zero paint into the YUV planes: SRC attenuates alpha by coverage and
 * clears fully covered pixels to the measured zero (luma 0, chroma 128);
 * SRC_OVER of nothing leaves every byte alone. */
static const char *z63_gfx_zero_path_yuva(Z63Gfx *g, const uint8_t *mask,
                                          unsigned pitch, unsigned height,
                                          unsigned samples, bool over)
{
    uint32_t *c48 = g->bank[6];
    bool planar = (c48[0x21] & 0xffff) == 0x71;
    unsigned cbpp = planar ? 1 : 2;
    uint64_t cb = UINT64_MAX, ce = 0, ab = UINT64_MAX, ae = 0;
    hwaddr color, alpha;
    g_autofree uint8_t *colors = NULL, *alphas = NULL;

    if ((pitch & 1) || !z63_gfx_yuv_valid(g, 0x901) ||
        c48[0x900] != 0x4e4e0002 || c48[0x2a] != c48[0x2b]) {
        return "YUV planes for a zero-paint path (0x48 0x900/0x901/0x2a)";
    }
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < pitch; x += 2) {
            uint64_t at = (uint64_t)y * pitch + x;

            if (mask[at] || mask[at + 1]) {
                cb = MIN(cb, at * cbpp);
                ce = MAX(ce, at * cbpp + 2 * cbpp);
                ab = MIN(ab, at);
                ae = MAX(ae, at + 2);
            }
        }
    }
    if (ce > 64 * MiB || ae > 32 * MiB ||
        !z63_gfx_bus(g, c48[0x20], MAX(ce, 2 * cbpp), &color) ||
        !z63_gfx_bus(g, c48[0x2a], MAX(ae, 2), &alpha) ||
        (ce && z63_gfx_overlap(color + cb, ce - cb, alpha + ab, ae - ab))) {
        return "YUV planes the engine's NoC window does not reach";
    }
    if (!ce || over) {
        return NULL;
    }
    colors = g_malloc(ce);
    alphas = g_malloc(ae);
    cpu_physical_memory_read(color, colors, ce);
    cpu_physical_memory_read(alpha, alphas, ae);
    for (unsigned y = 0; y < height; y++) {
        unsigned first = pitch, last = 0;

        for (unsigned x = 0; x < pitch; x += 2) {
            uint64_t at = (uint64_t)y * pitch + x;

            if (!mask[at] && !mask[at + 1]) {
                continue;
            }
            first = MIN(first, x);
            last = x + 2;
            if (mask[at]) {
                if (!planar) {
                    alphas[at] = (alphas[at] * (samples - mask[at]) +
                                  samples / 2) / samples;
                }
                if (mask[at] == samples) {
                    if (planar) {
                        colors[at] = 0;
                        alphas[at] = alphas[at + 1] = 128;
                    } else {
                        colors[at * 2] = 0;
                        colors[at * 2 + 2] = 128;
                        colors[at * 2 + 3] = 128;
                    }
                }
            }
            if (mask[at + 1]) {
                if (!planar) {
                    alphas[at + 1] = (alphas[at + 1] * (samples - mask[at + 1]) +
                                      samples / 2) / samples;
                }
                if (mask[at + 1] == samples) {
                    colors[at * cbpp + 1] = 0;
                }
            }
        }
        if (last) {
            uint64_t off = (uint64_t)y * pitch + first;

            cpu_physical_memory_write(color + off * cbpp, colors + off * cbpp,
                                      (last - first) * cbpp);
            cpu_physical_memory_write(alpha + off, alphas + off, last - first);
        }
    }
    return NULL;
}

/* Draw the pending path. NULL on success, else the check that failed. */
static const char *z63_gfx_render_path(Z63Gfx *g)
{
    static const uint32_t src_coeff[8] = { 0, 0x10000, 0, 0x10000, 0,
                                           0x10000, 0, 0 };
    static const uint32_t over_coeff[8] = { 0, 0x81100, 0, 0x81100, 0,
                                            0x81100, 1, 0x80100 };
    uint32_t *c46 = g->bank[4], *c47 = g->bank[5], *c48 = g->bank[6];
    bool over = !memcmp(&c48[0x780], over_coeff, sizeof(over_coeff));
    unsigned x0 = c47[0xe0] >> 16, y0 = c47[0xe0] & 0xffff;
    unsigned x1 = c47[0xe1] >> 16, y1 = c47[0xe1] & 0xffff;
    unsigned pitch = c48[0x21] >> 16, fmt = c48[0x21] & 0xffff;
    bool zero_mode = c47[0xc0] == 0 && c47[0xc1] == 0;
    bool zero_yuva = c47[0xc1] == 0 && (fmt == 0x72 || fmt == 0x71);
    unsigned pixel_bpp = zero_yuva ? (fmt == 0x71 ? 1 : 2) : 4;
    unsigned stride = pitch * pixel_bpp, sheight, last_row;
    unsigned quality = c46[0x60], axis = 1u << ((quality >> 2) & 3);
    unsigned samples = axis * axis;
    uint32_t color = c47[0xc1];
    uint8_t rgba[4] = { 0, 0, 0, color & 255 }, conv[4];
    uint64_t bytes, window_offset = 0, window_bytes = 0, pixels = 0;
    hwaddr target = 0;
    double min_y, max_y;
    unsigned first_y, end_y, start_count = 0, next_start = 0, active_count = 0;
    g_autofree uint8_t *buffer = NULL, *coverage = NULL;
    g_autofree Z63GfxCrossing *crossings = NULL;
    g_autofree Z63GfxActive *starts = NULL, *active = NULL;
    const char *why;

#define NEED(cond, what) do { if (!(cond)) { return what; } } while (0)
    NEED(z63_gfx_surface_height(g, g->path_direction, y0, y1, &sheight),
         "the target height (0x43 slot 0x12c), path direction or clip");
    last_row = MAX(z63_gfx_surface_row(g->path_direction, sheight, y0),
                   z63_gfx_surface_row(g->path_direction, sheight, y1));
    bytes = (uint64_t)last_row * stride + (x1 + 1) * pixel_bpp;
    NEED(c46[0x70] == 1 || (c46[0x70] == 0x3c && g->path_jobs),
         "0x46 0x70 not 1 (or 0x3c after a path)");
    NEED(!(quality & ~0xeu) && (quality & 0xcu) != 0xcu,
         "a path quality (0x46 0x60) z9 did not see");
    NEED(c47[0xc0] == 0x10000 || zero_mode, "0x47 0xc0 not 0x10000");
    NEED(c48[0x610] == 8 && c48[0x611] == 3 && c48[0x4b1] == 0,
         "a path sampler (0x48 0x610/0x611/0x4b1) other than 8/3/0");
    NEED(z63_gfx_color_ready(g), "the color chain (0x48 0x200) half-loaded");
    NEED(c47[0xdf] == 1 && x0 <= x1 && y0 <= y1, "the clip (0x47 0xdf-0xe1)");
    NEED(fmt == 3 || zero_yuva, "a path target format (0x48 0x21) other "
         "than RGBA8888, or a painted YUV target");
    NEED(c48[0x23] == c48[0x21] && c48[0x20] == c48[0x22],
         "two different target descriptors (0x48 0x20-0x23)");
    NEED(pitch >= x1 + 1 && bytes <= 64 * MiB, "a target pitch or size");
    NEED(over || !memcmp(&c48[0x780], src_coeff, sizeof(src_coeff)),
         "a blend (0x48 0x780) other than SRC or SRC_OVER");
    NEED(zero_yuva || z63_gfx_bus(g, c48[0x20], bytes, &target),
         "a target the engine's NoC window does not reach");
    for (unsigned c = 0; c < 3; c++) {
        unsigned v = extract32(color, 24 - c * 8, 8);

        NEED(v <= rgba[3], "a path paint (0x47 0xc1) not premultiplied");
        rgba[c] = rgba[3] ? (v * 255 + rgba[3] / 2) / rgba[3] : 0;
    }
    z63_gfx_color_pixel(g, rgba, conv);
    NEED(!zero_yuva || (!conv[0] && !conv[1] && !conv[2] && !conv[3] &&
                        (uint64_t)pitch * sheight <= 32 * MiB),
         "a painted path into the YUV planes");
#undef NEED
    /* Most glyphs touch a few rows of a full-screen target. */
    min_y = y1 + 1;
    max_y = y0;
    for (unsigned i = 0; i < g->path_edge_count; i++) {
        min_y = MIN(min_y, MIN(g->path_edges[i].y0, g->path_edges[i].y1));
        max_y = MAX(max_y, MAX(g->path_edges[i].y0, g->path_edges[i].y1));
    }
    first_y = MAX((double)y0, MIN((double)y1 + 1, floor(min_y)));
    end_y = MAX((double)y0, MIN((double)y1 + 1, ceil(max_y)));
    if (!zero_yuva && first_y < end_y) {
        unsigned a = z63_gfx_surface_row(g->path_direction, sheight, first_y);
        unsigned b = z63_gfx_surface_row(g->path_direction, sheight, end_y - 1);

        window_offset = (uint64_t)MIN(a, b) * stride;
        window_bytes = (uint64_t)(MAX(a, b) - MIN(a, b)) * stride + (x1 + 1) * 4;
    }
    buffer = g_malloc0(zero_yuva ? (uint64_t)pitch * sheight
                                 : MAX(window_bytes, 1));
    coverage = g_malloc0(pitch);
    crossings = g_new(Z63GfxCrossing, MAX(g->path_edge_count, 1));
    starts = g_new(Z63GfxActive, MAX(g->path_edge_count, 1));
    active = g_new(Z63GfxActive, MAX(g->path_edge_count, 1));
    if (window_bytes) {
        cpu_physical_memory_read(target + window_offset, buffer, window_bytes);
    }
    for (unsigned i = 0; i < g->path_edge_count; i++) {
        const Z63GfxEdge *e = &g->path_edges[i];
        double lo = MIN(e->y0, e->y1), hi = MAX(e->y0, e->y1);

        if (lo < hi && lo < end_y && hi > first_y) {
            starts[start_count++] = (Z63GfxActive) { lo, hi, i };
        }
    }
    qsort(starts, start_count, sizeof(*starts), z63_gfx_start_cmp);
    for (unsigned y = first_y; y < end_y; y++) {
        unsigned row;

        memset(coverage + x0, 0, x1 - x0 + 1);
        for (unsigned sy = 0; sy < axis; sy++) {
            double cy = y + (sy + 0.5) / axis;
            unsigned count = 0, kept = 0;
            int winding = 0;

            for (unsigned i = 0; i < active_count; i++) {
                if (cy < active[i].max_y) {
                    active[kept++] = active[i];
                }
            }
            active_count = kept;
            while (next_start < start_count && starts[next_start].min_y <= cy) {
                Z63GfxActive edge = starts[next_start++];
                unsigned lo = 0, hi = active_count;

                if (cy >= edge.max_y) {
                    continue;
                }
                while (lo < hi) {           /* keep descriptor order */
                    unsigned mid = lo + (hi - lo) / 2;

                    if (active[mid].index < edge.index) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                memmove(active + lo + 1, active + lo,
                        (active_count - lo) * sizeof(*active));
                active[lo] = edge;
                active_count++;
            }
            for (unsigned i = 0; i < active_count; i++) {
                const Z63GfxEdge *e = &g->path_edges[active[i].index];

                crossings[count++] = (Z63GfxCrossing) {
                    e->x0 + (cy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0),
                    e->y1 > e->y0 ? 1 : -1,
                };
            }
            qsort(crossings, count, sizeof(*crossings), z63_gfx_crossing_cmp);
            for (unsigned i = 0; i + 1 < count; i++) {
                unsigned begin, end;

                winding += crossings[i].winding;
                if (g->path_kind != 2 && (quality & 2) ? !(winding & 1)
                                                       : !winding) {
                    continue;
                }
                begin = MAX((double)x0 * axis,
                            MIN((double)(x1 + 1) * axis,
                                ceil(crossings[i].x * axis - 0.5)));
                end = MAX((double)x0 * axis,
                          MIN((double)(x1 + 1) * axis,
                              ceil(crossings[i + 1].x * axis - 0.5)));
                for (unsigned sx = begin; sx < end; sx++) {
                    coverage[sx / axis]++;
                }
            }
        }
        row = z63_gfx_surface_row(g->path_direction, sheight, y);
        for (unsigned x = x0; x <= x1; x++) {
            unsigned k = coverage[x], denom, weight, keep, alpha;
            uint8_t *b;

            if (!k) {
                continue;
            }
            pixels++;
            if (zero_yuva) {
                buffer[(uint64_t)row * pitch + x] = k;
                continue;
            }
            /* Coverage applied to the blended premultiplied result. */
            b = buffer + (uint64_t)row * stride + x * 4 - window_offset;
            denom = samples * (over ? 255 : 1);
            weight = k * (over ? 255 : 1);
            keep = over ? denom - conv[3] * k : samples - k;
            alpha = conv[3] * weight + b[3] * keep;
            for (unsigned c = 0; c < 3; c++) {
                uint64_t v = (uint64_t)conv[c] * conv[3] * weight +
                             (uint64_t)b[c] * b[3] * keep;

                b[c] = alpha ? (v + alpha / 2) / alpha : 0;
            }
            b[3] = (alpha + denom / 2) / denom;
        }
    }
    if (zero_yuva) {
        why = z63_gfx_zero_path_yuva(g, buffer, pitch, sheight, samples, over);
        if (why) {
            return why;
        }
    } else if (window_bytes) {
        cpu_physical_memory_write(target + window_offset, buffer, window_bytes);
    }
    g->path_pending = false;
    g->path_jobs++;
    g->path_pixels += pixels;
    if (g->path_jobs <= 8 || !(g->path_jobs & (g->path_jobs - 1))) {
        error_report("Z63_GFX: path %" PRIu64 ": %s, %u edges, %" PRIu64
                     " pixels, paint 0x%08x, %u samples a pixel%s",
                     g->path_jobs, g->path_kind == 2 ? "stroke" : "fill",
                     g->path_edge_count, pixels, color, samples,
                     g->path_glyph ? ", glyph" : "");
    }
    return NULL;
}

/* One packet of the raw-fill sequence. NULL if accepted. */
static const char *z63_gfx_clear_packet(Z63Gfx *g, unsigned reg)
{
    uint32_t *p = g->payload;

    if (reg == 0xb10 && g->need == 5) {
        unsigned width = (p[3] & 0xffff) + 1, height = (p[3] >> 16) + 1;
        unsigned stride = width + (p[4] >> 16);
        unsigned bpp = p[0] == 0x010000e2 ? 1 : p[0] == 0x010000e1 ? 2 : 4;
        uint64_t bytes = ((uint64_t)(height - 1) * stride + width) * bpp;
        hwaddr pa;

        if (g->clear_phase || g->path_pending || g->image_pending) {
            return "a clear while another draw is open";
        }
        if ((p[0] != 0x010000e0 && p[0] != 0x010000e1 && p[0] != 0x010000e2) ||
            p[1] || (p[2] & (bpp - 1)) || (p[4] & 0xffff) || width > 8192 ||
            height > 8192 || stride > 8192 || bytes > 64 * MiB ||
            !z63_gfx_bus(g, p[2], bytes, &pa)) {
            error_report("Z63_GFX: the clear's geometry words: %08x %08x "
                         "%08x %08x %08x", p[0], p[1], p[2], p[3], p[4]);
            return "clear geometry (0x48 0xb10) outside what z9 saw";
        }
        memcpy(g->clear_geometry, p, sizeof(g->clear_geometry));
        g->clear_phase = 1;
    } else if (reg == 0xb07 && g->need == 2) {
        if (g->clear_phase != 1 || p[1] ||
            (g->clear_geometry[0] == 0x010000e2 && p[0] > 0xff)) {
            return "a clear color (0x48 0xb07) out of sequence or with a mask";
        }
        g->clear_color = p[0];
        g->clear_phase = 2;
    } else if (reg == 0xb00 && g->need == 1 && p[0] == 1) {
        uint32_t *cg = g->clear_geometry;
        unsigned width = (cg[3] & 0xffff) + 1, height = (cg[3] >> 16) + 1;
        unsigned bpp = cg[0] == 0x010000e2 ? 1 : cg[0] == 0x010000e1 ? 2 : 4;
        unsigned stride = (width + (cg[4] >> 16)) * bpp;
        uint64_t bytes = (uint64_t)(height - 1) * stride + width * bpp;
        g_autofree uint8_t *row = NULL;
        hwaddr target;

        if (g->clear_phase != 2) {
            return "a clear start (0x48 0xb00) before geometry and color";
        }
        if (!z63_gfx_bus(g, cg[2], bytes, &target)) {
            return "a clear target the engine's NoC window does not reach";
        }
        row = g_malloc(width * bpp);
        for (unsigned x = 0; x < width; x++) {
            if (bpp == 4) {
                stl_le_p(row + x * 4, g->clear_color);
            } else if (bpp == 2) {
                stw_le_p(row + x * 2, g->clear_color);
            } else {
                row[x] = g->clear_color;
            }
        }
        for (unsigned y = 0; y < height; y++) {
            cpu_physical_memory_write(target + (uint64_t)y * stride, row,
                                      width * bpp);
        }
        g->clear_jobs++;
        g->clear_phase = 3;
        if (g->clear_jobs <= 8 || !(g->clear_jobs & (g->clear_jobs - 1))) {
            error_report("Z63_GFX: clear %" PRIu64 ": %ux%u, %u bytes a "
                         "pixel, color 0x%08x", g->clear_jobs, width, height,
                         bpp, g->clear_color);
        }
    } else if (reg == 0xb09 && g->need == 1) {
        static const uint32_t sequence[3] = { 0x01030200, 0x01010000,
                                              0x01010100 };

        if (g->clear_phase < 3 || g->clear_phase > 5 ||
            p[0] != sequence[g->clear_phase - 3]) {
            return "a clear finish (0x48 0xb09) other than z9's sequence";
        }
        g->clear_phase++;
    } else if (reg == 0x80a && g->need == 1 && p[0] == 0x80000000 &&
               g->clear_phase == 6) {
        g->clear_phase = 7;
    } else if (reg == 0x80b && g->need == 1 && p[0] == 0 &&
               g->clear_phase == 7) {
        g->clear_phase = 0;
    } else {
        return "a clear packet out of z9's sequence";
    }
    return NULL;
}

/* One finished FIFO A packet. False refuses it. */
static bool z63_gfx_commit(Z63Gfx *g, hwaddr off)
{
    unsigned op = g->header >> 24;
    unsigned reg = (g->header >> 12) & 0xfff;
    const char *why = NULL;

    if (op == 0x43 && (g->header & 0x3000)) {
        z63_gfx_refuse(off, g->header, "a 0x43 packet starting mid-slot");
        return false;
    }
    if (g->clear_phase && !(op == 0x48 && (reg == 0xb10 || reg == 0xb07 ||
                                           reg == 0xb00 || reg == 0xb09 ||
                                           reg == 0x80a || reg == 0x80b))) {
        z63_gfx_refuse(off, g->header, "a packet inside a clear sequence");
        return false;
    }
    if (g->inline_path_phase &&
        g->header != (0xa0000000 | (g->inline_path_end << 7)) &&
        !(g->header == 0xa8000000 && g->selector == g->inline_path_end)) {
        z63_gfx_refuse(off, g->header, "a non-empty inline path (z9 only "
                       "saw 0x5e5 / 0x5ec, or 0xe73 / 0xe85, back to back)");
        return false;
    }
    g->packets++;
    if ((op & 0xf0) == 0xa0) {
        static const uint16_t config[] = { 0x123b, 0x8d9, 0x8ca, 0x7cd, 0x7d0,
                                           0x7ca, 0x935, 0x937, 0x939,
                                           0x281, 0x97f, 0x93b, 0x826,
                                           0x7df, 0x5e5, 0x5ec, 0xe73,
                                           0xe85 };
        uint32_t sel = (g->header >> 7) & 0x7fff;
        g_autofree char *msg = NULL;

        if ((g->header & 0xf7c0007f) != 0xa0000000) {
            why = "a malformed instruction word";
        } else if (g->image_pending || g->path_pending) {
            why = "an instruction while a draw is queued and not committed";
        } else if (g->header == 0xa8000000) {                /* run */
            switch (g->selector) {
            case 0x123b:
                g->set_up = true;
                g->image_control[0] = g->image_control[1] = UINT32_MAX;
                g->path_controls[0] = g->path_controls[1] = false;
                g->path_paint_mask = g->path_paint_valid = 0;
                g->path_glyph = false;
                g->path_direction = 0;
                break;
            case 0x7cd: g->stroke_cap = 0; g->stroke_cap_valid = true; break;
            case 0x7d0: g->stroke_cap = 1; g->stroke_cap_valid = true; break;
            case 0x7ca: g->stroke_cap = 2; g->stroke_cap_valid = true; break;
            case 0x935: g->stroke_join = 0; g->stroke_join_valid = true; break;
            case 0x937: g->stroke_join = 1; g->stroke_join_valid = true; break;
            case 0x939: g->stroke_join = 2; g->stroke_join_valid = true; break;
            case 0x5e5:                         /* an empty inline path */
            case 0xe73:
                /* z9 saw this pair only under the fill program. This image
                 * also runs it right after binding the stroke program
                 * (the first time 0x7df glyph paint is followed by z9's
                 * stroke program); an empty path draws nothing either way,
                 * so it asks for a program, a control and the paint bit of
                 * that program's kind -- from either paint source, because
                 * the glyph flag (and its 0x7df mask) is still set from the
                 * glyph batch before: measured, mask 0x1 with paint valid
                 * 0x3 and the stroke program bound. */
                if (g->inline_path_phase || !g->path_controls[0] ||
                    !g->path_program ||
                    !((g->path_paint_mask | g->path_paint_valid) &
                      g->path_kind)) {
                    msg = g_strdup_printf("an inline path without a "
                                          "program, control and its paint "
                                          "(phase %u, control %d, program "
                                          "%#" PRIx64 " kind %u, glyph %d, "
                                          "paint mask %#x valid %#x)",
                                          g->inline_path_phase,
                                          g->path_controls[0],
                                          (uint64_t)g->path_program,
                                          g->path_kind, g->path_glyph,
                                          g->path_paint_mask,
                                          g->path_paint_valid);
                    why = msg;
                    break;
                }
                g->path_edge_count = 0;
                g->inline_path_phase = 1;
                g->inline_path_end = g->selector == 0xe73 ? 0xe85 : 0x5ec;
                break;
            case 0x5ec:
            case 0xe85:
                if (g->inline_path_phase != 1 ||
                    g->selector != g->inline_path_end) {
                    why = "an inline path end without its start";
                    break;
                }
                g->inline_path_phase = 0;
                g->inline_path_end = 0;
                g->path_pending = true;     /* valid empty work */
                break;
            default:
                msg = g_strdup_printf("running program 0x%x (a draw, by "
                                      "../z9-qemu's reading)", g->selector);
                why = msg;
                break;
            }
            if (!why) {
                z63_gfx_first(g, 14, "run of program 0x%x (configuration)",
                              g->selector);
                return true;
            }
        } else if (!(g->header & 0x08000000)) {               /* select */
            for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
                if (sel == config[i] && (sel == 0x123b || g->set_up)) {
                    g->selector = sel;
                    z63_gfx_first(g, 15, "program select 0x%x", sel);
                    return true;
                }
            }
            msg = g_strdup_printf("selecting program 0x%x (a draw, or not "
                                  "yet set up, by ../z9-qemu's reading)", sel);
            why = msg;
        } else {
            why = "an instruction word with bit 27 and a selector";
        }
        z63_gfx_refuse(off, g->header, why);
        return false;
    }
    if (op == 0x80) {                                  /* program parameters */
        if (g->selector == 0x8d9 && g->need == 1 &&
            (g->payload[0] == 1 || g->payload[0] == 0x80000001)) {
            g->image_control[0] = g->payload[0];
            return true;
        }
        if (g->selector == 0x8ca && g->need == 1 && g->payload[0] == 0) {
            g->image_control[1] = g->payload[0];
            return true;
        }
        if (g->selector == 0x97f && g->need == 1 && !g->path_pending &&
            (g->payload[0] == 1 || g->payload[0] == 0x80000001)) {
            g->path_glyph = false;
            g->path_direction = g->payload[0];
            for (unsigned row = 0; row < 3; row++) {
                memcpy(&g->path_matrix[row * 3], &g->bank[1][0xac + row * 4], 12);
            }
            g->path_controls[0] = true;
            z63_gfx_first(g, 24, "path control (program 0x97f)");
            return true;
        }
        if ((g->selector == 0x93b || (g->selector == 0x7df && g->path_glyph)) &&
            g->need == 1 && !g->path_pending && g->payload[0] &&
            !(g->payload[0] & ~3u)) {
            g->path_paint_valid |= g->payload[0];
            for (unsigned mode = 0; mode < 2; mode++) {
                if (g->payload[0] & (1u << mode)) {
                    for (unsigned row = 0; row < 2; row++) {
                        memcpy(&g->path_paint_matrix[mode][row * 3],
                               &g->bank[1][0xd0 + mode * 8 + row * 4], 12);
                    }
                }
            }
            if (g->selector == 0x7df) {
                g->path_paint_mask = g->payload[0];
            } else {
                g->path_paint_mask |= g->payload[0];
            }
            g->path_controls[1] = true;
            z63_gfx_first(g, 25, "path paint (program 0x%x)", g->selector);
            return true;
        }
        if (g->selector == 0x826 && g->need == 5 && !g->path_pending &&
            !g->payload[2] && !g->payload[3] &&
            (g->payload[4] == 1 || g->payload[4] == 0x80000001)) {
            uint32_t matrix[9];
            bool ok = true;

            for (unsigned i = 0; i < 2; i++) {
                float v = z63_gfx_float(g->payload[i]);

                ok &= isfinite(v) && fabsf(v) <= 32768;
            }
            for (unsigned row = 0; row < 3; row++) {
                memcpy(&matrix[row * 3], &g->bank[1][0xc4 + row * 4], 12);
            }
            for (unsigned i = 0; i < 9; i++) {
                float v = z63_gfx_float(matrix[i]);

                ok &= isfinite(v) && fabsf(v) <= 8192;
            }
            ok &= z63_gfx_float(matrix[6]) == 0 && z63_gfx_float(matrix[7]) == 0 &&
                  z63_gfx_float(matrix[8]) == 1;
            if (ok) {
                if (!g->path_glyph) {   /* no paint inherited from a path */
                    g->path_paint_mask = 0;
                    g->path_controls[1] = false;
                }
                memcpy(g->path_matrix, matrix, sizeof(matrix));
                memcpy(g->glyph_translation, g->payload,
                       sizeof(g->glyph_translation));
                g->path_glyph = true;
                g->path_direction = g->payload[4];
                g->path_controls[0] = true;
                z63_gfx_first(g, 26, "glyph (program 0x826)");
                return true;
            }
        }
        if (g->selector == 0x281 && g->need == 5 && !g->image_pending &&
            (g->payload[4] == 1 || g->payload[4] == 7)) {
            bool ok = true;

            for (unsigned i = 0; i < 2; i++) {
                float n = z63_gfx_float(g->payload[i]);
                float inv = z63_gfx_float(g->payload[i + 2]);

                ok &= isfinite(n) && n >= 1 && n <= 8192 && n == floorf(n) &&
                      inv == 1.0f / n;
            }
            if (ok) {
                memcpy(g->image_geometry, g->payload, sizeof(g->image_geometry));
                for (unsigned row = 0; row < 3; row++) {
                    memcpy(&g->image_matrix[row * 3],
                           &g->bank[1][0xb8 + row * 4], 12);
                }
                g->image_pending = true;
                z63_gfx_first(g, 23, "image geometry (program 0x281)");
                return true;
            }
        }
        error_report("Z63_GFX: parameters (%u words, first 0x%08x) for "
                     "program 0x%x", g->need, g->payload[0], g->selector);
        z63_gfx_refuse(off, g->header, "program parameters this model does "
                       "not execute (a draw, by ../z9-qemu's reading)");
        return false;
    }
    switch (op) {
    case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48:
        if (reg + g->need > 4096) {
            why = "a register run past the 4096-register bank";
            break;
        }
        if (op == 0x48 && (reg == 0xb10 || reg == 0xb07 || reg == 0xb00 ||
                           reg == 0xb09 || reg == 0x80a || reg == 0x80b)) {
            why = z63_gfx_clear_packet(g, reg);
            if (why) {
                break;
            }
            memcpy(&g->bank[6][reg], g->payload, g->need * 4);
            memset(&g->written48[reg], true, g->need);
            z63_gfx_first(g, 27, "clear packet (0x48 reg 0x%x)", reg);
            return true;
        }
        if (op == 0x48) {
            for (unsigned i = 0; i < g->need; i++) {
                if (reg + i == 0xb00 && g->payload[i]) {
                    why = "a clear start (0x48 reg 0xb00) inside a longer "
                          "packet";
                }
            }
            if (why) {
                break;
            }
            if (reg == 0 && g->need == 1 && g->payload[0] == 1 &&
                (g->image_pending || g->path_pending)) {
                why = "a fence while a draw is queued and not committed";
                break;
            }
            memset(&g->written48[reg], true, g->need);
            if (reg <= 0x30 && reg + g->need > 0x30) {
                g->table_address = g->payload[0x30 - reg];
            }
        }
        memcpy(&g->bank[op - 0x42][reg], g->payload, g->need * 4);
        if (op == 0x46 && reg <= 0x4a && reg + g->need > 0x4a &&
            g->payload[0x4a - reg] == 1 && (g->image_pending ||
                                            g->path_pending)) {
            const char *bad = g->image_pending && g->path_pending
                              ? "an image and a path queued together"
                              : g->image_pending ? z63_gfx_render_image(g)
                                                 : z63_gfx_render_path(g);

            if (bad && g->skip_unmodelled) {
                uintptr_t n;

                if (!g->skipped) {
                    g->skipped = g_hash_table_new(g_str_hash, g_str_equal);
                }
                n = GPOINTER_TO_UINT(g_hash_table_lookup(g->skipped, bad)) + 1;
                g_hash_table_insert(g->skipped, (gpointer)bad,
                                    GUINT_TO_POINTER(n));
                if (n == 1 || !(n & (n - 1))) {
                    error_report("Z63_GFX: skipped a draw this model cannot "
                                 "do (%" PRIuPTR "x): %s -- no pixels "
                                 "written (gfx-skip-unmodelled=on)", n, bad);
                }
                g->image_pending = g->path_pending = false;
            } else if (bad) {
                g_autofree char *msg = g_strdup_printf("a draw this model "
                                                       "cannot do: %s", bad);
                z63_gfx_refuse(off, g->header, msg);
                return false;
            }
        }
        z63_gfx_first(g, op - 0x42 + 16, "packet 0x%02x (reg 0x%x, %u "
                      "words)", op, reg, g->need);
        if (g->need == 1 && g->payload[0] == 1) {
            if (op == 0x48 && reg == 0) {
                g->pending |= Z63_GFX_EV_A48;
                z63_gfx_first(g, 8, "fence 0x48 reg 0 -> event bit 24");
            } else if (op == 0x45 && reg == 28) {
                g->pending |= Z63_GFX_EV_A45;
                z63_gfx_first(g, 9, "fence 0x45 reg 28 -> event bit 25");
            } else if (op == 0x46 && reg == 113) {
                g->pending |= Z63_GFX_EV_A46;
                z63_gfx_first(g, 10, "fence 0x46 reg 113 -> event bit 3");
            }
        }
        return true;
    case 0x49: {
        unsigned bank, idx;

        if (reg != 0) {
            why = "a stream with a register field (0x491000 is read-back "
                  "on the Z 9)";
            break;
        }
        if (z63_gfx_table_range(g->table_address, g->need, &bank, &idx)) {
            memcpy(&g->tables[bank][idx], g->payload, g->need * 4);
        } else if (z63_gfx_color_range(g->table_address, g->need, &bank,
                                       &idx)) {
            for (unsigned i = 0; i < g->need; i++) {
                uint32_t v = g->payload[i];
                int end = (v & 0xfff) + sextract32(v, 12, 12);

                if ((bank < 3 && v > 0xfff) ||
                    (bank >= 3 && (v > 0xffffff || end < 0 || end > 4096))) {
                    why = "a color table entry out of range";
                }
            }
            if (why) {
                break;
            }
            memcpy(&g->color_tables[bank][idx], g->payload, g->need * 4);
            memset(&g->color_written[bank][idx], true, g->need);
        } else {
            why = "a stream to a table address (0x48 reg 0x30) this model "
                  "does not know";
            break;
        }
        g->table_address += g->need;
        g->stream_words += g->need;
        z63_gfx_first(g, 6, "stream 0x49 (%u words to 0x%x)", g->need,
                      g->table_address - g->need);
        return true;
    }
    case 0x4a:
        why = "a 0x4a stream, whose destination nothing has established";
        break;
    default:
        why = "an unknown FIFO A opcode";
        break;
    }
    z63_gfx_refuse(off, g->header, why);
    return false;
}

static bool z63_gfx_push_a(Z63Gfx *g, uint32_t word, hwaddr off)
{
    if (g->header == 0) {
        unsigned op = word >> 24;

        if (word == Z63_GFX_TRAILER) {
            return true;            /* a trailer after a zero-length packet */
        }
        if (op >= 0x51 && op <= 0x58) {
            z63_gfx_refuse(off, word, "a FIFO A read-back");
            return false;
        }
        g->header = word;
        /* An instruction word is a whole packet; its low bits are not a
         * length (0xa0091d80 would otherwise swallow 0xd80 words). */
        g->need = (op & 0xf0) == 0xa0 ? 0 : word & 0xfff;
        g->got = 0;
        g->trailer = g->mode & 1;
        if (g->need == 0) {
            bool ok = z63_gfx_commit(g, off);

            g->header = 0;
            return ok;
        }
        return true;
    }
    if (g->got < g->need) {
        g->payload[g->got++] = word;
        if (g->got < g->need) {
            return true;
        }
        if (g->trailer) {
            return true;            /* the trailer comes next */
        }
    } else if (word != Z63_GFX_TRAILER) {
        z63_gfx_refuse(off, word, "a missing 0xDECAFACE trailer");
        return false;
    }
    {
        bool ok = z63_gfx_commit(g, off);

        g->header = 0;
        return ok;
    }
}

static bool z63_gfx_push_b(Z63Gfx *g, uint32_t word, hwaddr off)
{
    switch (g->b_phase) {
    case 0: {                                   /* a header */
        unsigned op = (word >> 12) & 0xf;

        if (word == Z63_GFX_TRAILER) {
            return true;
        }
        if (op == 0xd && (word & 0xffff0fff) == 0) {
            g->pending |= Z63_GFX_EV_B;
            z63_gfx_first(g, 11, "FIFO B fence (0xd000) -> event bit 28");
            return true;
        }
        if (op == 0xb && (word & 0x0ffc) == 0) {
            g->b_reg = word >> 16;
            g->b_pairs = (word & 3) + 1;
            g->b_phase = 1;
            z63_gfx_first(g, 12, "FIFO B write (reg 0x%x, %u pairs)",
                          g->b_reg, g->b_pairs);
            return true;
        }
        z63_gfx_refuse(off + 0x60, word, op == 0xc ? "a FIFO B read-back"
                                                   : "an unknown FIFO B word");
        return false;
    }
    case 1:                                     /* a value */
        g->b_regs[g->b_reg & 0xfff] = word;
        if (--g->b_pairs) {
            g->b_phase = 2;
        } else {
            g->b_phase = 0;
        }
        return true;
    default:                                    /* the next register */
        if (word & 0xffff) {
            z63_gfx_refuse(off + 0x60, word, "a malformed FIFO B pair");
            return false;
        }
        g->b_reg = word >> 16;
        g->b_phase = 1;
        return true;
    }
}

/*
 * E0000000 / C0000000 descriptors: a path program, or a path. The program
 * is identified, as ../z9-qemu identifies the Z 9's two, by the SHA-256 of
 * its first 96 bytes -- which says which program it is without this file
 * holding or interpreting a byte of it. An unknown program is refused and
 * its fingerprint logged, so it can be identified from the code that
 * publishes it.
 */
static const char *z63_gfx_path_descriptor(Z63Gfx *g, const uint32_t desc[2])
{
    static const struct {
        const char *sha256;
        unsigned kind;          /* 1 fill, 2 stroke */
        const char *where;
    } programs[] = {
        /* z9's two, measured on the Z 9 C 5.32 */
        { "c7a2d85834969d4ed236b979c78d07f4478814ea15614a1f965bd71962b9d394",
          1, "z9 fill" },
        { "f7b11e4c7f46b46b927a8ee15476061eb325c12b69ee03c31a2e28da5ee1b9d8",
          2, "z9 stroke" },
    };
    bool program = desc[0] == 0xe0000000;
    hwaddr command;

    if (g->header || g->path_pending || g->image_pending || g->clear_phase ||
        g->inline_path_phase) {
        return "a path descriptor while other work is open";
    }
    if ((desc[1] >> 30) != 1 ||
        !z63_gfx_bus(g, desc[1] << 2, program ? 96 : 16, &command)) {
        return "a path descriptor the engine's NoC window does not reach";
    }
    if (program) {
        uint8_t raw[96];
        g_autofree char *fp = NULL;

        cpu_physical_memory_read(command, raw, sizeof(raw));
        fp = g_compute_checksum_for_data(G_CHECKSUM_SHA256, raw, sizeof(raw));
        for (unsigned i = 0; i < ARRAY_SIZE(programs); i++) {
            if (!strcmp(fp, programs[i].sha256)) {
                if (g->path_program != command || g->path_kind != programs[i].kind) {
                    z63_gfx_first(g, 28 + programs[i].kind - 1,
                                  "path program (%s, sha256 %.16s...)",
                                  programs[i].where, fp);
                }
                g->path_kind = programs[i].kind;
                g->path_program = command;
                return NULL;
            }
        }
        error_report("Z63_GFX: path program at 0x%" HWADDR_PRIx " has sha256 "
                     "%s, first words %08x %08x %08x %08x", command, fp,
                     ldl_le_p(raw), ldl_le_p(raw + 4), ldl_le_p(raw + 8),
                     ldl_le_p(raw + 12));
        return "a path program (E0000000) whose fingerprint is not known";
    }
    return z63_gfx_path_queue(g, command);
}

/* Doorbell: run the next descriptor's command list through FIFO A. */
static bool z63_gfx_ring(Z63Gfx *g, uint32_t count, hwaddr off)
{
    hwaddr ring, list;
    uint32_t desc[2];

    if (count != 1 || g->ring_size != Z63_GFX_RING_SLOTS - 1 || !g->ring_on ||
        (g->ring_base & 7) ||
        !z63_gfx_bus(g, g->ring_base, Z63_GFX_RING_SLOTS * 8, &ring)) {
        z63_gfx_refuse(off, count, "a ring submission whose ring this model "
                       "cannot reach (base, size, enable or NoC window)");
        return false;
    }
    cpu_physical_memory_read(ring + g->ring_head * 8, desc, sizeof(desc));
    desc[0] = le32_to_cpu(desc[0]);
    desc[1] = le32_to_cpu(desc[1]);
    if (desc[0] == 0xe0000000 || desc[0] == 0xc0000000) {
        const char *why = z63_gfx_path_descriptor(g, desc);

        if (why) {
            error_report("Z63_GFX: ring slot %u holds {0x%08x, 0x%08x}",
                         g->ring_head, desc[0], desc[1]);
            z63_gfx_refuse(off, count, why);
            return false;
        }
        g->ring_head = (g->ring_head + 1) % Z63_GFX_RING_SLOTS;
        g->descriptors++;
        g->pending |= Z63_GFX_EV_RING;
        return true;
    }
    if (desc[0] >= Z63_GFX_MAX_LIST || (desc[1] >> 30) != 1 ||
        !z63_gfx_bus(g, desc[1] << 2, ((uint64_t)desc[0] + 1) * 4, &list)) {
        error_report("Z63_GFX: ring slot %u holds {0x%08x, 0x%08x}",
                     g->ring_head, desc[0], desc[1]);
        z63_gfx_refuse(off, count, desc[0] == 0xe0000000 ||
                       desc[0] == 0xc0000000 ? "a path-program descriptor"
                       : "a descriptor this model cannot follow");
        return false;
    }
    z63_gfx_first(g, 13, "ring descriptor (%u words at bus 0x%08x)",
                  desc[0] + 1, desc[1] << 2);
    for (uint32_t i = 0; i <= desc[0]; i++) {
        uint32_t w;

        cpu_physical_memory_read(list + i * 4, &w, 4);
        if (!z63_gfx_push_a(g, le32_to_cpu(w), 0)) {
            return false;
        }
    }
    g->ring_head = (g->ring_head + 1) % Z63_GFX_RING_SLOTS;
    g->descriptors++;
    g->pending |= Z63_GFX_EV_RING;
    return true;
}

static uint64_t z63_gfx_read(void *opaque, hwaddr off, unsigned size)
{
    Z63Gfx *g = opaque;

    if (size != 4 || (off & 3)) {
        z63_stop_read("GFX", Z63_GFX_BASE + off, size);
        return 0;
    }
    switch (off) {
    case 0x00: return Z63_GFX_FIFO_A;
    case 0x08: return 0;
    case 0x0c: return g->enable;
    case 0x14: return g->pending;
    case 0x18: return g->mode;
    case 0x20: return 0;
    case 0x34: return 0;
    case 0x60: return Z63_GFX_FIFO_B;
    case 0x70: return g->ring_base;
    case 0x74: return g->ring_size;
    case 0x78: return 0;
    case 0x80: return g->ring_on;
    default:
        z63_stop_read("GFX", Z63_GFX_BASE + off, size);
        return 0;
    }
}

static void z63_gfx_write(void *opaque, hwaddr off, uint64_t value,
                          unsigned size)
{
    Z63Gfx *g = opaque;
    static const hwaddr zero_regs[6] = { 0x58, 0x5c, 0x7c, 0x84, 0xe4,
                                         0x164 };

    if (size != 4 || (off & 3)) {
        z63_stop_write("GFX", Z63_GFX_BASE + off, size, value);
        return;
    }
    switch (off) {
    case 0x00: z63_gfx_push_a(g, value, off); return;
    case 0x0c: g->enable = value; return;
    case 0x10: g->pending &= ~(uint32_t)value; return;
    case 0x18:
        if (value & ~7u) {
            break;
        }
        g->mode = value;
        return;
    case 0x60: z63_gfx_push_b(g, value, off); return;
    case 0x70: g->ring_base = value; g->ring_head = 0; return;
    case 0x74: g->ring_size = value; return;
    case 0x78: z63_gfx_ring(g, value, off); return;
    case 0x80: g->ring_on = value; return;
    default:
        for (unsigned i = 0; i < ARRAY_SIZE(zero_regs); i++) {
            if (off == zero_regs[i] && value == 0) {
                g->idle_regs[i] = value;
                return;
            }
        }
        break;
    }
    z63_stop_write("GFX", Z63_GFX_BASE + off, size, value);
}

/* LDR/STR W only; `valid` admits wider so a different width stops with
 * its PC instead of becoming a bus error (see nikon-z63-xdmac.h). */
static const MemoryRegionOps z63_gfx_ops = {
    .read = z63_gfx_read,
    .write = z63_gfx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static void z63_gfx_init(Object *owner, MemoryRegion *sysmem, Z63Gfx *g,
                         Z63Noc *noc)
{
    g->noc = noc;
    g->path_edges = g_new(Z63GfxEdge, Z63_GFX_PATH_EDGES);
    memory_region_init_io(&g->mr, owner, &z63_gfx_ops, g, "GFX",
                          Z63_GFX_SIZE);
    memory_region_add_subregion(sysmem, Z63_GFX_BASE, &g->mr);
}

#endif /* HW_ARM_NIKON_Z63_GFX_H */
