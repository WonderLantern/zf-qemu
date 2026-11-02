/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Nikon Z 6III (EXPEED 7) research machine -- the USB complex,
 * 0x17880000 and the four glue blocks at 0x17A00000 .. 0x17A60000.
 *
 * Reversed from the decrypted factory RTOS image eg2010_0200i1.bi
 * (sha256 736ceaac...1c47fda). Every behaviour below cites the firmware VA
 * that establishes it; file offset = VA - 0x40160000. The full recovered
 * register map, the identification evidence, the offset census and
 * everything this file deliberately refuses are in docs/usb-registers.md
 * -- read that before extending this.
 *
 * ------------------------------------------------------------------
 * WHAT THE COMPLEX IS
 * ------------------------------------------------------------------
 *
 * One Synopsys DesignWare USB 3 controller (DWC_usb3, "DWC3") at
 * 0x17880000, driven by this RTOS component in DEVICE (peripheral) mode
 * only, plus four small glue register files.
 *
 * The identification is not from the address, not from the Z 6III Linux
 * device tree (which is in a different component and is not consulted
 * here), and not from the interrupt-source table alone. It is from a
 * 160-entry uint16 offset table at rodata 0x4185B3D8 that the driver's two
 * register accessors index:
 *
 *     sub_40C71848  *(u32 *)(word_4185B3D8[i] + 0x17880000) = v   (0x40C71874)
 *     sub_40C7187C   return *(u32 *)(word_4185B3D8[i] + 0x17880000)
 *                                                            (0x40C718A8)
 *
 * Printed out, that table IS the published DWC3 map: 0x0000..0x0058 the
 * xHCI capability + operational set with CAPLENGTH 0x20 and the spec's
 * reserved holes absent; 0x0420/0x0430 two port register sets at
 * op_base + 0x400 + 0x10n; 0x0440 + 0x0460..0x047C MFINDEX and
 * interrupter 0 (IMAN/IMOD/ERSTSZ/hole/ERSTBA/ERDP); 0xC100..0xC134 the
 * DWC3 globals ending GSNPSID/GUID/GUCTL/GBUSERRADDR; 0xC200
 * GUSB2PHYCFG(0); 0xC2C0 GUSB3PIPECTL(0); 0xC300+/0xC380+ the 16+16 FIFO
 * size registers; 0xC400.. the event-buffer quad; 0xC700.. the device
 * block; and 0xC800..0xC8FC exactly 16 endpoints x
 * {DEPCMDPAR2, DEPCMDPAR1, DEPCMDPAR0, DEPCMD}.
 *
 * Four independent confirmations from the values the driver writes:
 *
 *   - 0x1788C110 (the one register also reached as a bare immediate) has
 *     bit 11 pulsed set/delay/clear/delay at 0x40C71820..0x40C7183C: the
 *     DWC3 GCTL CoreSoftReset sequence.
 *   - GCTL is written `old & 0x60DC7 | 0x30C02000` (sub_40C70F14), whose
 *     bits [13:12] = 2 is PrtCapDir = Device.
 *   - GUCTL is written 0x0A414802 (sub_40C71110); GUCTL[31:22] REFCLKPER
 *     = 41, a ~24 MHz reference clock.
 *   - DEPCMD words are composed as (param << 16) | cmd[3:0] with 0x800
 *     HIPRI_FORCERM forced for command 8, 0x100 CMDIOC added when
 *     DCTL[31] RUN_STOP is set, 0x400 CMDACT always: sub_40C70378.
 *
 * The interrupt-source name table names three lines. Their SPI numbers
 * follow (VA - 0x41847000) / 0x20, the same arithmetic that yields this
 * board's already-verified Z63_UART_IRQ_SPI 173 from UART_0_RX at
 * 0x418485A0:
 *
 *     EXS_USB3_TYPE_C_CONTROLLER      0x41847D80   SPI 108
 *     EXS_USB3_TYPE_C_ASSIST          0x41847DA0   SPI 109
 *     EXS_USB3_NOT_TYPE_C_CONTROLLER  0x41847DC0   SPI 110
 *
 * No interrupt is raised by this model. The qemu_irq fields are not even
 * declared, because nothing here knows when a line should assert.
 *
 * ------------------------------------------------------------------
 * TWO THINGS THAT ARE NOT PART OF THIS COMPLEX
 * ------------------------------------------------------------------
 *
 * 1. 0x1A000000 / 0x1A400000 / 0x1A800000 / 0x1AC00000 is PCI EXPRESS,
 *    not USB. They are the DBI (Data Bus Interface) windows of four
 *    Synopsys DesignWare PCIe root ports -- two controllers x two ports,
 *    matching EXS_PCIE00/01/10/11 in the interrupt table. Settled by the
 *    DWC port-logic registers the firmware names at their published DBI
 *    offsets inside sub_40287ED0: 0x8BC MISC_CONTROL_1_OFF written 1 then
 *    0 around the read-only config writes (bit 0 = DBI_RO_WR_EN), 0x80C
 *    LINK_WIDTH_SPEED_CONTROL bit 17 PORT_LOGIC_SPEED_CHANGE, 0x890
 *    GEN3_RELATED_OFF, 0x8A8 GEN3_EQ_CONTROL_OFF, the MSI block at
 *    0x820/0x828/0x82C/0x830, and the UNROLLED iATU at DBI+0x300000 with
 *    a 0x200 region stride. Configuration access is the DWC scheme, not
 *    ECAM at a fixed base: outbound region 0 is programmed TYPE=4 (CFG0),
 *    base 0, limit 0xFFFF, REGION_EN, and the 64 KiB config window then
 *    lives in the port's AXI outbound aperture; the root port's OWN
 *    config space is read directly in the DBI window at offset 0.
 *    Access there is 1, 2 and 4 bytes wide (LDRB at 0x40289CA8), unlike
 *    this complex.
 *
 *    It is NOT MAPPED here and falls through to the board's z63_unmapped
 *    trap. docs/usb-registers.md section 8 has the whole thing, including
 *    the link-up state machine in the controller wrappers at 0x17B00000 /
 *    0x17B80000 and the four specific gaps between this firmware and
 *    QEMU's existing hw/pci-host/designware.c.
 *
 * 2. "0x1AA00000" is DRAM, not a register file. Every such address in the
 *    driver carries a MOVK #0xA,LSL#32 (e.g. 0x40C6DEBC) and is really
 *    0xA1AA00000 = Z63_DRAM_BASE + 0x1AA00000. It is the gadget's DMA
 *    area: EP0 setup buffer, nine 4 KiB endpoint buffers at
 *    0xA1AA00410 + (n << 12), nine 1040-byte TRB rings at
 *    0xA1AA09410 + 1040n, and the DWC3 event buffer at 0xA1AA0C000 --
 *    the last confirmed by GEVNTADRLO/HI being written 0x1AA0C000 / 0xA.
 *    Nothing in this file touches it; it is ordinary RAM to the model.
 *
 * ------------------------------------------------------------------
 * ACCESS WIDTH
 * ------------------------------------------------------------------
 *
 * Uniformly 32-bit. Every MMIO access in the complex is LDR Wn / STR Wn;
 * checked by listing all load/store instructions of sub_40C71848,
 * sub_40C7187C, sub_402BB2D8, sub_402BB30C, sub_40C716E4, sub_40C717B4,
 * sub_40C717EC, sub_40C7160C and sub_40C719E4. The LDRH instructions in
 * those functions (0x402BB300, 0x402BB334, 0x40C71870, 0x40C718A4) read
 * the offset TABLES out of rodata/.data and the LDRB/STRB ones touch BSS
 * flags -- none of them is MMIO. So .valid = {4, 4} below is a
 * measurement, not a convention, and a narrower access is a surprise
 * worth stopping on.
 *
 * ------------------------------------------------------------------
 * WHAT THIS MODEL IS NOT
 * ------------------------------------------------------------------
 *
 * There is no USB device model here, no descriptor handling, no TRB
 * execution, no event generation and no transfer emulation -- by
 * instruction, and because none of it is a register. The gadget class
 * logic (EF_UVC / MTX_UAC / IAP2CTLSVC) lives in sub_40C6C7F8 and
 * sub_40C6E728 and is not modelled at all.
 *
 * This header must be included by hw/arm/nikon-z63.c; see the wiring note
 * at the end. It calls the board's z63_stop_read / z63_stop_write so that
 * an access it cannot justify stops the VM with the offending PC rather
 * than returning an invented value.
 */
