/* Exercise the actual overlay header with deterministic timer/IRQ/entropy
 * boundaries. This is a device contract test, not firmware/hardware execution.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef uint64_t hwaddr;
typedef void Object;
typedef void DeviceState;
typedef struct { int unused; } MemoryRegion;
typedef struct { void (*cb)(void *); void *opaque; bool pending; int64_t due; } QEMUTimer;
typedef int *qemu_irq;
typedef struct { MemoryRegion mem; int armv7m; } Z63Stm32;
typedef struct {
    uint64_t (*read)(void *, hwaddr, unsigned);
    void (*write)(void *, hwaddr, uint64_t, unsigned);
    int endianness;
    struct { unsigned min_access_size, max_access_size; } valid;
} MemoryRegionOps;
#define QEMU_CLOCK_VIRTUAL 0
#define DEVICE_LITTLE_ENDIAN 0
#define DEVICE(x) ((DeviceState *)(x))
#define g_new0(t,n) ((t *)calloc((n),sizeof(t)))
static int64_t now;
static int irq, stops;
static unsigned entropy_calls;
static void qemu_set_irq(qemu_irq q, int value) { *q = value; }
static int64_t qemu_clock_get_ns(int clock) { return now; }
static QEMUTimer *timer_new_ns(int c, void (*cb)(void *), void *p) {
    QEMUTimer *t = calloc(1,sizeof(*t)); t->cb=cb; t->opaque=p; return t;
}
static void timer_mod(QEMUTimer *t, int64_t due) { t->pending=true; t->due=due; }
static void timer_del(QEMUTimer *t) { t->pending=false; }
static qemu_irq qdev_get_gpio_in(DeviceState *d, int n) { assert(n==80); return &irq; }
static uint32_t z63_stm32_pc(void) { return 0; }
static void z63_stm32_stop(const char *b, hwaddr a, unsigned z, bool w, uint64_t v) { stops++; }
static void error_report(const char *format, ...) { }
static void memory_region_init_io(MemoryRegion *m,Object *o,const MemoryRegionOps *ops,
                                 void *state,const char *name,uint64_t size) { assert(size==0x400); }
static void memory_region_add_subregion(MemoryRegion *m,hwaddr base,MemoryRegion *sub) {
    assert(base==0x50060800);
}
void qemu_guest_getrandom_nofail(void *p,size_t n) {
    uint32_t word=0xa5000000+(++entropy_calls); assert(n==4); memcpy(p,&word,4);
}
#include "../qemu/nikon-zf-rng.h"
static void advance(ZfRng *s,int64_t n) {
    now=n; if(s->timer->pending && s->timer->due<=now) {
        s->timer->pending=false; s->timer->cb(s);
    }
}
int main(void) {
    ZfRng s={.irq=&irq}; s.timer=timer_new_ns(0,zf_rng_ready,&s);
    assert(zf_rng_read(&s,0,4)==0 && zf_rng_read(&s,4,4)==0);
    zf_rng_write(&s,0,4,4); assert(s.timer->pending && irq==0);
    advance(&s,1999); assert(s.sr==0 && entropy_calls==0);
    advance(&s,2000); assert(s.sr==1 && entropy_calls==1 && irq==0);
    zf_rng_write(&s,0,12,4); assert(irq==1); /* enabling IE on pending data */
    assert(zf_rng_read(&s,8,4)==0xa5000001 && irq==0 && s.sr==0);
    assert(s.timer->pending && s.timer->due==4000);
    advance(&s,4000); assert(s.sr==1 && irq==1 && entropy_calls==2);
    s.sr|=0x60; zf_rng_write(&s,4,0x20,4); assert(s.sr==0x21);
    zf_rng_write(&s,4,0,4); assert(s.sr==1); /* DRDY is read-only */
    zf_rng_write(&s,0,0,4); assert(s.sr==0 && irq==0 && !s.timer->pending);
    advance(&s,10000); assert(entropy_calls==2);
    zf_rng_write(&s,0,4,4); zf_rng_write(&s,0,0,4);
    advance(&s,20000); assert(entropy_calls==2 && s.sr==0);
    zf_rng_write(&s,0,16,4); assert(stops==1 && s.cr==0);
    zf_rng_write(&s,8,1,4); assert(stops==2);
    assert(zf_rng_read(&s,3,1)==0 && stops==3);
    assert(zf_rng_read(&s,12,4)==0 && stops==4);
    Z63Stm32 m={0}; zf_rng_init(&m,NULL);
    puts("PASS: RNG reset, enable, latency, entropy, IRQ, consume, clear, cancel and strict access contract");
    return 0;
}
