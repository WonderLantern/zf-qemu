/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NIKON_ZF_EYE_I2C_H
#define NIKON_ZF_EYE_I2C_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* Own EyeDv transfer40c95a24: EG I2C bank3/7-bit38. Tables41f52f60..3020
 * write40=ca;41..43;4a..4e;53..54;41=71/31 enables/disables measurement.
 * Implement that configuration subset. No chip identity/ADC/read values
 * are invented. The external near condition is already a digital result. */
typedef struct ZfEyeI2c {
    uint8_t registers[256], written[256], cursor;
    bool selected, pointer;
    uint64_t starts, writes, resets, unsupported_reads, unsupported_writes;
} ZfEyeI2c;

static inline bool zf_eye_i2c_register(unsigned r)
{
    return (r>=0x40 && r<=0x43) || (r>=0x4a && r<=0x4e) || r==0x53 || r==0x54;
}
static inline bool zf_eye_i2c_start(ZfEyeI2c *p,unsigned bank,unsigned address,bool read)
{
    if(bank!=3 || address!=0x38) { return false; }
    p->starts++;p->selected=false;
    if(read) { p->unsupported_reads++;return false; }
    p->selected=true;p->pointer=true;return true;
}
static inline bool zf_eye_i2c_write(ZfEyeI2c *p,uint8_t byte)
{
    if(!p->selected) { return false; }
    if(p->pointer) {
        if(!zf_eye_i2c_register(byte)) { p->unsupported_writes++;return false; }
        p->cursor=byte;p->pointer=false;return true;
    }
    if(!zf_eye_i2c_register(p->cursor) || (p->cursor==0x40 && byte!=0xca)) {
        p->unsupported_writes++;return false;
    }
    if(p->cursor==0x40) {
        memset(p->registers,0,sizeof(p->registers));
        memset(p->written,0,sizeof(p->written));p->resets++;
    }
    p->registers[p->cursor]=byte;p->written[p->cursor]=1;p->cursor++;p->writes++;
    return true;
}
static inline void zf_eye_i2c_stop(ZfEyeI2c *p) { p->selected=false; }
static inline bool zf_eye_measurement(const ZfEyeI2c *p)
{
    return p->written[0x41] && (p->registers[0x41]&0x40);
}
static inline bool zf_eye_output_near(const ZfEyeI2c *p,bool near)
{
    return zf_eye_measurement(p) && near;
}
#endif