#ifndef HW_ARM_NIKON_Z63_USB_H
#define HW_ARM_NIKON_Z63_USB_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "system/memory.h"

/* Defined by the board, below the device models. Forward-declared so this
 * header can be included before NikonZ63State is defined.
 *
 * hw/arm/nikon-z63-display.h declares the same two helpers for the same
 * reason and guards them with this macro; hw/arm/nikon-z63-storage.h
 * declares them unguarded. Two identical static declarations in one
 * translation unit are valid C and clang does not implement
 * -Wredundant-decls, but GCC does, so the guard makes the include order
 * stop mattering for whichever headers carry it. */
#ifndef Z63_STOP_HELPERS_DECLARED
#define Z63_STOP_HELPERS_DECLARED
static void z63_stop_read(const char *block, hwaddr addr, unsigned size);
static void z63_stop_write(const char *block, hwaddr addr, unsigned size,
                           uint64_t value);
#endif

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* The DWC3. 64 KiB is the databook's register space (xHCI 0x0000-0xBFFF,
 * globals 0xC000-0xCFFF, link block 0xD000+) and the firmware's own
 * offset table runs to 0xD06C, so the whole window is claimed. Offsets
 * inside it that no call site names still stop; see
 * z63_dwc3_offset_known. */
#define Z63_DWC3_BASE           0x17880000ULL
#define Z63_DWC3_SIZE           0x10000

/* EXS subsystem control. 68 indexed registers at 0x000..0x80C plus ten
 * interrupt-status registers at 0x1000..0x1024; the offsets come from a
 * 68-entry uint16 table in .data at 0xA02808508 (file offset 0x5099508)
 * read by sub_402BB2D8 (0x402BB300) and sub_402BB30C (0x402BB334).
 *
 * THIS BLOCK IS SHARED. The PCIe driver uses indices 13/15/17/19
 * (offsets 0x034/0x03C/0x044/0x04C, sub_40289B30) and 49/51/52
 * (0x618/0x704/0x708, sub_40287ED0). It is modelled here because the USB
 * bring-up cannot run without it and nothing else in the tree claims it;
 * if a PCIe model ever lands, this region moves there or is shared. */
#define Z63_EXS_BASE            0x17A00000ULL
#define Z63_EXS_SIZE            0x2000
#define Z63_EXS_IRQ_FIRST       0x1000 /* ten 2-bit W1C status registers */
#define Z63_EXS_IRQ_LAST        0x1024
#define Z63_EXS_SD_DETECT       0x000C /* index 3: bit 0 = SD card absent */
#define Z63_EXS_PCIE2_STATUS    0x0044 /* index 17: bit 1 = port 2 card/link */

/* The two USB3 PHY / isolation glue blocks, one per port. Not an array
 * with a clean stride: 0x17A40000 sits between them. */
#define Z63_USB_PHY_INST        2
#define Z63_USB_PHY0_BASE       0x17A20000ULL /* Type-C port      */
#define Z63_USB_PHY1_BASE       0x17A60000ULL /* the other port   */
#define Z63_USB_PHY_SIZE        0x1000

/* "Type-C assist" -- EXS_USB3_TYPE_C_ASSIST, SPI 109. What it DOES is not
 * established; only its six register roles are. See below. */
