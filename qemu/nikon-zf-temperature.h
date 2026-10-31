/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NIKON_ZF_TEMPERATURE_H
#define NIKON_ZF_TEMPERATURE_H
#include <stdbool.h>
#include <stdint.h>
/* Own40c8f870/40c8f980/40c8f9e4: bank3@48, config1=60a0/61a0,
 * pointer0 gives a big-endian signed12-bit sample in bits15..4.
 * Raw counts only: no part identity, Celsius calibration or alert pin.
 * Conversion is functional/immediate; each read latches both bytes. */
typedef struct ZfTemperature {
    int16_t input, held, read_sample;
    bool input_set, configured, have_sample, selected, reading;
    uint16_t config, staged_config;
    uint8_t pointer, phase, read_at;
    uint64_t starts, pointer_writes, config_writes, reads, errors;
} ZfTemperature;

static inline bool zf_temperature_running(const ZfTemperature *p)
{
    return p->configured && !(p->config & 0x100);
}
static inline void zf_temperature_input(ZfTemperature *p,int value)
{
    p->input=value;p->input_set=true;
    if(zf_temperature_running(p)) { p->held=value;p->have_sample=true; }
}
static inline bool zf_temperature_start(ZfTemperature *p,unsigned bank,unsigned address,bool read)
{
    if(bank!=3 || address!=0x48) { return false; }
    p->starts++;p->selected=false;
    if(read && (p->pointer!=0 || !p->have_sample)) { p->errors++;return false; }
    p->selected=true;p->reading=read;p->phase=0;p->read_at=0;
    if(read) { p->read_sample=p->held; }
    return true;
}
static inline bool zf_temperature_write(ZfTemperature *p,uint8_t byte)
{
    if(!p->selected || p->reading) { return false; }
    if(p->phase==0) {
        if(byte>1) { p->errors++;return false; }
        p->pointer=byte;p->phase=1;p->pointer_writes++;return true;
    }
    if(p->pointer!=1) { p->errors++;return false; }
    if(p->phase==1) { p->staged_config=(uint16_t)byte<<8;p->phase=2;return true; }
    if(p->phase==2) {
        uint16_t value=p->staged_config|byte;
        if(value!=0x60a0 && value!=0x61a0) { p->errors++;return false; }
        p->config=value;p->configured=true;p->config_writes++;p->phase=3;
        if(zf_temperature_running(p)) { p->held=p->input;p->have_sample=true; }
        return true;
    }
    p->errors++;return false;
}
static inline bool zf_temperature_read(ZfTemperature *p,uint8_t *byte)
{
    if(!p->selected || !p->reading || p->read_at>=2) { p->errors++;return false; }
    uint16_t wire=(uint16_t)((uint16_t)p->read_sample<<4);
    *byte=p->read_at==0 ? wire>>8 : wire&255;
    p->read_at++;p->reads++;return true;
}
static inline void zf_temperature_stop(ZfTemperature *p) { p->selected=false; }
#endif
