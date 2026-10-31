/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NIKON_ZF_TOPLCD_H
#define NIKON_ZF_TOPLCD_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Own EX 0802ef88 and table080506dc; user-confirmed 8+7 segments: one dot.
 * SPI2 templates0805071c..54: 81..84 have one byte, 92/93 none, A0 two.
 * PA4 CS is active low (0802f308/0802f328), PE8 is DISP. No MISO reply,
 * analogue LCD drive, contrast/bias or inferred countdown is supplied. */
#define ZF_TOPLCD_W 192
#define ZF_TOPLCD_H 112
typedef struct ZfSegmentPanel {
    bool cs_high, invalid;
    uint8_t command, at, ram[2], staging[2], registers[4], power_command;
    uint64_t bytes, frames, writes, dropped, errors, truncated, frame_bytes;
} ZfSegmentPanel;

static const uint8_t zf_segment_wire_bits[16] = {
    8,9,10,11,14,12,13,0,1,2,3,6,4,5,7,15
};

static inline void zf_segment_cs(ZfSegmentPanel *p, bool high)
{
    if (high == p->cs_high) { return; }
    if (high) {
        if (p->frame_bytes) { p->frames++; }
        if (p->command) { p->truncated++; }
    }
    p->cs_high = high;
    p->command = p->at = 0;
    p->invalid = false;
    p->frame_bytes = 0;
}

static inline void zf_segment_byte(ZfSegmentPanel *p, uint8_t byte)
{
    p->bytes++;
    if (p->cs_high) { p->dropped++; return; }
    p->frame_bytes++;
    if (p->invalid) { return; }
    if (p->command == 0xa0) {
        p->staging[p->at++] = byte;
        if (p->at == 2) {
            memcpy(p->ram, p->staging, 2);
            p->writes++;
            p->command = p->at = 0;
        }
    } else if (p->command >= 0x81 && p->command <= 0x84) {
        p->registers[p->command - 0x81] = byte;
        p->command = 0;
    } else if ((byte >= 0x81 && byte <= 0x84) || byte == 0xa0) {
        p->command = byte; p->at = 0;
    } else if (byte == 0x92 || byte == 0x93) {
        p->power_command = byte; /* opaque observed command, not a status */
    } else {
        p->errors++; p->invalid = true;
    }
}

static inline void zf_segment_masks(const ZfSegmentPanel *p,
                                     uint8_t digits[2], bool flags[2])
{
    /* flags[0] is the only decimal point; logical16/flags[1] is retained
     * as an unknown auxiliary bit, never invented as a second dot. */
    digits[0] = digits[1] = 0; flags[0] = flags[1] = false;
    for (unsigned i = 0; i < 16; i++) {
        unsigned bit = zf_segment_wire_bits[i];
        if (!(p->ram[bit >> 3] & (0x80u >> (bit & 7)))) { continue; }
        if (i < 14) { digits[i / 7] |= 1u << (i % 7); }
        else { flags[i - 14] = true; }
    }
}

static inline char zf_segment_character(uint8_t mask)
{
    static const uint8_t digits[] = {0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};
    if (!mask) { return ' '; }
    if (mask == 0x40) { return '-'; }
    for (unsigned i=0;i<10;i++) { if (mask==digits[i]) { return '0'+i; } }
    return '?'; /* pixels still display the exact segments */
}

static inline void zf_segment_text(const ZfSegmentPanel *p, char text[5])
{
    uint8_t digits[2]; bool flags[2]; unsigned at=0;
    zf_segment_masks(p,digits,flags);
    for (unsigned i=0;i<2;i++) {
        text[at++]=zf_segment_character(digits[i]);
        if (i==0 && flags[0]) { text[at++]='.'; }
    }
    text[at]=0;
}

static inline uint32_t zf_segment_pixel(const uint8_t digits[2],
                                       const bool flags[2], bool visible,
                                       unsigned x, unsigned y)
{
    /* Functional geometry, not a photograph of the glass. a,b,c,d,e,f,g. */
    static const unsigned boxes[8][4] = {
        {24,8,64,16},{64,16,72,48},{64,56,72,88},{24,88,64,96},
        {16,56,24,88},{16,16,24,48},{24,48,64,56},{80,88,88,96}
    };
    if (!visible || x>=ZF_TOPLCD_W || y>=ZF_TOPLCD_H) { return 0x00dadfd4; }
    unsigned digit=x/96; x%=96;
    for (unsigned i=0;i<8;i++) {
        if (i==7 && digit==1) { continue; } /* no physical right dot */
        if (x>=boxes[i][0] && x<boxes[i][2] && y>=boxes[i][1] && y<boxes[i][3]) {
            bool lit=i==7 ? flags[0] : !!(digits[digit] & (1u<<i));
            return lit ? 0x00182018 : 0x00c7cdbf;
        }
    }
    return 0x00dadfd4;
}
#endif
