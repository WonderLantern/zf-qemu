/*
 * The HE RAW coder, not carried in yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ../zr-qemu's RAW video path can put its packets through an authored HE
 * forward coder with a pinned public LibRaw inverse, built there as a
 * separate C++ archive (qemu/nikon-he-runtime.cpp, nikon-he-forward.inc and
 * nikon-he-public/). That archive is not part of this tree yet, so these two
 * entry points answer "no".
 *
 * Nothing on the Z 6III's path to a recorded file needs them today: the HE
 * codec model is the immutable opt-in `video-raw-he-codec-model`, which
 * defaults off, and with it off the transport carries zlib WLVR packets
 * instead. A refusal here is the same refusal the model already handles --
 * its own header says a failure never grants a device completion -- so the
 * property simply cannot be turned on until the archive is carried in.
 */
#include "qemu/osdep.h"
#include "nikon-he-runtime.h"

int wl_he_encode14(const uint8_t *packed, size_t bytes, unsigned width,
                   unsigned height, unsigned pitch, uint8_t *output,
                   size_t capacity)
{
    (void)packed; (void)bytes; (void)width;
    (void)height; (void)pitch; (void)output; (void)capacity;
    return 0;
}

int wl_he_decode14(const uint8_t *packet, size_t bytes, unsigned width,
                   unsigned height, uint16_t *output, size_t samples)
{
    (void)packet; (void)bytes; (void)width;
    (void)height; (void)output; (void)samples;
    return 0;
}
