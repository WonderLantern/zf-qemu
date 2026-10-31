#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include "../qemu/nikon-zf-clock.h"
int main(void)
{
    uint32_t h,p1,p2;
    assert(zf_clock_plan(0x24003010,0,&h,&p1,&p2) && h==16000000 && p1==h && p2==h);
    assert(zf_clock_plan(0,1,&h,&p1,&p2) && h==10000000);
    assert(zf_clock_plan(0x2240190a,2,&h,&p1,&p2) && h==50000000 && p1==h && p2==h);
    /* Actual SysTick reload: firmware's 1 ms contract. */
    assert((49999+1)*1000u==h);
    assert(zf_clock_plan(0x2240190a,2|(8<<4)|(4<<10)|(5<<13),&h,&p1,&p2));
    assert(h==25000000 && p1==12500000 && p2==6250000);
    assert(zf_clock_plan(0x2240190a|(1<<16),2,&h,&p1,&p2) && h==25000000);
    assert(zf_clock_plan(0x2240190a,2|(15<<4),&h,&p1,&p2) && h==97656);
    assert(!zf_clock_plan(0x22401900,2,&h,&p1,&p2)); /* M=0 */
    assert(!zf_clock_plan(0x2240000a,2,&h,&p1,&p2)); /* N=0 */
    assert(!zf_clock_plan(0x2240190a,3,&h,&p1,&p2)); /* reserved source */
    puts("PASS: RCC source, actual Zf 50MHz/1ms, AHB/APB/P divisors and invalid clock rejection");
    return 0;
}
