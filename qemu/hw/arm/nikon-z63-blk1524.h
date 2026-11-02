/*
 * Nikon Z 6III -- the block at 0x15240000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reached through the runtime pointer slot at 0xa02dc3d88. sub_40C13940
 * brings it up and writes 2 to +0x44 and 0 to +0x48:
 *
 *     MOV  X8, #0x3d88 ; MOVK X8,#0x2dc,LSL#16 ; MOVK X8,#0xa,LSL#32
 *     LDR  X8, [X8]               ; the device base
 *     MOV  W9, #2
 *     STR  W9, [X8,#0x44]         ; 0x40c13970 -- the machine stopped here
 *     STR  WZR, [X8,#0x48]        ; 0x40c13974 -- and here
 *
 * Same driver as the AUX pair
 * ---------------------------
 * 123 of the AUX pair's offsets are in this set too, out of its 128
 * (nikon-z63-aux.h, slot 0xa02dc41a8). The slots sit in the same band of
 * BSS -- 0xa02dc3d88, 0xa02dc3dd8, 0xa02dc41a8, 0xa02dc47d8, 0xa02dc47f0 are
 * all pointer slots the same code loads bases from -- so one driver serves
 * several blocks in quite different address bands.
 *
 * That is also why the windows are kept separate instead of merged. The
 * union set reaches 0x1f1c, but the AUX instances are 0x1000 apart, so
 * registers past +0x1000 cannot belong to them. Each block therefore gets
 * its own size, taken from its own harvest, and nothing claims the driver's
 * whole register set exists at every base.
 *
 * The register set
 * ----------------
 * 143 offsets, highest 0x1f1c, from 51 functions in
 * 0x40c13000-0x40c1b000, by z63-tools/handle_offsets.py --array-base
 * 0xa02dc3d88: a forward dataflow over the basic-block graph.
 *
 * Because a pointer array's slots hold the bases of several device types, a
 * set harvested this way is the union over all of them and is wider than
 * this block's own register file. That costs nothing -- nothing else maps
 * this window -- but it can absorb an access worth seeing. The window size
 * 0x2000 comes from the highest offset in the set and is not independently
 * established.
 *
 * 2 offsets are only ever read, never written. For those, storage is a
 * DECLARED CHOICE: the reset value is returned and nothing here knows what
 * hardware would report. An offset not in the set still stops the machine
 * with its PC.
 */

#ifndef HW_ARM_NIKON_Z63_BLK1524_H
#define HW_ARM_NIKON_Z63_BLK1524_H

#define Z63_BLK1524_BASE  0x15240000ULL
#define Z63_BLK1524_SIZE  0x02000

static const uint16_t z63_blk1524_offsets[] = {
    0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x0020,
    0x002c, 0x0044, 0x0048, 0x004c, 0x0050, 0x0054, 0x005c, 0x0060,
    0x0064, 0x0068, 0x00a0, 0x00a4, 0x00a8, 0x00ac, 0x00b0, 0x00b4,
    0x00b8, 0x00bc, 0x00c0, 0x00c4, 0x00c8, 0x00cc, 0x0100, 0x0104,
    0x0108, 0x010c, 0x0110, 0x0118, 0x011c, 0x0120, 0x0124, 0x0128,
    0x012c, 0x0130, 0x0134, 0x0138, 0x013c, 0x0140, 0x0144, 0x0148,
    0x014c, 0x0150, 0x0154, 0x0170, 0x0174, 0x0178, 0x017c, 0x0180,
    0x0184, 0x0188, 0x018c, 0x0190, 0x0194, 0x0198, 0x019c, 0x01a0,
    0x01a4, 0x01a8, 0x01b4, 0x01b8, 0x0200, 0x0204, 0x0208, 0x020c,
    0x0210, 0x0214, 0x0218, 0x021c, 0x0220, 0x0224, 0x0260, 0x0264,
    0x0268, 0x026c, 0x0270, 0x0274, 0x0278, 0x027c, 0x0280, 0x0284,
    0x0288, 0x028c, 0x0290, 0x0294, 0x0298, 0x029c, 0x02a0, 0x02a4,
    0x02a8, 0x02ac, 0x02b0, 0x02b4, 0x02b8, 0x02bc, 0x02c0, 0x02c4,
    0x02c8, 0x02cc, 0x02d0, 0x02d4, 0x02d8, 0x02dc, 0x02e0, 0x02e4,
    0x02e8, 0x02ec, 0x02f0, 0x02f4, 0x02f8, 0x02fc, 0x0300, 0x0304,
    0x0308, 0x030c, 0x0310, 0x0314, 0x0318, 0x031c, 0x1900, 0x1904,
    0x1908, 0x190c, 0x1910, 0x1914, 0x1918, 0x191c, 0x1920, 0x19fc,
    0x1a20, 0x1a44, 0x1a68, 0x1c04, 0x1d0c, 0x1e14, 0x1f1c,
};

/* Only ever read by the firmware -- the declared choice above. */
static const uint16_t z63_blk1524_status_offsets[] = {
    0x01b4, 0x01b8,
};

#endif /* HW_ARM_NIKON_Z63_BLK1524_H */
