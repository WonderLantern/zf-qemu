#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include "../qemu/nikon-zf-matrix.h"
int main(void)
{
    /* Original boot65 GPIOF mode/pull: PF1 output, PF10 input/up.
     * Exactly the selected row may pull the MENU column low; simply
     * closing a button must not make all six scan rows report it.
     */
    const uint32_t mode=0x10000555;
    unsigned active=0;
    for (unsigned row=0;row<6;row++) {
        uint32_t odr=row==1 ? 0 : 2;
        active+=zf_menu_contact_low(true,5,10,mode,odr);
        assert(!zf_menu_contact_low(false,5,10,mode,odr));
        for(unsigned p=0;p<11;p++)for(unsigned b=0;b<16;b++)
            if(p!=5||b!=10)assert(!zf_menu_contact_low(true,p,b,mode,odr));
    }
    assert(active==1);
    for(unsigned rowmode=0;rowmode<4;rowmode++)
        for(unsigned colmode=0;colmode<4;colmode++) {
            uint32_t m=(mode&~((3u<<2)|(3u<<20)))|(rowmode<<2)|(colmode<<20);
            assert(zf_menu_contact_low(true,5,10,m,0)==(rowmode==1&&colmode==0));
        }
    puts("PASS: one selected MENU row, released contact, other pins and GPIO modes");
    for(unsigned key=0;key<=5;key++) {
        unsigned closures=0;
        for(unsigned row=0;row<6;row++) {
            uint32_t odr=0x3f & ~(1u<<row);
            for(unsigned p=0;p<11;p++)for(unsigned b=0;b<16;b++) {
                bool closed=zf_navigation_contact_low(key,p,b,mode,odr);
                assert(closed==(key>=1&&key<=4&&p==5&&b==key+6&&row==3));
                closures+=closed;
            }
        }
        assert(closures==(key>=1&&key<=4));
    }
    for(unsigned key=1;key<=4;key++)for(unsigned rowmode=0;rowmode<4;rowmode++)
        for(unsigned colmode=0;colmode<4;colmode++) {
            unsigned pin=key+6;
            uint32_t m=(mode&~((3u<<6)|(3u<<(pin*2))))|(rowmode<<6)|(colmode<<(pin*2));
            assert(zf_navigation_contact_low(key,5,pin,m,0)==(rowmode==1&&colmode==0));
        }
    puts("PASS: all four navigation contacts, six scan rows, release, unused pins and mode changes");
}
