/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../qemu/nikon-zf-ramdump.h"

int main(void)
{
    const uint64_t pc = 0x40c07cc8;
    assert(zf_ramdump_read_allowed(true, true, pc, 0x1508004c, 4));
    assert(zf_ramdump_read_allowed(true, true, pc, 0x15080058, 4));
    assert(zf_ramdump_read_allowed(true, true, pc, 0x150803c0, 4));
    assert(zf_ramdump_read_allowed(true, true, pc, 0x150caffc, 4));
    assert(zf_ramdump_read_allowed(true, true, pc, 0x15280fcc, 4));
    assert(!zf_ramdump_read_allowed(true, false, pc, 0x1508004c, 4));
    assert(!zf_ramdump_read_allowed(false, true, pc, 0x1508004c, 4));
    assert(!zf_ramdump_read_allowed(true, true, pc + 4, 0x1508004c, 4));
    assert(!zf_ramdump_read_allowed(true, true, 0x40c07914, 0x1508004c, 4));
    assert(!zf_ramdump_read_allowed(true, true, pc, 0x1508004d, 4));
    assert(!zf_ramdump_read_allowed(true, true, pc, 0x1508004c, 8));
    assert(!zf_ramdump_read_allowed(true, true, pc, 0x1508004c, 1));
    assert(!zf_ramdump_read_allowed(true, true, pc, 0x150803c4, 4));
    assert(!zf_ramdump_read_allowed(true, true, pc, 0x15081490, 4));
    assert(!zf_ramdump_read_allowed(true, true, pc, 0x150ce800, 4));
    assert(!zf_ramdump_read_allowed(true, true, pc, 0x15280fd0, 4));
    assert(!zf_ramdump_read_allowed(true, true, pc, UINT64_MAX - 3, 4));
    puts("PASS: Zf diagnostic read scope; disabled, wrong-machine/PC/width and range boundaries rejected");
}
