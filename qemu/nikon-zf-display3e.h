/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NIKON_ZF_DISPLAY3E_H
#define NIKON_ZF_DISPLAY3E_H
#include <stdbool.h>
#include <stdint.h>
/* Own402e2754/402e2804: bank0@3e, two-byte register/value writes.
 * Known registers only. No part ID, analog voltage or read data invented. */
typedef struct ZfDisplay3e {
    uint8_t value[4], written, address, phase;
    bool selected;
    uint64_t starts, writes, rejects;
} ZfDisplay3e;

static inline bool zf_display3e_start(ZfDisplay3e *p, unsigned bank, unsigned address, bool read)
{
    if (bank != 0 || address != 0x3e) return false;
    p->starts++;
    p->selected = false;
    p->phase = 0;
    if (read) { p->rejects++; return false; }
    p->selected = true;
    return true;
}

static inline bool zf_display3e_write(ZfDisplay3e *p, uint8_t byte)
{
    if (!p->selected) return false;
    if (p->phase == 0) {
        if (byte != 0 && byte != 1 && byte != 3) { p->rejects++; p->selected = false; return false; }
        p->address = byte;
        p->phase = 1;
        return true;
    }
    if (p->phase == 1) {
        p->value[p->address] = byte;
        p->written |= 1u << p->address;
        p->phase = 2;
        p->writes++;
        return true;
    }
    p->rejects++;
    p->selected = false;
    return false;
}

static inline bool zf_display3e_read(ZfDisplay3e *p, uint8_t *byte)
{
    (void)byte;
    p->rejects++;
    return false;
}

static inline void zf_display3e_stop(ZfDisplay3e *p)
{
    p->selected = false;
    p->phase = 0;
}
#endif
