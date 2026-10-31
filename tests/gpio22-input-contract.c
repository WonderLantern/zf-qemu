/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../qemu/nikon-zf-gpio-input.h"
int main(void)
{
    const uint64_t offsets[] = {0, 0x54, 0x58, 0x5c, 0x158, UINT64_MAX};
    unsigned long checks=0;
    for (unsigned value=0;value<65536;value++) {
        for (unsigned zf=0;zf<2;zf++) for (unsigned high=0;high<2;high++) {
            for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
                uint16_t result=zf_gpio22_bit8_word(zf,high,false,offsets[i],value);
                bool selected=zf && high && offsets[i]==0x58;
                assert((result&~0x100U)==(value&~0x100U));
                assert(selected ? (result&0x100U)!=0 : result==value);
                checks++;
            }
        }
    }
    printf("PASS %lu input-domain cases: only selected Zf bit may rise; all other bits/ports preserved\n",checks);
}
