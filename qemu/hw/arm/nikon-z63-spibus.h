/*
 * Nikon Z 6III -- the serial-bus controller array at 0x0e700000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Not to be confused with nikon-z63-spi.h, which is about Shared Peripheral
 * Interrupts. This file is the serial bus.
 *
 * Twelve controllers, 0x100 apart. The firmware addresses them by index, not
 * by a descriptor base: sub_40C1464C builds the constant 0x0e700000 with a
 * MOV/MOVK pair, reads the instance number out of the handle, and shifts:
 *
 *     MOV  X8, #0
 *     MOVK X8, #0xe70,LSL#16     ; 0x0e700000
 *     LDR  W9, [X0,#8]           ; the instance number
 *     ADD  X9, X8, X9,LSL#8      ; base + index * 0x100
 *     LDR  W10, [X9,#0x3c]!
 *
 * which is where both the stride and the instance count come from, and why
 * the register set below is one instance's and applies at all twelve.
 *
 * 35 offsets, from 27 functions in 0x40c13000-0x40c18000, extracted by
 * z63-tools/handle_offsets.py --window-base 0x0e700000 --limit 0x100: a
 * forward dataflow over the basic-block graph, recording every constant
 * offset applied to a register provably holding an instance base.
 *
 * This replaces a three-offset model (+0x00, +0x20, +0x60) that was built
 * from the accesses that had been seen rather than from the driver. The
 * machine stopped on 0x0e70073c -- instance 7, +0x3c -- which that model had
 * no reason to know about.
 *
 * What the previous model established, kept here
 * ----------------------------------------------
 * 40c14420 walks a DRAM table twelve times, 0x100 apart, and for each *
 * entry that is flagged present it does: *
 *   n  = table[entry].index *
 *   p  = 0x0e700000 + n * 0x100 *
 *   p[0x60] |= 1                          -- power the chip select up *
 *   drv[i] = 0x0e700000[i].0x48 & 0x1ff   -- read back its setting *
 * The Linux device tree has a node named spi@0x0e700500, and its *
 * property names include cs_pdr_addr and cs_pdr_bit -- chip-select *
 * power-down address and bit -- with cs_pdr_addr = 0x58 and *
 * cs_pdr_bit = 0xc. The node's reg property is <0 0xe700500 0 0x100 *
 * 0 0xd151000 0 0x500>, so the window is the 0x100 the loop walks and *
 * the second reg is exactly the five GPIO banks modelled above. *
 * An earlier version of this comment claimed the 0x60 write was the *
 * chip-select power control. The device tree falsifies that: the *
 * power-down register is +0x58, and the firmware's write is +0x60. So *
 * +0x58 is accepted on the device tree's authority alone -- no *
 * firmware access to it has been observed -- while +0x60 and +0x48 *
 * rest on the firmware, and none of the three has a claimed meaning. *
 * The bus carries an Infineon SLB9670 TPM 2.0, node slb9670@0 on this *
 * controller. That is why a TPM is expected to answer later; nothing *
 * here models it, and a TPM command that needs a real response will *
 * stop and say so rather than be invented. *
 * The +0x48 read is stored into a DRAM config block, so a *
 * never-written window hands back 0; the model does not assert that 0 *
 * is what silicon reports, and no reset value is invented for it.
 *
 * +0x58 is the one offset in the set below that the firmware never touches.
 * It is there on the Linux device tree's authority alone -- cs_pdr_addr =
 * 0x58 on node spi@0x0e700500 -- and not because any instruction reaches
 * it. Everything else in the set is a firmware access. Nothing here claims
 * a meaning for any of them.
 *
 * Why storage is right here
 * -------------------------
 * sub_40C1464C's first act is a read-modify-write that clears bit 0 of
 * +0x3c, and its second is a poll:
 *
 *     LDR   W10, [X9]
 *     TBNZ  W10, #0, loc_40C14670
 *
 * It spins until bit 0 of the word it just cleared reads back clear. With
 * storage that is true immediately, which is the firmware's own stated
 * expectation -- it cleared the bit itself. A model that returned a fixed
 * value here would either hang the machine or lie about a bit the firmware
 * owns.
 *
 * +0x24 is the one offset the firmware only ever reads. For it, storage is a
 * DECLARED CHOICE: this model returns the reset value, and nothing here
 * knows what the hardware would report.
 *
 * Linux drives instance 5 itself (node spi@0x0e700500, the SLB9670 TPM)
 * with spi-nikon.ko, which keeps the ioremapped base at priv+0x18. Its
 * register set, taken from the module the same way (every constant offset
 * on that base, and every nikon_spi_read_reg() argument):
 *
 *     W 0x00 0x08 0x0c 0x10 0x18 0x1c 0x28 0x38 0x64
 *     R 0x04 0x14 0x24 0x2c 0x30 0x48     RW 0x20 0x3c 0x4c
 *
 * All but +0x2c and +0x30 are already in the firmware's set. Those two are
 * read by nikon_spi_write_fifo, nikon_spi_interrupt_event and
 * nikon_spi_expired_timer and written by nobody, so they get the same
 * declared choice as +0x24: the reset value, with no claim about what
 * silicon reports. They were found when the machine stopped on 0x0e70052c
 * from spi-nikon's nikon_spi_read_reg. No TPM is modelled behind the bus.
 */

#ifndef HW_ARM_NIKON_Z63_SPIBUS_H
#define HW_ARM_NIKON_Z63_SPIBUS_H

static const uint16_t z63_spibus_offsets[] = {
    0x0058, /* device tree only; see above */
    0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x001c,
    0x0020, 0x0024, 0x0028, 0x0034, 0x0038, 0x003c, 0x0044, 0x0048,
    0x004c, 0x0050, 0x0054, 0x005c, 0x0060, 0x0064, 0x0068, 0x00a0,
    0x00a4, 0x00a8, 0x00ac, 0x00b0, 0x00b4, 0x00b8, 0x00bc, 0x00c0,
    0x00c4, 0x00c8, 0x00cc,
    0x002c, 0x0030, /* spi-nikon.ko only; see above */
};

/* Only ever read by the firmware (and by spi-nikon.ko for the last two)
 * -- the declared choice above. */
static const uint16_t z63_spibus_status_offsets[] = {
    0x0024, 0x002c, 0x0030,
};

#endif /* HW_ARM_NIKON_Z63_SPIBUS_H */
