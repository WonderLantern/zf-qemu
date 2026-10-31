#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef int *qemu_irq;
static void qemu_set_irq(qemu_irq irq, int level) { if (irq) *irq = level; }
#include "../qemu/nikon-zf-pcie.h"
int main(void)
{
    ZfPcieConfig s = {0};
    uint64_t v = 99;
    for (unsigned b=0;b<2;b++) {
        uint64_t a=0x17b00000ULL+b*0x80000;
        assert(zf_pcie_config_read(&s,a+0xe4c,4,&v) && v==0);
        assert(zf_pcie_config_write(&s,a+0xe4c,4,0x707));
        assert(zf_pcie_config_read(&s,a+0xe4c,4,&v) && v==0x707);
        assert(!zf_pcie_config_write(&s,a+0xe4c,4,0x708));
        assert(zf_pcie_config_write(&s,a+0xe4c,4,0x700));
        assert(zf_pcie_config_read(&s,a+0xe4c,4,&v) && v==0x700);
        assert(zf_pcie_config_write(&s,a+0x258,4,1));
        assert(!zf_pcie_config_write(&s,a+0x258,4,2));
        assert(zf_pcie_config_write(&s,a+0xe4,4,0xfe010000));
        assert(zf_pcie_config_write(&s,a+0xe8,4,0xf0000000));
        if (b == 0) {
            assert(!zf_pcie_config_read(&s,a+0xe8,4,&v));
        } else {
            assert(zf_pcie_config_read(&s,a+0xe8,4,&v) && v==0);
        }
        assert(!zf_pcie_config_read(&s,a+0xb4,4,&v));
        if (b == 0) {
            assert(!zf_pcie_config_write(&s,a+0xe8,4,1));
        } else {
            s.intx_status = 1;
            assert(zf_pcie_config_write(&s,a+0xe8,4,1));
            assert(s.intx_status == 0);
        }
        assert(!zf_pcie_config_read(&s,a+0xe4c,8,&v));
        for (unsigned off=0x60;off<=0x260;off+=0x200) {
            assert(zf_pcie_config_read(&s,a+off,4,&v) && v==0);
            for (unsigned field=0;field<4;field++) {
                assert(zf_pcie_config_write(&s,a+off,4,(uint64_t)field<<17));
                assert(zf_pcie_config_read(&s,a+off,4,&v) && v==((uint64_t)field<<17));
            }
            assert(!zf_pcie_config_write(&s,a+off,4,0x80000));
            assert(!zf_pcie_config_write(&s,a+off,4,1));
            assert(zf_pcie_config_read(&s,a+off,4,&v) && v==0x60000);
            assert(!zf_pcie_config_read(&s,a+off,2,&v));
        }
    }
    assert(!zf_pcie_config_read(&s,0x17c00e4c,4,&v));
    puts("PASS: wrapper retention, field rejection, no link/status fabrication");
}
