/*
 * Nikon Z 6III -- the three-channel block at 0x154c0000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Three channels 0x400 apart, each with three sub-units at +0x100, +0x200
 * and +0x300. Listed in nikon-z63-display.h as known and unmapped; the
 * MCU-driven start reached it (sub_40C2F7C0, 0x40c2f7e0, reading +0x100 of
 * a channel). ../z9-qemu models the same block on the Z 9 as "stream154c":
 * idle states at +0x100/+0x200/+0x300, write-one-to-clear event words at
 * +0x108/+0x214/+0x314, two banks of buffer addresses -- a stream DMA of
 * some kind. That is the reference; nothing here claims more.
 *
 * The register set came from z63-tools/make_window.py (--const-window,
 * 0x40c2e800-0x40c30c00 of `eg2010_0200i1.bi`, md5
 * b024ec7148e24ec0493e27d4d0f20b0e, read through `z63_eg.elf`, md5
 * 0fedcc0523da1dac8127a8010ccf978b): 39 materialised constants in
 * [0x154c0000, 0x154c0c00) from 21 functions, folded here to one channel
 * (the generator lists them absolute; 0x404 and 0x528..0x558 are channel
 * 1's +0x004 and +0x128..+0x158). Added by hand, because this image indexes
 * them as base + (channel << 10) rather than as constants:
 *
 *   +0x200 +0x300   read by sub_40C2F7C0 (0x40c2f7ec / 0x40c2f7f8)
 *   +0x214 +0x314   read and written back by the channel ISRs
 *                   (sub_40C2F730 and its siblings 0x14 apart)
 *   +0x010 +0x014   strobes, written 1 and never read (z9's reset/stop)
 *   +0x204 +0x20c +0x210 +0x21c +0x28c, and the same at +0x3xx
 *                   the sub-unit set-up (0x40c2f9c0 writes +0x204 = 2,
 *                   0x40c2fac4 +0x210 = 0x111); every (channel << 10) +
 *                   0x154c0xxx term in the decompiled driver
 *   +0x240..+0x27c +0x280 +0x284 +0x288, and the same at +0x3xx
 *                   sub_40C2FAA8 with the sub-unit base in X0: +0x10, four
 *                   address/high-nibble pairs at +0x40 or +0x60 (8 apart),
 *                   +0x1c, +0x80 = 0, +0x84 = 1, +0x88
 *
 * What is a declared choice: storage throughout. The sub-unit states and
 * the fourteen offsets the firmware only reads (+0x160..+0x17c,
 * +0x270..+0x27c, +0x370..+0x37c) answer their reset value, 0 -- idle,
 * which is what sub_40C2F7C0 waits for (it proceeds unless a state reads 3)
 * and what z9 assumes too. The event words are written back with the value
 * read, which storage round-trips; nothing raises an event here.
 *
 * An offset not in this set still stops the machine with its PC.
 */

#ifndef HW_ARM_NIKON_Z63_STREAM154C_H
#define HW_ARM_NIKON_Z63_STREAM154C_H

#define Z63_STRM_BASE    0x154c0000ULL
#define Z63_STRM_SIZE    0x0c00
#define Z63_STRM_STRIDE  0x0400

static const uint16_t z63_strm_offsets[] = {
    0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x001c,
    0x0100, 0x0104, 0x0108, 0x010c, 0x0110, 0x0114, 0x0118, 0x0128,
    0x012c, 0x0130, 0x0134, 0x0144, 0x0148, 0x014c, 0x0150, 0x0154,
    0x0158, 0x0160, 0x0164, 0x0170, 0x0174, 0x0178, 0x017c, 0x0200,
    0x0204, 0x020c, 0x0210, 0x0214, 0x021c, 0x0240, 0x0244, 0x0248,
    0x024c, 0x0250, 0x0254, 0x0258, 0x025c, 0x0260, 0x0264, 0x0268,
    0x026c, 0x0270, 0x0274, 0x0278, 0x027c, 0x0280, 0x0284, 0x0288,
    0x028c, 0x0300, 0x0304, 0x030c, 0x0310, 0x0314, 0x031c, 0x0340,
    0x0344, 0x0348, 0x034c, 0x0350, 0x0354, 0x0358, 0x035c, 0x0360,
    0x0364, 0x0368, 0x036c, 0x0370, 0x0374, 0x0378, 0x037c, 0x0380,
    0x0384, 0x0388, 0x038c,
};

#endif /* HW_ARM_NIKON_Z63_STREAM154C_H */
