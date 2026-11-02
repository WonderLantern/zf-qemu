/*
 * Nikon Z 6III -- devices 0x1cc, 0x1cd and 0x1ce in ISP_APB.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Three devices the firmware's table names in one group, with their shared
 * driver entry points right after them. At flat 0x451f9598 onward (runtime =
 * flat + the measured .data delta 0x9bd60f000):
 *
 *     0x1cc  0x15180000
 *     0x1cd  0x15182000
 *     0x1ce  0x151c0000
 *     ops    0x40c0b8b8  0x40c0b8e8  0x40c0b908  0x40c0b90c
 *
 * Not a strided family: 0x2000 then 0x3e000 apart. Three windows, one
 * register set, which is what the table says they are.
 *
 * The machine found them on a set-then-clear of +0x14 through a descriptor:
 *
 *     LDR  X8, [X19,#8]           ; the device base
 *     STR  W20, [X8,#0x14]        ; 0x40c0aaf0, W20 = 1
 *     STR  WZR, [X8,#0x14]        ; 0x40c0aaf4
 *
 * A pulse, with nothing read back, so storage is the firmware's own
 * expectation for that register.
 *
 * The register set
 * ----------------
 * 90 offsets, highest 0x494, from 28 functions in
 * 0x40c0a000-0x40c0d000, by z63-tools/handle_offsets.py --field 8: a forward
 * dataflow over the basic-block graph.
 *
 * The window is 0x2000, the most the table allows -- 0x15180000 and
 * 0x15182000 are only 0x2000 apart -- because the per-channel registers
 * below run to +0x1c00 + 0x21c. They were 0x1000 while only the harvested
 * set was known.
 *
 * What is a declared choice
 * -------------------------
 * Storage, and giving all three the same set. The second rests on the three
 * being one group in the table with one block of entry points; their
 * individual accesses have not been separated. 28 offsets are only ever
 * read, and for those the reset value is returned with no claim about what
 * hardware would report.
 *
 * An offset not in the set still stops the machine with its PC.
 */

#ifndef HW_ARM_NIKON_Z63_DEV1CC_H
#define HW_ARM_NIKON_Z63_DEV1CC_H

#define Z63_DEV1CC_SIZE  0x02000

static const struct {
    const char *name;
    hwaddr base;
} z63_dev1cc[] = {
    { "DEV-0x1CC", 0x15180000ULL },
    { "DEV-0x1CD", 0x15182000ULL },
    { "DEV-0x1CE", 0x151c0000ULL },
};

static const uint16_t z63_dev1cc_offsets[] = {
    0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x001c,
    0x0020, 0x0024, 0x0028, 0x002c, 0x0030, 0x0034, 0x0038, 0x003c,
    0x0040, 0x0044, 0x0048, 0x004c, 0x0050, 0x0054, 0x0058, 0x005c,
    0x0060, 0x0064, 0x0068, 0x006c, 0x0070, 0x0074, 0x0078, 0x007c,
    0x0080, 0x0088, 0x008c, 0x0098, 0x009c, 0x00a0, 0x00a4, 0x00a8,
    0x00ac, 0x00b0, 0x00b4, 0x00b8, 0x00bc, 0x00c0, 0x00c4, 0x00d4,
    0x00d8, 0x00f4, 0x00f8, 0x00fc, 0x0144, 0x0334, 0x0338, 0x033c,
    0x0340, 0x0344, 0x0348, 0x034c, 0x0350, 0x0354, 0x0358, 0x035c,
    0x0360, 0x0364, 0x0368, 0x036c, 0x0370, 0x0374, 0x0378, 0x037c,
    0x0400, 0x0404, 0x0410, 0x0414, 0x0418, 0x041c, 0x0420, 0x0424,
    0x0428, 0x0430, 0x0434, 0x0470, 0x0474, 0x0480, 0x0484, 0x0488,
    0x0490, 0x0494,
};

/*
 * Eight channels, two groups of four, which the harvest cannot see because
 * the base is computed: the loop at 0x40c0b048-0x40c0b0f0 runs i < 2,
 * j < 4 over base + 0x1000 * i + 0x400 * j and writes +0x144 (0x40c0b084)
 * and +0x15c/+0x160 (0x40c0b0ac/0x40c0b0b4), then calls three helpers with
 * (i, j) that build the same base: sub_40C0B354 (+0x154..+0x1bc),
 * sub_40C0B4C4 (+0x1c0..+0x1e0) and sub_40C0B524 (+0x200..+0x21c). The
 * helpers are also called from 0x40c0b1e4, 0x40c0b260 and 0x40c94c60..
 * 0x40c94d30. The set below is one channel's, relative to its base, and
 * is applied at all eight. Storage, written only.
 */
static const uint16_t z63_dev1cc_chan_offsets[] = {
    0x0144, 0x0154, 0x0158, 0x015c, 0x0160, 0x0164, 0x0168, 0x016c,
    0x0170, 0x0174, 0x0178, 0x017c, 0x0180, 0x0184, 0x0188, 0x018c,
    0x0190, 0x0198, 0x019c, 0x01a0, 0x01a4, 0x01a8, 0x01ac, 0x01b0,
    0x01b4, 0x01b8, 0x01bc, 0x01c0, 0x01c4, 0x01c8, 0x01cc, 0x01d0,
    0x01d4, 0x01d8, 0x01dc, 0x01e0, 0x0200, 0x0204, 0x0208, 0x020c,
    0x0210, 0x0214, 0x0218, 0x021c,
};

/* Only ever read by the firmware -- the declared choice above. */
static const uint16_t z63_dev1cc_status_offsets[] = {
    0x0010, 0x0038, 0x003c, 0x0334, 0x0338, 0x033c, 0x0340, 0x0344,
    0x0348, 0x034c, 0x0350, 0x0354, 0x0358, 0x035c, 0x0360, 0x0364,
    0x0368, 0x036c, 0x0370, 0x0374, 0x0378, 0x037c, 0x0470, 0x0474,
    0x0484, 0x0488, 0x0490, 0x0494,
};

#endif /* HW_ARM_NIKON_Z63_DEV1CC_H */