#define Z63_USB_ASSIST_BASE     0x17A40000ULL
#define Z63_USB_ASSIST_SIZE     0x1000

/* ------------------------------------------------------------------ */
/* DWC3 registers the firmware names                                   */
/*                                                                    */
/* The DWC names are the identification's conclusion and are used here  */
/* as a reading aid; the offsets and the bit positions are measured.    */
/* ------------------------------------------------------------------ */

#define Z63_DWC3_GCTL           0xC110 /* RMW: PrtCapDir=Device, sub_40C70F14
                                        * bit 11 CoreSoftReset pulsed at
                                        * 0x40C71820/24/28 and /30/38/3C */
#define Z63_DWC3_GUCTL1         0xC11C /* RMW old & 0xFEFF87FF | cfg<<24   */
#define Z63_DWC3_GUCTL          0xC12C /* = 0x0A414802, REFCLKPER 41 ns    */
#define Z63_DWC3_GUSB2PHYCFG    0xC200 /* bit 6 SusPHY, bit 8 EnblSlpM     */
#define Z63_DWC3_GUSB3PIPECTL   0xC2C0 /* bit 17 SusPHY                    */
#define Z63_DWC3_GEVNTADRLO     0xC400 /* = 0x1AA0C000, read back          */
#define Z63_DWC3_GEVNTADRHI     0xC404 /* = 0xA, read back                 */
#define Z63_DWC3_GEVNTSIZ       0xC408 /* size 0x1000, bit 31 int mask     */
#define Z63_DWC3_GEVNTCOUNT     0xC40C /* event bytes pending              */
#define Z63_DWC3_DCFG           0xC700 /* [9:3] DEVADDR, [21:17] NUMP=0xF  */
#define Z63_DWC3_DCTL           0xC704 /* bit 31 RUN_STOP, bit 30 CSFTRST  */
#define Z63_DWC3_DEVTEN         0xC708 /* = old & 0xC1A0 | 0x25F           */
#define Z63_DWC3_DSTS           0xC70C /* read-only: CONNECTSPD, USBLNKST  */
#define Z63_DWC3_DGCMDPAR       0xC710
#define Z63_DWC3_DGCMD          0xC714 /* bit 10 CMDACT                    */
#define Z63_DWC3_DALEPENA       0xC720 /* bit (2*ep)|dir                   */

/* Sixteen endpoint command blocks: PAR2 / PAR1 / PAR0 / DEPCMD.
 * The per-endpoint index tables are at rodata 0x4185B370 (PAR2),
 * 0x4185B330 (PAR1), 0x4185B2F0 (PAR0) and 0x4185B2B0 (DEPCMD), each 16
 * uint32 holding offset-table indices 91+4n, 92+4n, 93+4n, 94+4n. Used by
 * sub_40C702B4 (0x40C702EC/0x40C7030C/0x40C7032C) and sub_40C70378. */
#define Z63_DWC3_DEP_BASE       0xC800
#define Z63_DWC3_DEP_INST       16
#define Z63_DWC3_DEP_STRIDE     0x10
#define Z63_DWC3_DEP_END        (Z63_DWC3_DEP_BASE + \
                                 Z63_DWC3_DEP_INST * Z63_DWC3_DEP_STRIDE)
#define Z63_DWC3_DEPCMD_IN_BLK  0x0C /* PAR2 +0, PAR1 +4, PAR0 +8, CMD +C */

/* The three self-clearing bits this model has to answer. Each is set by
 * the firmware and then polled; the loops are transcribed in
 * docs/usb-registers.md section 4 and replayed in section 6. */
#define Z63_DWC3_DCTL_CSFTRST   (1u << 30) /* poll 0x40C7118C, TBZ #0x1E  */
#define Z63_DWC3_CMDACT         (1u << 10) /* DEPCMD poll 0x40C704B0,
                                            * DGCMD  poll 0x40C70608     */

/* All four waits were transcribed from the disassembly into C and run
 * against this file's own MemoryRegionOps, with the stop helpers replaced
 * by counters. Each loop exits on its first read; DEPCMD(0) comes back as
 * 0x106 (command 6, CMDIOC set because RUN_STOP was set, CMDACT clear),
 * DCTL as 0x80000000, and GUSB2PHYCFG0 survives the save/clear/restore
 * dance unchanged. The transcript is in docs/usb-registers.md section 6. */

/* ------------------------------------------------------------------ */
/* Glue-block registers                                                */
/* ------------------------------------------------------------------ */

/* 0x17A20010 / 0x17A60010 bits [17:14]: cleared to bring a port up
 * (sub_40C717EC, 0x40C71814..0x40C7181C), set to park it (sub_40C719E4,
 * 0x40C71AA8..0x40C71AB0; sub_40C71494 does the same to port 1). Which
 * bit is which is NOT established. */
#define Z63_USB_PHY_CTRL        0x010
#define Z63_USB_PHY_PARK_MASK   0x3C000

/* Port 1 is additionally given four tuning words by sub_40C71494
 * (0x40C71494): +0x020 = 0x000FE188, +0x028 = 0x00086800,
 * +0x038 = 0x04C6001A, +0x018 = 0x0000042B. Nothing reads them back and
 * they are unexplained. Port 0 never receives an equivalent. */
#define Z63_USB_PHY_T18         0x018
#define Z63_USB_PHY_T20         0x020
#define Z63_USB_PHY_T28         0x028
#define Z63_USB_PHY_T38         0x038

/* 0x17A40000. Offsets are from the disassembly of sub_40C716E4, where
 * X22 = 0x17A40004, so the decompiler's displacements run +4 low. */
#define Z63_USB_ASSIST_IE       0x004 /* |= 3, two sources   0x40C71750   */
#define Z63_USB_ASSIST_IS       0x008 /* W1C; = 0xFFFF then |= 3
                                       * 0x40C7174C, 0x40C71768;
                                       * the ISR sub_40C717B4 clears
                                       * bits [1:0] at 0x40C717D8        */
