/*
 * Nikon Z 6III -- the CODEC register block at 0x16020000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The SoC's own region table puts 0x16000000-0x16800000 under the name
 * CODEC (29 entries at `.data` 0xa02bd2380; z63-tools/region_table.py prints
 * it), so that is what this band is, on the firmware's authority.
 *
 * One generic accessor pair, offsets at the call sites
 * ----------------------------------------------------
 * This driver does not have a family of per-register accessors. It has two
 * functions and passes the offset in:
 *
 *     sub_40C5B2E8(offset)          -> read
 *     sub_40C5B300(offset, value)   -> write
 *         BL   sub_40C5B258           ; the base, from the runtime pointer
 *         STR  W19, [X0,X20]          ; at 0xa02e6ef70
 *
 * so no offset appears in the accessor at all and there are 99 call sites to
 * the write alone. z63-tools/handle_offsets.py --call-arg reads them: it
 * records the constant in the named argument register at every call to the
 * named callee.
 *
 * Tail calls count. sub_40C5B368 writes two words and the machine stopped on
 * both of them:
 *
 *     MOV  W1, #0x3234 ; MOVK W1,#0x544d,LSL#16   ; 0x544d3234
 *     MOV  W0, #0x7f8
 *     BL   sub_40C5B300                           ; pc 0x40c5b314, +0x7f8
 *     MOV  W1, #0x2020 ; MOVK W1,#0x3633,LSL#16   ; 0x36332020
 *     MOV  W0, #0x7fc
 *     B    sub_40C5B300                           ; a tail call, +0x7fc
 *
 * Matching only BL found +0x7f8 and missed +0x7fc, which is now fixed in the
 * tool. The two values are ASCII: little-endian they lay down the bytes
 * "42MT" then "  36", so the eight bytes at +0x7f8 read `42MT  36`. Read as
 * two big-endian words they are "TM24" and "63  ". No meaning is claimed
 * for either reading here; what is established is that the firmware writes
 * those exact eight bytes and never reads them back.
 *
 * The register set
 * ----------------
 * 74 offsets, highest 0x0ff4, from 13 functions in
 * 0x40c5b000-0x40c5d000.
 *
 * This mode finds the offsets the firmware names with a constant, not every
 * offset it can reach: a call whose argument the dataflow cannot pin down
 * contributes nothing. So the set is a lower bound, and the next stop in
 * this window is expected to be a computed offset rather than a missing
 * constant. That is the opposite failure mode from the --array-base and
 * --const-window blocks, which over-approximate.
 *
 * Storage, with the usual contract: an offset not in the set still stops the
 * machine with its PC. None of these offsets is read-only in the firmware's
 * use of them, so there is no status-register declared choice here.
 */

#ifndef HW_ARM_NIKON_Z63_CODEC_H
#define HW_ARM_NIKON_Z63_CODEC_H

#define Z63_CODEC_BASE  0x16020000ULL
#define Z63_CODEC_SIZE  0x01000

static const uint16_t z63_codec_offsets[] = {
    0x0004, 0x0008, 0x0080, 0x0084, 0x0100, 0x0110, 0x0120, 0x0128,
    0x0200, 0x0204, 0x0300, 0x0310, 0x0318, 0x0370, 0x0374, 0x0378,
    0x0390, 0x0394, 0x03f0, 0x0400, 0x0404, 0x0408, 0x040c, 0x0410,
    0x0414, 0x0440, 0x0444, 0x0454, 0x0464, 0x0704, 0x0740, 0x0744,
    0x0748, 0x074c, 0x0780, 0x07a0, 0x07f8, 0x07fc, 0x0900, 0x0904,
    0x0908, 0x090c, 0x0910, 0x0914, 0x0920, 0x0c10, 0x0c14, 0x0c18,
    0x0c1c, 0x0c40, 0x0e40, 0x0e60, 0x0e64, 0x0eb0, 0x0eb4, 0x0eb8,
    0x0f00, 0x0f04, 0x0f08, 0x0f0c, 0x0f10, 0x0f20, 0x0f44, 0x0f50,
    0x0f54, 0x0f60, 0x0f64, 0x0f68, 0x0f6c, 0x0fe0, 0x0fe4, 0x0fe8,
    0x0ff0, 0x0ff4,
};

#endif /* HW_ARM_NIKON_Z63_CODEC_H */
