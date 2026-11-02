/*
 * Nikon Z 6III -- the YUV compressor at 0x15648000 and the codec
 * decompressor at 0x15660000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * One driver, 0x40c71ac4-0x40c75178, serves both. Its init sub_40C71AC4
 * registers devices 0x1f2..0x1f6 (descriptors 0xa0280f1c8.., no base in
 * them) and one handler, sub_40C71B74, on INTIDs 0x75 and 0x77 -- YUV_COMP
 * and CODEC_DECOMP in this image's interrupt-name table. The bases are
 * materialised in the code and in three per-device tables of register
 * addresses at 0x4185b620 (+0x00), 0x4185b648 (+0x70) and 0x4185b688
 * (+0x20): devices 0x1f2/0x1f3 are the 0x15648000 block, 0x1f4..0x1f6 the
 * 0x15660000 one. Reached when the live view restarts in movie mode, after
 * TENC and TDEC: sub_40C74D70 sets bit 0 of +0x00 (pc 0x40c74d94).
 *
 * The register set, from `eg2010_0200i1.bi` (md5
 * b024ec7148e24ec0493e27d4d0f20b0e) read through `z63_eg.elf` (md5
 * 0fedcc0523da1dac8127a8010ccf978b). Both blocks are driven by the same
 * code with the base chosen per device -- sub_40C71E68 switches on the
 * device id and builds a different base in each case before a shared
 * body -- so one layout is applied to both:
 *
 *   +0x00  enable (bit 0, RMW)            +0x10  written
 *   +0x20  status, read by the handler    +0x28  cause, read with it
 *   +0x50  state word                     +0x58  36-bit address, read as
 *   +0x64  read                                  one 64-bit load (+0x5c)
 *   +0x70  start (bit 0 set, RMW)
 *   +0x1000 + 0x400 * k, k = 0..3: +0x00 +0x04 +0x08 +0x0c +0x20, and two
 *   sub-units at +0x200 and +0x300, each +0x00 +0x18 +0x1c +0x20 +0x28 +0x2c
 *   +0x30 +0x40 +0x44 +0x48 +0x4c +0x64 +0x68 +0x7c and eight 36-bit
 *   addresses at +0x80..+0xbf
 *
 * Extracted by following the MOV/MOVK-built bases, and pointers ADDed to
 * them, to every load and store through them; the groups were seen at
 * k = 0..3 in the 0x15660000 block and k = 2, 3 in the 0x15648000 one,
 * whose k = 0, 1 copies the same code reaches through the other switch
 * cases (0x1564900c.. for device 0x1f2, 0x1564980c.. for 0x1f3).
 *
 * The +0x58 load (0x40c74ef4) and the address registers are 64-bit
 * accesses, so these windows take them as two 32-bit halves, each checked
 * against the set.
 *
 * What is a declared choice: storage throughout. Status, cause and state
 * read their reset value, 0 -- idle, nothing pending; nothing here
 * compresses, decompresses or interrupts.
 *
 * An offset not in this set still stops the machine with its PC.
 */

#ifndef HW_ARM_NIKON_Z63_YUVCOMP_H
#define HW_ARM_NIKON_Z63_YUVCOMP_H

#define Z63_YUVCOMP_BASE     0x15648000ULL
#define Z63_CODECDEC_BASE    0x15660000ULL
#define Z63_YUVCOMP_SIZE     0x2000

static const uint16_t z63_yuvcomp_top[] = {
    0x000, 0x010, 0x020, 0x028, 0x050, 0x058, 0x05c, 0x064, 0x070,
};
static const uint16_t z63_yuvcomp_group[] = {
    0x000, 0x004, 0x008, 0x00c, 0x020,
};
static const uint16_t z63_yuvcomp_sub[] = {
    0x00, 0x18, 0x1c, 0x20, 0x28, 0x2c, 0x30, 0x40, 0x44, 0x48, 0x4c,
    0x64, 0x68, 0x7c,
    /* eight 36-bit addresses, each read-modify-written as one 64-bit
     * access (sub_40C738AC: 0x40c73954.., through ADD Xn, X8, #0x274..) */
    0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c,
    0xa0, 0xa4, 0xa8, 0xac, 0xb0, 0xb4, 0xb8, 0xbc,
};

/* The full offset list for one block, built from the three pieces. */
static size_t z63_yuvcomp_offsets(uint16_t *out, size_t max)
{
    size_t n = 0;

    for (size_t i = 0; i < ARRAY_SIZE(z63_yuvcomp_top) && n < max; i++) {
        out[n++] = z63_yuvcomp_top[i];
    }
    for (unsigned k = 0; k < 4; k++) {
        unsigned g = 0x1000 + 0x400 * k;

        for (size_t i = 0; i < ARRAY_SIZE(z63_yuvcomp_group) && n < max;
             i++) {
            out[n++] = g + z63_yuvcomp_group[i];
        }
        for (unsigned u = 0x200; u <= 0x300; u += 0x100) {
            for (size_t i = 0; i < ARRAY_SIZE(z63_yuvcomp_sub) && n < max;
                 i++) {
                out[n++] = g + u + z63_yuvcomp_sub[i];
            }
        }
    }
    return n;
}

#define Z63_YUVCOMP_NOFFS (ARRAY_SIZE(z63_yuvcomp_top) + 4 * \
                           (ARRAY_SIZE(z63_yuvcomp_group) + \
                            2 * ARRAY_SIZE(z63_yuvcomp_sub)))

#endif /* HW_ARM_NIKON_Z63_YUVCOMP_H */