#define Z63_USB_ASSIST_CTRL     0x010 /* (old & ~2) | 0x11  0x40C71728;
                                       * bit 4 alone at 0x40C716C8 (on)
                                       * and 0x40C71A84 (off)            */
#define Z63_USB_ASSIST_MODE     0x014 /* (old & ~0x1F) | 0x11 or 0x15
                                       * 0x40C7177C..0x40C71788          */
#define Z63_USB_ASSIST_DIV      0x024 /* = 93750 (0x16E36)  0x40C71720   */
#define Z63_USB_ASSIST_T40      0x040 /* = 53 (0x35)        0x40C71744   */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* One glue window: a flat register file named in the stop message. Same
 * shape as the board's Z63Win and the display header's Z63DispWin, kept
 * separate so this header stays self-contained. */
typedef struct Z63UsbWin {
    const char *name;
    uint64_t base;
    uint32_t *words;
    uint32_t size;
    MemoryRegion region;
} Z63UsbWin;

typedef struct Z63Dwc3 Z63Dwc3;

typedef struct Z63Usb {
    /* 0x17880000..0x1788ffff: the device-mode controller,
     * nikon-z63-dwc3.h. */
    Z63Dwc3 *ctl;

    /* 0x17a00000..0x17a01fff. */
    MemoryRegion exs;
    uint32_t exs_regs[Z63_EXS_SIZE / 4];

    /* 0x17a20000 and 0x17a60000. */
    Z63UsbWin phy[Z63_USB_PHY_INST];
    uint32_t phy_regs[Z63_USB_PHY_INST][Z63_USB_PHY_SIZE / 4];

    /* 0x17a40000..0x17a40fff. */
    MemoryRegion assist;
    uint32_t assist_regs[Z63_USB_ASSIST_SIZE / 4];
    uint32_t assist_is;         /* +0x008 status, write one to clear */
    qemu_irq assist_irq;        /* EXS_USB3_TYPE_C_ASSIST, SPI 109 */

    /* -machine cfexpress=on: a card in the CFexpress slot, behind PCIe
     * port 2 (../z9-qemu's adapter, nikon-z9-pcie.h). */
    bool cfexpress;
    /* Set while the slot's detect says the card has been taken out
     * (-machine cfexpress-card=off, or qom-set at run time to take it out
     * and put it back). Absence is what is stored, so the quiet default
     * stays "a card is in the slot". The card itself -- the NVMe endpoint
     * on the port -- is left where it is: this is the detect line, which
     * is what the firmware's card-detect service polls. */
    bool cfexpress_card_out;
    bool (*zf_pcie2_link_input)(void *opaque);
    void *zf_pcie2_link_opaque;
    bool zf_sd_present; /* tied to the attached SD block device */
} Z63Usb;

/* ------------------------------------------------------------------ */
/* Offset whitelists                                                   */
/*                                                                    */
/* Both indexed blocks are reached through `table[index] + base`, so     */
/* every offset in their tables is physically reachable. The whitelists  */
/* below are narrower on purpose: they admit only what a CALL SITE       */
/* names, so that the first access to a merely-reachable register stops  */
/* with its PC -- which is a lead -- instead of silently reading 0.      */
/* ------------------------------------------------------------------ */

/* 0x17880000. The 80 offsets named by a call site, static or through the
 * four per-endpoint index tables.
 *
 * NOT admitted, deliberately: the entire xHCI host block 0x0000..0x08AC,
 * GSNPSID/GUID/GBUSERRADDR, the GHWPARAMS block, GUCTL2, the 32
 * GTXFIFOSIZ/GRXFIFOSIZ registers, the GTXFIFOPRI / GRXFIFOPRI /
 * GDMAHLRATIO group at 0xC610..0xC624, DEV_IMOD and 0xD060..0xD06C.
 * This RTOS component is a device-mode driver and names none of them.
 * A stop there means either a firmware path this analysis never saw, or
 * the Linux host driver (whose device tree calls the same
 * base socionext,sc2007-xhci) running against this model. */
static bool z63_dwc3_offset_known(hwaddr off)
{
    if (off >= Z63_DWC3_DEP_BASE && off < Z63_DWC3_DEP_END) {
        return true; /* 16 x {PAR2, PAR1, PAR0, DEPCMD} */
    }
    switch (off) {
    case Z63_DWC3_GCTL:
    case Z63_DWC3_GUCTL1:
    case Z63_DWC3_GUCTL:
    case Z63_DWC3_GUSB2PHYCFG:
    case Z63_DWC3_GUSB3PIPECTL:
    case Z63_DWC3_GEVNTADRLO:
    case Z63_DWC3_GEVNTADRHI:
    case Z63_DWC3_GEVNTSIZ:
    case Z63_DWC3_GEVNTCOUNT:
    case Z63_DWC3_DCFG:
    case Z63_DWC3_DCTL:
    case Z63_DWC3_DEVTEN:
    case Z63_DWC3_DSTS:
    case Z63_DWC3_DGCMDPAR:
    case Z63_DWC3_DGCMD:
    case Z63_DWC3_DALEPENA:
        return true;
    default:
        return false;
    }
}

/* 0x17A00000. All 68 entries of the .data offset table at 0xA02808508.
 * The whole table is admitted rather than just the four registers the USB
 * driver names, because the table IS the block's own statement of which
 * offsets exist and because the PCIe driver reaches seven more of them
 * through the same accessor. */
