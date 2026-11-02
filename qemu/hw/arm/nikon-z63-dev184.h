/*
 * Nikon Z 6III -- device 0x184 at 0x15280000, a four-channel engine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * sub_40C75178 stores 0x15280000 at 0xa0280f220, registers the descriptor
 * 0xa0280f218 as device 0x184 (sub_40C001C4) and line 0x137 with the
 * handler sub_40C751CC. Its one user is the job sub_40CC13FC (from
 * sub_40CC12E0 and sub_4100232c): start (sub_40C75404), program the
 * channels that are enabled (sub_40C757C4, channel 0..3), arm them
 * (sub_40C756E4), then wait on event 79 with a timeout (sub_40CC174C),
 * read each finished channel's results back (sub_40C7599C) and stop
 * (sub_40C75610), or abort (sub_40C75A44) on the timeout. No machine run
 * has reached it; the static sweep of the image found the base unmapped.
 *
 * The register set, from the driver's functions 0x40c751cc-0x40c75a60
 * (handle_offsets.py --field 8 over that range, then each function read,
 * because the channel and result bases are computed):
 *
 *   +0x000 +0x004 +0x008     start/stop words (15, 1/2/4/8 per channel)
 *   +0x020 +0x024 +0x028     sub_40C756D8 / 756CC / 756C0
 *   +0x030                   status, read and written back by the
 *                            handler (0x40c751d4/0x40c751d8), bits 0..3
 *                            one per channel
 *   +0x110..+0x120           sub_40C75750, the job's parameters
 *   +0x124..+0x134           the same five words again, sub_40C757B8
 *   +0x400n + 0x040..0x10c   channel n's 52 setup words (sub_40C757C4:
 *                            v4 = base + 0x40 + 0x400n, words 0..51)
 *   +0x400n + 0x200..0x29c   channel n's results, read only, by
 *   +0x400n + 0x300..0x39c   sub_40C7599C (v4 + 0x1c0 and v4 + 0x2c0,
 *                            8 x 20 bytes each)
 *   +0xfc0 +0xfc4 +0xfc8 +0xfcc   control words the start, stop, arm,
 *                            abort and handler paths write (0x1f, 0)
 *
 * The power-down dump copies 0xfd0 bytes from the base, which agrees with
 * the highest register being +0xfcc.
 *
 * Storage, declared: nothing here finishes a channel, so a job that is
 * started waits out its timeout and takes the abort path; the result words
 * read what they hold, 0.
 */

#ifndef HW_ARM_NIKON_Z63_DEV184_H
#define HW_ARM_NIKON_Z63_DEV184_H

#define Z63_DEV184_BASE     0x15280000ULL
#define Z63_DEV184_SIZE     0x1000
#define Z63_DEV184_CHANNELS 4
#define Z63_DEV184_CHAN_STRIDE 0x400

/* Fixed words, relative to the base. */
static const uint16_t z63_dev184_offsets[] = {
    0x0000, 0x0004, 0x0008, 0x0020, 0x0024, 0x0028, 0x0030,
    0x0110, 0x0114, 0x0118, 0x011c, 0x0120,
    0x0124, 0x0128, 0x012c, 0x0130, 0x0134,
    0x0fc0, 0x0fc4, 0x0fc8, 0x0fcc,
};

/* Word ranges [first, last] of each channel, relative to its 0x400n. */
static const uint16_t z63_dev184_chan_ranges[][2] = {
    { 0x0040, 0x010c },     /* setup, sub_40C757C4 */
    { 0x0200, 0x029c },     /* results, sub_40C7599C */
    { 0x0300, 0x039c },
};

#endif /* HW_ARM_NIKON_Z63_DEV184_H */
