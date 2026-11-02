/*
 * Nikon Z 6III -- the TENC block at 0x152c0000, and its second window at
 * 0x15ac0000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Device 0x1fb. Its init, sub_40C30B5C, builds the descriptor at
 * 0xa02dc5a98 = {0x1fb, 0x152c0000, 0x15ac0000} and installs the handler
 * sub_40C30BC8 on INTID 0x10b -- TENC in this image's interrupt-name table
 * (Z63_SPI_TENC). It is reached once the live view restarts in movie mode:
 * route 1 of the reset controller is pulsed (sub_40C30C24) and
 * sub_40C30D60 initialises the block, the first access the board could not
 * answer (pc 0x40c30d68). ../z9-qemu never reached it. What it encodes is
 * not known; the name is the image's.
 *
 * The register set, from `eg2010_0200i1.bi` (md5
 * b024ec7148e24ec0493e27d4d0f20b0e) read through `z63_eg.elf` (md5
 * 0fedcc0523da1dac8127a8010ccf978b), the driver 0x40c30b5c-0x40c312bc:
 *
 *   - 41 offsets from z63-tools/make_window.py (descriptor base at +8);
 *   - added by hand, because the driver computes them in loops that no
 *     constant-offset scan sees: +0x120..+0x15c (sub_40C30F48, +0x120 +
 *     32 * bank + 8 * i, address/high-nibble pairs), +0x220..+0x25c (+0x220
 *     + 32 * bank + 8 * i) and +0x320..+0x35c (the same + 0x100); and
 *     +0x500..+0x510, +0x514..+0x524, the two five-word groups the helper
 *     sub_40C30E50 fills through a pointer its callers compute
 *     (sub_40C30E18: base + 0x500, sub_40C30EAC: base + 0x514);
 *   - the second window, the descriptor's +0x10 base 0x15ac0000:
 *     +0x38 (sub_40C30DA8 / sub_40C30DD4, written 1 at start) and
 *     +0x14..+0xb4 (sub_40C31074; +0x50 is written once per word of a
 *     table, a FIFO-shaped port).
 *
 * The handler reads +0x1c, writes the value back (W1C, the mask is +0x18,
 * written 0x7ff at init and 0 at reset) and calls the client with
 * "(status & 0x700) != 0". Start is sub_40C30DA8: second window +0x38 = 1,
 * +0x404 = 1, +0x14 = 1, +0x404 = 0, +0x8 = 0 then 1.
 *
 * What is a declared choice: storage throughout. The offsets the firmware
 * only reads (below) answer their reset value, 0; nothing here raises the
 * interrupt or produces anything, so a client waiting on TENC's
 * completion will wait, and that is the next thing to find.
 *
 * An offset not in these sets still stops the machine with its PC.
 */

#ifndef HW_ARM_NIKON_Z63_TENC_H
#define HW_ARM_NIKON_Z63_TENC_H

#define Z63_TENC_BASE    0x152c0000ULL
#define Z63_TENC_SIZE    0x1000
#define Z63_TENC2_BASE   0x15ac0000ULL
#define Z63_TENC2_SIZE   0x1000

static const uint16_t z63_tenc_offsets[] = {
    0x0000, 0x0004, 0x0008, 0x000c, 0x0014, 0x0018, 0x001c, 0x0100,
    0x0104, 0x0108, 0x010c, 0x0110, 0x0114, 0x0118,
    0x0120, 0x0124, 0x0128, 0x012c, 0x0130, 0x0134, 0x0138, 0x013c,
    0x0140, 0x0144, 0x0148, 0x014c, 0x0150, 0x0154, 0x0158, 0x015c,
    0x0160, 0x0164, 0x0170, 0x0174, 0x0178, 0x017c, 0x0184,
    0x0220, 0x0224, 0x0228, 0x022c, 0x0230, 0x0234, 0x0238, 0x023c,
    0x0240, 0x0244, 0x0248, 0x024c, 0x0250, 0x0254, 0x0258, 0x025c,
    0x0270, 0x0274, 0x0278, 0x027c,
    0x0320, 0x0324, 0x0328, 0x032c, 0x0330, 0x0334, 0x0338, 0x033c,
    0x0340, 0x0344, 0x0348, 0x034c, 0x0350, 0x0354, 0x0358, 0x035c,
    0x0370, 0x0374, 0x0378, 0x037c, 0x0404,
    0x0500, 0x0504, 0x0508, 0x050c, 0x0510,
    0x0514, 0x0518, 0x051c, 0x0520, 0x0524, 0x0528, 0x052c,
    0x0530, 0x0534, 0x0538, 0x053c, 0x0540, 0x0544, 0x0548, 0x054c,
    0x0550,
};

/* Only ever read by the firmware -- the declared choice above. */
static const uint16_t z63_tenc_status_offsets[] = {
    0x0160, 0x0164, 0x0170, 0x0174, 0x0178, 0x017c, 0x0270, 0x0274,
    0x0278, 0x027c, 0x0370, 0x0374, 0x0378, 0x037c,
};

static const uint16_t z63_tenc2_offsets[] = {
    0x0014, 0x001c, 0x002c, 0x0038, 0x003c, 0x0044, 0x0048, 0x004c,
    0x0050, 0x00a0, 0x00a4, 0x00a8, 0x00ac, 0x00b0, 0x00b4,
};

#endif /* HW_ARM_NIKON_Z63_TENC_H */