static bool z63_exs_offset_known(hwaddr off)
{
    if (off & 3) {
        return false;
    }
    if (off <= 0x04C) {                      /* idx  0..19 */
        return true;
    }
    if (off == 0x080 || off == 0x084) {      /* idx 20,21  */
        return true;
    }
    if (off == 0x100) {                      /* idx 22     */
        return true;
    }
    if (off >= 0x300 && off <= 0x32C) {      /* idx 23..34 */
        return true;
    }
    if (off == 0x400) {                      /* idx 35     */
        return true;
    }
    if (off >= 0x500 && off <= 0x518) {      /* idx 36..42 */
        return true;
    }
    if (off >= 0x600 && off <= 0x618) {      /* idx 43..49 */
        return true;
    }
    if (off >= 0x700 && off <= 0x70C) {      /* idx 50..53 */
        return true;
    }
    if (off >= 0x800 && off <= 0x80C) {      /* idx 54..57 */
        return true;
    }
    if (off >= Z63_EXS_IRQ_FIRST && off <= Z63_EXS_IRQ_LAST) {
        return true;                         /* idx 58..67 */
    }
    return false;
}

/* 0x17A20000 and 0x17A60000. The union of the two instances' established
 * offsets is used for both.
 *
 * DECLARED CHOICE. Only +0x010 is established on port 0 (sub_40C717EC,
 * sub_40C719E4) and only +0x010/0x018/0x020/0x028/0x038 on port 1
 * (sub_40C71494). The two blocks are the same IP -- they are 0x40000
 * apart, and +0x010 is manipulated with the identical 0x3C000 field on
 * both -- so refusing +0x020 on port 0 would be a false negative. If that
 * turns out to be wrong, split this into two functions. */
static bool z63_usb_phy_offset_known(hwaddr off)
{
    switch (off) {
    case Z63_USB_PHY_CTRL:
    case Z63_USB_PHY_T18:
    case Z63_USB_PHY_T20:
    case Z63_USB_PHY_T28:
    case Z63_USB_PHY_T38:
        return true;
    default:
        return false;
    }
}

/* 0x17A40000. Everything sub_40C716E4, sub_40C7160C, sub_40C719E4 and the
 * ISR sub_40C717B4 touch, and nothing else. */
static bool z63_usb_assist_offset_known(hwaddr off)
{
    switch (off) {
    case Z63_USB_ASSIST_IE:
    case Z63_USB_ASSIST_IS:
    case Z63_USB_ASSIST_CTRL:
    case Z63_USB_ASSIST_MODE:
    case Z63_USB_ASSIST_DIV:
    case Z63_USB_ASSIST_T40:
        return true;
    default:
        return false;
    }
}

/* The DWC3's behaviour -- events, endpoint commands, transfers -- is the
 * controller in nikon-z63-dwc3.h, which keeps this file's offset list
 * (z63_dwc3_offset_known) and its stops. */
#include "hw/arm/nikon-z63-dwc3.h"

/* ------------------------------------------------------------------ */
/* EXS subsystem control at 0x17A00000                                 */
/* ------------------------------------------------------------------ */

static uint64_t z63_exs_read(void *opaque, hwaddr off, unsigned size)
{
    Z63Usb *u = opaque;

    if (size != 4 || (off & 3) || off >= Z63_EXS_SIZE ||
        !z63_exs_offset_known(off)) {
        z63_stop_read("EXS", Z63_EXS_BASE + off, size);
        return 0;
    }

    if (off >= Z63_EXS_IRQ_FIRST && off <= Z63_EXS_IRQ_LAST) {
        /* The ten interrupt-status registers. sub_402BB248 (0x402BB248) is
         * the block's ISR: for each of ten channels it reads
         * 0x17A01000 + 4i, masks bits [1:0] (ANDS at 0x402BB280), writes
         * the word back (0x402BB288) and, if the masked value is non-zero,
         * calls the handler at BSS 0xA02DBF9F0[i].
         *
         * 0 means "no channel pending", which is true: this model raises no
         * interrupt and produces no source. The ISR therefore walks all ten
         * channels and dispatches nothing. */
        return 0;
    }

    if (off == Z63_EXS_SD_DETECT) {
        /* Bit 0 is the SD slot's card-detect input, active low. The card
         * service polls each slot every 50 ms (sub_40327600 ->
         * sub_403276E8); the SD slot's sample is sub_4028DB44, which reads
         * index 3 of the offset table (this word) and stores ~value & 1
         * (MVN / AND #1 at 0x4028db78 / 0x4028db88) as "card present". Its
         * only other user is sub_4028D24C's set-up, a read-modify-write
         * (& 0xfefcfcff | 0x01010200) that carries bit 0 through. As
         * storage the bit read 0 -- a card -- and the camera, finding no
         * card behind the host (nikon-z63-storage.h answers an empty
         * slot), said "This memory card cannot be accessed": the open
         * (sub_402E7878 -> sub_4028DBD0) fails, mount state 2, [Err].
         *
         * DECLARED CHOICE: the slot is empty, the same configuration
         * nikon-z63-storage.h's Present State declares; bit 0 reads 1
         * whatever was written. The CFexpress slot needs nothing: its
         * detect (sub_4028C1CC, index 2 * port + 13 = +0x044, bit 1,
         * active high -- inferred to be detect rather than link because
         * sub_4028C6B0 tests it before powering the card) reads 0. */
        return (u->exs_regs[off / 4] & ~1u) | (u->zf_sd_present ? 0 : 1);
    }

    /* Zf own SDK guard4028fc0c consumes EXS port2 bit1. Reflect the
     * real virtual endpoint/reset/enable predicate, not GPIO_TOP data. */
    if (off == Z63_EXS_PCIE2_STATUS && u->zf_pcie2_link_input) {
        return (u->exs_regs[off / 4] & ~2u) |
               (u->zf_pcie2_link_input(u->zf_pcie2_link_opaque) ? 2u : 0u);
    }

    if (off == Z63_EXS_PCIE2_STATUS && u->cfexpress) {
        /* PCIe port 2, the CFexpress slot: bit 1 is both the slot's detect
         * (sub_4028C1CC, index 2 * port + 13, tested by sub_4028C6B0 before
         * it powers the card) and the link-usable test of the port's
         * accessors (sub_40289B30). With a card attached, DECLARED: the
         * card is there and its link is up -- ../z9-qemu answers the same
         * word 2 (pcie-link-diagnostic-mask), and its wrapper input then
         * reports the trained link (+0xb4 = 0x11) once an endpoint sits on
         * the port's bus.
         *
         * While cfexpress-card is off the bit reads 0, which is the one
         * thing a photographer does that this slot can report: the card
         * taken out. The camera's own card-detect service polls this every
         * 50 ms, needs the sample to hold for two polls, and then runs its
         * remove path -- the volume goes away and the firmware posts its
         * own card-out event. Putting it back reports an insert the same
         * way. Nothing else of the port is touched: the endpoint and its
         * link stay as they are, so a card the firmware re-powers answers
         * again. */
        if (u->cfexpress_card_out) {
            return u->exs_regs[off / 4] & ~2u;
        }
        return u->exs_regs[off / 4] | 2;
    }

    /* Everything else is read-modify-written by its users -- the USB clock
     * and reset gates at 0x500/0x504 (sub_40C7160C 0x40C7160C,
     * sub_40C719E4, sub_40C715A8) and 0x600/0x604 (sub_40C71494), and the
     * PCIe driver's link-status words at 0x034/0x03C/0x044/0x04C
     * (sub_40289B30). Returning the last write is the only answer that
     * does not invent hardware state.
     *
     * Consequence worth knowing: 0x034 and friends read 0, so
     * sub_40289B30's bit-1 test fails and the PCIe accessors return error
     * code 2, "link not usable". With no PCIe device attached that is the
     * truthful answer, but it is a consequence of the zero reset, not a
     * measured one. */
    return u->exs_regs[off / 4];
}

static void z63_exs_write(void *opaque, hwaddr off, uint64_t value,
                          unsigned size)
{
    Z63Usb *u = opaque;

    if (size != 4 || (off & 3) || off >= Z63_EXS_SIZE ||
        !z63_exs_offset_known(off)) {
        z63_stop_write("EXS", Z63_EXS_BASE + off, size, value);
        return;
    }

    if (off >= Z63_EXS_IRQ_FIRST && off <= Z63_EXS_IRQ_LAST) {
        /* Write-1-to-clear against a status that is always 0. Accepted and
         * discarded; the ISR's read-modify-write at 0x402BB278..0x402BB288
         * is then a no-op, which is what it is on real hardware when the
         * channel was idle. */
        return;
    }

    u->exs_regs[off / 4] = (uint32_t)value;
}

static const MemoryRegionOps z63_exs_ops = {
    .read = z63_exs_read,
    .write = z63_exs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* The two PHY glue windows at 0x17A20000 / 0x17A60000                 */
/*                                                                    */
/* Plain register files. Nothing here is polled and nothing is read     */
/* that was not written: sub_40C717EC and sub_40C719E4 read +0x010 only */
/* to modify bits [17:14] of it, and the four port-1 tuning words are   */
/* write-only. So "return the last write" answers every call site.      */
/* ------------------------------------------------------------------ */

static uint64_t z63_usb_phy_read(void *opaque, hwaddr off, unsigned size)
{
    Z63UsbWin *w = opaque;

    if (size != 4 || (off & 3) || off >= w->size ||
        !z63_usb_phy_offset_known(off)) {
        z63_stop_read(w->name, w->base + off, size);
        return 0;
    }
    return w->words[off / 4];
}

static void z63_usb_phy_write(void *opaque, hwaddr off, uint64_t value,
                              unsigned size)
{
    Z63UsbWin *w = opaque;

    if (size != 4 || (off & 3) || off >= w->size ||
        !z63_usb_phy_offset_known(off)) {
        z63_stop_write(w->name, w->base + off, size, value);
        return;
    }
    w->words[off / 4] = (uint32_t)value;
}

static const MemoryRegionOps z63_usb_phy_ops = {
    .read = z63_usb_phy_read,
    .write = z63_usb_phy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* "Type-C assist" at 0x17A40000                                       */
/*                                                                    */
/* The one place in the complex where the firmware waits on something   */
/* this model cannot produce.                                           */
/*                                                                    */
/* sub_40C716E4 (0x40C716E4) programs +0x024, +0x010, +0x040, clears    */
/* +0x008, enables +0x004, and then arms the block by writing +0x014:   */
/*                                                                    */
/*   0x40c7177c  LDR   W8, [X22,#0x10]      ; read  0x17A40014          */
/*   0x40c71784  ORR   W8, W9, W8           ; mode = 0x11 or 0x15       */
/*   0x40c71788  STR   W8, [X22,#0x10]      ; arm                        */
/*   0x40c7178c  MOV   W0, W19  (10)                                     */
/*   0x40c71790  BL    sub_40D33F20         ; delay(10)                  */
/*   0x40c71794  LDRB  W8, [X21]            ; BSS flag 0xA02E7B078       */
/*   0x40c71798  CMP   W8, #1                                            */
/*   0x40c7179c  B.NE  loc_40C7178C         ; NO BOUND                   */
/*                                                                    */
/* The flag is set only by the ISR sub_40C717B4 (0x40C717E4), reached    */
/* through the thunk sub_40C6A32C. There is no register read inside the  */
/* loop, so the arming write is the only place a model can speak.        */
/*                                                                    */
/* This model therefore STOPS on the write to +0x014, with the offending */
/* PC, instead of letting the guest spin forever with no diagnostic.      */
/*                                                                    */
/* To push past it in a bring-up run, the change is the one branch        */
/* below -- label it when you make it. It needs the block's interrupt     */
/* raised after the arming write, with +0x008 bits [1:0] set first. WHICH */
/* LINE THAT IS WAS NOT DETERMINED: either SPI 109                        */
/* (EXS_USB3_TYPE_C_ASSIST, rodata 0x41847DA0) directly, or a channel of  */
/* the 0x17A01000 mux. The handler is installed at runtime and neither    */
/* sub_40C6A32C nor sub_40C717B4 appears as a constant anywhere in the    */
/* image, so the image cannot say. Nor is there any evidence about HOW    */
/* LONG the real block takes, which is the other half of an invented      */
/* answer.                                                               */
/*                                                                    */
/* This path runs only when the gadget stack starts: sub_40C6C7F8 case   */
/* 0x4000004 calls sub_40C7160C then sub_40C716E4 then sub_40C70F14 at   */
/* 0x40C6DAB8/0x40C6DAC0. A run that never selects USB mode never gets   */
/* here.                                                                 */
/* ------------------------------------------------------------------ */

static uint64_t z63_usb_assist_read(void *opaque, hwaddr off, unsigned size)
{
    Z63Usb *u = opaque;

    if (size != 4 || (off & 3) || off >= Z63_USB_ASSIST_SIZE ||
        !z63_usb_assist_offset_known(off)) {
        z63_stop_read("USB-ASSIST", Z63_USB_ASSIST_BASE + off, size);
        return 0;
    }

    if (off == Z63_USB_ASSIST_IS) {
        /* Write-1-to-clear status, two sources, raised by the arming
         * write below. */
        return u->assist_is;
    }

    /* +0x010 and +0x014 are read to be modified (0x40C71728, 0x40C7177C);
     * +0x004 likewise (0x40C71750). +0x024 and +0x040 are never read. */
    return u->assist_regs[off / 4];
}

static void z63_usb_assist_write(void *opaque, hwaddr off, uint64_t value,
                                 unsigned size)
{
    Z63Usb *u = opaque;
    hwaddr abs = Z63_USB_ASSIST_BASE + off;

    if (size != 4 || (off & 3) || off >= Z63_USB_ASSIST_SIZE ||
        !z63_usb_assist_offset_known(off)) {
        z63_stop_write("USB-ASSIST", abs, size, value);
        return;
    }

    if (off == Z63_USB_ASSIST_IS) {
        u->assist_is &= ~(uint32_t)value;
        qemu_set_irq(u->assist_irq,
                     u->assist_is & u->assist_regs[Z63_USB_ASSIST_IE / 4]);
        return;
    }
    u->assist_regs[off / 4] = (uint32_t)value;
    if (off == Z63_USB_ASSIST_MODE && (value & 0x1f)) {
        /* The arming write (0x40C71788, mode 0x11 or 0x15), after which
         * sub_40C716E4 waits, unbounded, for its ISR's flag 0xa02e7b078.
         * The ISR (sub_40C6A32C -> sub_40C717B4, INTID 141 = SPI 109,
         * installed at 0x40C6A30C and 0x40C6BFC4) only acknowledges both
         * sources (+0x008 |= 3) and sets the flag. DECLARED: the block
         * answers its arming at once with source 0, which is all the
         * firmware distinguishes. */
        u->assist_is |= 1;
        qemu_set_irq(u->assist_irq,
                     u->assist_is & u->assist_regs[Z63_USB_ASSIST_IE / 4]);
    }
}

static const MemoryRegionOps z63_usb_assist_ops = {
    .read = z63_usb_assist_read,
    .write = z63_usb_assist_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Reset and wiring                                                    */
/* ------------------------------------------------------------------ */

/* Reset clears every register file. NO NON-ZERO RESET VALUE IS SET
 * ANYWHERE IN THIS MODEL, because none was measured.
 *
 * Where that matters: GCTL, GUCTL1, DCFG, GUSB2PHYCFG, GUSB3PIPECTL,
 * DEVTEN and DALEPENA are all read-modify-written, so the bits the driver
 * preserves come straight from the reset value, and real DWC3 silicon has
 * non-zero resets for at least GCTL (PwrDnScale, U2EXIT_LFPS) and
 * GUSB2PHYCFG (USBTRDTIM, PHYIF). Zero is wrong on hardware; it is
 * consistent with everything THIS firmware can observe, because the driver
 * never tests a preserved bit. docs/usb-registers.md section 5 has the
 * list.
 *
 * Zeroing is also what makes the two forced reads coherent: DSTS reads 0
 * (CONNECTSPD = High-Speed, USBLNKST = U0) and GEVNTCOUNT reads 0, which
 * is the state of a controller whose bring-up has not run. */
static void z63_usb_reset(Z63Usb *u)
{
    unsigned i;

    z63_dwc3_reset(u->ctl);
    memset(u->exs_regs, 0, sizeof(u->exs_regs));
    memset(u->assist_regs, 0, sizeof(u->assist_regs));
    for (i = 0; i < Z63_USB_PHY_INST; i++) {
        memset(u->phy_regs[i], 0, sizeof(u->phy_regs[i]));
    }
}

static void z63_usb_init(Object *owner, MemoryRegion *sysmem, Z63Usb *u)
{
    static const char *const phy_names[Z63_USB_PHY_INST] = {
        "USB-PHY0", "USB-PHY1",
    };
    static const uint64_t phy_bases[Z63_USB_PHY_INST] = {
        Z63_USB_PHY0_BASE, Z63_USB_PHY1_BASE,
    };
    unsigned i;

    u->ctl = g_new0(Z63Dwc3, 1);
    z63_usb_reset(u);

    /* 0x17880000..0x1788ffff, one region, so that an offset the driver has
     * never named still reaches this model and stops with the block name
     * rather than landing in the unassigned pool. */
    memory_region_init_io(&u->ctl->iomem, owner, &z63_dwc3_dev_ops, u->ctl,
                          "z63-usb-dwc3", Z63_DWC3_SIZE);
    memory_region_add_subregion(sysmem, Z63_DWC3_BASE, &u->ctl->iomem);

    /* 0x17a00000..0x17a01fff. Shared with the PCIe driver -- see the
     * comment on Z63_EXS_BASE. */
    memory_region_init_io(&u->exs, owner, &z63_exs_ops, u,
                          "z63-usb-exs", Z63_EXS_SIZE);
    memory_region_add_subregion(sysmem, Z63_EXS_BASE, &u->exs);

    /* The two PHY glue windows. One subregion each, so the stop message
     * names the port. */
    for (i = 0; i < Z63_USB_PHY_INST; i++) {
        u->phy[i].name = phy_names[i];
        u->phy[i].base = phy_bases[i];
        u->phy[i].size = Z63_USB_PHY_SIZE;
        u->phy[i].words = u->phy_regs[i];
        memory_region_init_io(&u->phy[i].region, owner, &z63_usb_phy_ops,
                              &u->phy[i], u->phy[i].name, u->phy[i].size);
        memory_region_add_subregion(sysmem, u->phy[i].base,
                                    &u->phy[i].region);
    }

    /* 0x17a40000..0x17a40fff. */
    memory_region_init_io(&u->assist, owner, &z63_usb_assist_ops, u,
                          "z63-usb-assist", Z63_USB_ASSIST_SIZE);
    memory_region_add_subregion(sysmem, Z63_USB_ASSIST_BASE, &u->assist);
}

#endif /* HW_ARM_NIKON_Z63_USB_H */

/* ------------------------------------------------------------------ */
/* How to wire this into hw/arm/nikon-z63.c                            */
/*                                                                    */
/* Three edits, located by text anchor rather than line number because  */
/* other models are landing in the same file.                           */
/*                                                                    */
/* Nothing in the board moves and no existing region is disturbed: the  */
/* four spans used here (0x17880000+0x10000, 0x17a00000+0x2000,         */
/* 0x17a20000+0x1000, 0x17a40000+0x1000, 0x17a60000+0x1000) do not      */
/* overlap hw/arm/nikon-z63-storage.h's 0x17820000+0x300,               */
/* 0x17828000+0x200 and 0x17828200+0x200, nor anything else the board   */
/* already maps.                                                        */
/*                                                                    */
/* 1. After the last #include -- the line                               */
/*                                                                    */
/*        #include <libfdt.h>                                          */
/*                                                                    */
/*    and after any other nikon-z63-*.h includes already there -- add:  */
/*                                                                    */
/*        #include "hw/arm/nikon-z63-usb.h"                            */
/*                                                                    */
/*    It has to come before NikonZ63State so the state field below      */
/*    resolves; the board's z63_stop_read / z63_stop_write are          */
/*    forward-declared inside this header for exactly that reason, under  */
/*    the shared Z63_STOP_HELPERS_DECLARED guard that                   */
/*    hw/arm/nikon-z63-display.h also uses.                             */
/*                                                                    */
/* 2. In `typedef struct NikonZ63State { ... }`, on the line after      */
/*                                                                    */
/*        Z63Display display;        /+ display complex, 0x1548C000 +/  */
/*                                                                    */
/*    (or after `Z63StorageState storage;` if the display model is not  */
/*    present), add:                                                    */
/*                                                                    */
/*        Z63Usb usb;                /+ USB complex, 0x17880000 +/      */
/*                                                                    */
/* 3. In machine init, on the line after                                */
/*                                                                    */
/*        z63_display_init(OBJECT(machine), sysmem, &s->display);       */
/*                                                                    */
/*    (or next to any other *_init call in that group), add:            */
/*                                                                    */
/*        /+ USB complex: the DWC3 at 0x17880000 in device mode, the    */
/*         + shared EXS control block and the three glue files. See     */
/*         + docs/usb-registers.md. +/                                  */
/*        z63_usb_init(OBJECT(machine), sysmem, &s->usb);               */
/*                                                                    */
/*    (replace `/+ ... +/` with a real comment; written this way so     */
/*     this note does not close the comment it lives in)                */
/*                                                                    */
/* z63_usb_init() calls z63_usb_reset() itself, so there is no fourth   */
/* edit unless the board later grows a machine-level reset hook that    */
/* should also re-zero these files; in that case call                   */
/* z63_usb_reset(&s->usb) from it.                                      */
/*                                                                    */
/* ------------------------------------------------------------------ */
/* WHAT TO EXPECT ON THE FIRST RUN                                     */
/* ------------------------------------------------------------------ */
/*                                                                    */
/* Nothing at all until something starts the USB gadget stack. The      */
/* complex is quiet at boot: the 0x17A00000 ISR (sub_402BB248) finds no  */
/* channel pending, and no other path touches these windows.            */
/*                                                                    */
/* When the gadget does start (sub_40C6C7F8 case 0x4000004 at            */
/* 0x40C6DAB8), the order is:                                           */
/*                                                                    */
/*   sub_40C7160C   EXS 0x500/0x504 clock and reset gates, then          */
/*                  0x17A40010 |= 0x10            -- all accepted        */
/*   sub_40C716E4   0x17A40024/0x10/0x40/0x08/0x04 -- all accepted,       */
/*                  then the arming write to 0x17A40014 -- STOPS here,   */
/*                  reported as USB-ASSIST-ARM with PC 0x40C71788        */
/*                                                                    */
/* That stop is the expected result and the PC in the log is the one     */
/* named here. Past it -- if the interrupt is ever wired -- sub_40C70F14 */
/* runs the DWC3 bring-up: GCTL CoreSoftReset pulse, DCTL CSFTRST and    */
/* its poll, GUCTL, the event buffer registers, GCTL PrtCapDir=Device,   */
/* GUCTL1, DCFG, DEVTEN, then DEPSTARTCFG and two SETTRANSFRESOURCE      */
/* commands on physical endpoints 0 and 1. All of that is answered by    */
/* this model; the next stop would be whatever the enumeration path      */
/* reaches, and with GEVNTCOUNT reading 0 it never gets an event to      */
/* enumerate against.                                                   */
/* ------------------------------------------------------------------ */
