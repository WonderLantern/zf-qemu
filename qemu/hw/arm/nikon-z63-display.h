/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Nikon Z 6III (EXPEED 7) research machine -- display band, 0x15400000+.
 *
 * Reversed from the decrypted factory RTOS image eg2010_0200i1.bi
 * (sha256 736ceaac...1c47fda). Every behaviour below cites the firmware VA
 * that establishes it; file offset = VA - 0x40160000. The full recovered
 * register map, including the parts this file deliberately refuses, is in
 * docs/display-registers.md -- read that before extending this.
 *
 * WHAT THE BAND IS
 *
 * The SoC is a Socionext Milbeaut-family part: its interrupt-source name
 * table at rodata 0x41847000 (0x20 bytes per entry) names DISP_0, DISP_1,
 * DISP_1B, DISP_DWCH, HDMI_MASTER, HDMI_CEC, MIPI_DSI_TX, RSZ, MPO,
 * IDMAC0_0..IDMAC2_1, RDMA_0..2, YUV_COMP/DECOMP alongside PERI_XDMAC_n,
 * IPCU_n, NOC_MAIN, SDRAMC_n and EXS_UHS2_1_F_SDH30/40.
 *
 * Three groups of the band are modelled here:
 *
 *   0x1548C000..0x15492FFF  seven 4 KiB pages. Page 1 (0x1548D000) is a
 *                           MIPI DSI host; the other six are small control
 *                           pages that were fully enumerated.
 *   0x15480000..0x15487FFF  the two DISP units' own register windows, four
 *                           0x1000 sub-blocks each.
 *   0x15496000..0x15498FFF  399 registers behind one driver. NOT reversed;
 *                           every access stops with its offset and PC.
 *
 * The two display units are reached only through pointers in .data, which is
 * why a search for the constant 0x15480000 in this image finds almost
 * nothing. The initialised descriptors are at 0xA02808438 (id 0x1C7,
 * windows 0x15480000 / 0x15490800 / 0x15C80000), 0xA02808470 (id 0x1C8,
 * 0x15484000 / 0x15490900 / 0x15C84000) and 0xA028084A8 (id 0x1C9, all
 * windows NULL). Loaded at 0x402b0600 and 0x402b0614 / 0x402b0628.
 *
 * NOT modelled, and not mapped: 0x15440000 (driver 0x40c25340), 0x154C0000
 * (0x40c2f7d0), 0x15648000/0x15660000 (0x40c71b80, base table 0x4185B620).
 * They fall through to the board's z63_unmapped trap, which already reports
 * the address and the PC. 0x155C0000 / 0x15600000 / 0x15680000 are inside
 * the band numerically but are the main DMA banks the board already models
 * (z63_dma_bases); nothing here touches them.
 *
 * No display timing, panel geometry or pixel format is asserted anywhere in
 * this file. The DSI host's eleven VID_* timing registers are written
 * entirely from a caller-supplied struct (0x402b48fc) and this image
 * contains no initialised instance of it, so none of those values can be
 * recovered from the firmware and none is invented here.
 *
 * This header must be included by hw/arm/nikon-z63.c; see the wiring note at
 * the end. It calls the board's z63_stop_read / z63_stop_write so that an
 * access it cannot justify stops the VM with the offending PC rather than
 * returning an invented value.
 */
#ifndef HW_ARM_NIKON_Z63_DISPLAY_H
#define HW_ARM_NIKON_Z63_DISPLAY_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "system/memory.h"

/* Defined by the board, below the device models. Forward-declared so this
 * header can be included before NikonZ63State is defined.
 *
 * hw/arm/nikon-z63-storage.h declares the same two helpers for the same
 * reason. Two identical static declarations in one translation unit are
 * valid C and clang does not implement -Wredundant-decls, but GCC does, so
 * the pair is guarded: give the other header the same guard macro and the
 * include order stops mattering. */
#ifndef Z63_STOP_HELPERS_DECLARED
#define Z63_STOP_HELPERS_DECLARED
static void z63_stop_read(const char *block, hwaddr addr, unsigned size);
static void z63_stop_write(const char *block, hwaddr addr, unsigned size,
                           uint64_t value);
#endif

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* The DSI/display control complex: seven contiguous 4 KiB pages. The page
 * bases are the band's own statement of its layout -- the dispatcher at
 * 0x402b64bc selects one of 18 sub-blocks through the byte jump table at
 * rodata 0x4184B354 and then reads base+offset at 0x402b65f4:
 *
 *   idx  0..3   window0 + n*0x1000     idx 8      0x15491000
 *   idx  4      0x1548C000             idx 9      0x15492000
 *   idx  5      0x1548D000             idx 10..13 0x15490000 + n*0x100
 *   idx  6      0x1548E000             idx 14..17 window2 + n*0x1000
 *   idx  7      0x1548F000
 */
#define Z63_DISP_CPLX_BASE      0x1548C000ULL
#define Z63_DISP_CPLX_PAGES     7
#define Z63_DISP_CPLX_SIZE      (Z63_DISP_CPLX_PAGES * 0x1000)

#define Z63_DISP_PAGE_SEL       0 /* 0x1548C000 */
#define Z63_DISP_PAGE_DSI       1 /* 0x1548D000 -- MIPI DSI host */
#define Z63_DISP_PAGE_E         2 /* 0x1548E000 */
#define Z63_DISP_PAGE_F         3 /* 0x1548F000 */
#define Z63_DISP_PAGE_PWR       4 /* 0x15490000 -- four 0x100 sub-blocks */
#define Z63_DISP_PAGE_U0        5 /* 0x15491000 */
#define Z63_DISP_PAGE_U1        6 /* 0x15492000 -- identical to 0x15491000 */

/* Byte offset of the DSI page inside the complex. */
#define Z63_DSI_PAGE_OFF        (Z63_DISP_PAGE_DSI * 0x1000)

/* The two DISP units' own windows, 0x4000 apart and contiguous. The span is
 * established by the sub-block writes in 0x402b0528 / 0x402b0c70 (+0x0000,
 * +0x1000, +0x2000, +0x3000 of window 0) and the descriptors' two bases
 * 0x15480000 and 0x15484000. */
#define Z63_DISP_UNIT_BASE      0x15480000ULL
#define Z63_DISP_UNIT_STRIDE    0x4000
#define Z63_DISP_UNIT_INST      2

/* Window 2 of each unit, from the same descriptors. Four 0x1000 sub-blocks
 * (dispatcher indices 14..17). NOT ONE OFFSET IN IT IS ESTABLISHED: nothing
 * writes it through a constant and its only reader is the generic
 * dispatcher. It is also outside the 0x15400000..0x15700000 band this work
 * covers. Mapped only so that a stop names the block instead of landing in
 * the unassigned pool. */
#define Z63_DISP_WIN2_BASE      0x15C80000ULL

/* 0x15496000 / 0x15497000 / 0x15498000: one driver, 0x402b6d14..0x402bb100,
 * 399 distinct 32-bit registers (37 + 343 + 19). Not reversed -- see
 * docs/display-registers.md section 5 for the complete offset census, the
 * read-only list, and why this refuses rather than storing writes. */
#define Z63_DISP_TXQ_BASE       0x15496000ULL
#define Z63_DISP_TXQ_SIZE       0x3000

/* ------------------------------------------------------------------ */
/* MIPI DSI host at 0x1548D000                                         */
/*                                                                    */
/* Identification: a Synopsys DesignWare MIPI DSI Host, v1.3x register   */
/* map. This is not inferred from the block's neighbourhood. 0x402b48fc  */
/* writes eleven consecutive registers whose field widths are the DWC     */
/* video-mode set exactly -- 0x03c [13:0], 0x040 [12:0], 0x044 [12:0],   */
/* 0x048 [11:0], 0x04c [11:0], 0x050 [14:0], 0x054 [9:0], 0x058 [9:0],   */
/* 0x05c [9:0], 0x060 [13:0] -- and 0x402b4b90 writes 0x098/0x09c as     */
/* [9:0] + [25:16], the DWC PHY timing pair. On top of that, PHY_STATUS   */
/* has the DWC stopstate/ulpsactivenot bit assignment (see below),        */
/* PHY_ULPS_CTRL takes the DWC 4-bit request/exit encoding, PHY_IF_CFG    */
/* [1:0] is the lane count that indexes those bits, and 0x402b4ec0 reads  */
/* back the *_ACT shadow block at live+0x100. The interrupt-source table   */
/* calls it MIPI_DSI_TX (rodata 0x41847920).                              */
/*                                                                    */
/* The DWC names are used below as a reading aid. The bit positions and   */
/* widths are measured; the names are the identification's conclusion.    */
/* ------------------------------------------------------------------ */

#define Z63_DSI_VERSION         0x000 /* read at 0x402b4ed4, never compared */
#define Z63_DSI_PWR_UP          0x004 /* bit0, 0x402b475c                   */
#define Z63_DSI_CLKMGR_CFG      0x008
#define Z63_DSI_DPI_VCID        0x00c
#define Z63_DSI_DPI_COLOR_CODING 0x010
#define Z63_DSI_DPI_CFG_POL     0x014
#define Z63_DSI_DPI_LP_CMD_TIM  0x018
#define Z63_DSI_PCKHDL_CFG      0x02c
#define Z63_DSI_MODE_CFG        0x034
#define Z63_DSI_VID_MODE_CFG    0x038
#define Z63_DSI_VID_PKT_SIZE    0x03c
#define Z63_DSI_VID_NUM_CHUNKS  0x040
#define Z63_DSI_VID_NULL_SIZE   0x044
#define Z63_DSI_VID_HSA_TIME    0x048
#define Z63_DSI_VID_HBP_TIME    0x04c
#define Z63_DSI_VID_HLINE_TIME  0x050
#define Z63_DSI_VID_VSA_LINES   0x054
#define Z63_DSI_VID_VBP_LINES   0x058
#define Z63_DSI_VID_VFP_LINES   0x05c
#define Z63_DSI_VID_VACTIVE     0x060
#define Z63_DSI_CMD_MODE_CFG    0x068
#define Z63_DSI_GEN_HDR         0x06c
#define Z63_DSI_GEN_PLD_DATA    0x070 /* written at 0x402b4b54, never read  */
#define Z63_DSI_CMD_PKT_STATUS  0x074 /* read at 0x402b4edc, never compared */
#define Z63_DSI_SDF_3D          0x090
#define Z63_DSI_LPCLK_CTRL      0x094
#define Z63_DSI_PHY_TMR_LPCLK   0x098
#define Z63_DSI_PHY_TMR_CFG     0x09c
#define Z63_DSI_PHY_RSTZ        0x0a0
#define Z63_DSI_PHY_IF_CFG      0x0a4
#define Z63_DSI_PHY_ULPS_CTRL   0x0a8
#define Z63_DSI_PHY_TX_TRIGGERS 0x0ac
#define Z63_DSI_PHY_STATUS      0x0b0
#define Z63_DSI_INT_ST1         0x0c0 /* read at 0x402b063c, reported out   */
#define Z63_DSI_INT_MSK1        0x0c8 /* written at 0x402b475c, never read  */
#define Z63_DSI_VID_SHADOW_CTRL 0x100
/* The shadow readbacks 0x140..0x160 and 0x190, all read-only (0x402b4ec0). */
#define Z63_DSI_ACT_FIRST       0x140
#define Z63_DSI_ACT_LAST        0x160
#define Z63_DSI_SDF_3D_ACT      0x190
/* Vendor registers outside the DWC map, written from the same timing struct
 * as PHY_TMR_LPCLK/PHY_TMR_CFG at 0x402b4b90: nine 6-bit fields. What they
 * configure is NOT established and nothing here claims it. */
#define Z63_DSI_VND_TMR0        0x400
#define Z63_DSI_VND_TMR1        0x404
#define Z63_DSI_VND_TMR2        0x408

#define Z63_DSI_SHADOW_REQ      (1u << 8) /* 0x100 bit 8, see the latch      */

/* PHY_STATUS values, indexed by PHY_IF_CFG[1:0]. This table is read
 * verbatim out of the firmware's own rodata at 0x4184B3A0:
 *
 *   3c 00  bc 01  bc 07  bc 1f   ->  0x003c 0x01bc 0x07bc 0x1fbc
 *
 * and it is the mask the ULPS-entry loop at 0x402b4df0 waits to see clear.
 * The union 0x1fbc is bits 2,3,4,5,7,8,9,10,11,12, which group as the five
 * pairs {2,3} {4,5} {7,8} {9,10} {11,12} -- the DWC stopstate /
 * ulpsactivenot pair for the clock lane and up to four data lanes, with the
 * index selecting how many pairs are in play.
 *
 * Three further tables gate the same register, and all three are subsets of
 * this one, which is why one value satisfies every call site:
 *
 *   0x4184B3E8  the HS-clock-enable gate at 0x402b4cc0 requires
 *               0x1fbc & ~tbl[n] == this table's entry
 *   0x4184B3E0  the HS-clock-disable gate at 0x402b4d2c requires
 *               0xa90 & ~tbl[n] = 0x0010/0x0090/0x0290/0x0a90
 *   0x4184B3F0  the ULPS-exit loop at 0x402b4e98 waits for
 *               0x1528 & ~tbl[n] = 0x0028/0x0128/0x0528/0x1528
 */
static const uint16_t z63_dsi_phy_stop_ulps[4] = {
    0x003c, 0x01bc, 0x07bc, 0x1fbc,
};

/* The ten live -> shadow pairs the firmware reads back (0x402b4ec0). The
 * uniform live+0x100 offset and the matching field widths are what pair
 * them; THE FIRMWARE NEVER COMPARES AN ACT VALUE AGAINST WHAT IT WROTE, so
 * the pairing is an inference. Returning 0 for all ten would be equally
 * consistent with the image -- the mirror is implemented because it is more
 * useful and this firmware cannot observe it to be wrong. */
static const uint16_t z63_dsi_shadow[10][2] = {
    { 0x040, 0x140 }, { 0x044, 0x144 }, { 0x048, 0x148 }, { 0x04c, 0x14c },
    { 0x050, 0x150 }, { 0x054, 0x154 }, { 0x058, 0x158 }, { 0x05c, 0x15c },
    { 0x060, 0x160 }, { 0x090, 0x190 },
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* One DISP unit window, or one window-2 placeholder: a flat register file
 * named in the stop message. Same shape as the board's Z63Win, kept
 * separate so this header stays self-contained. */
typedef struct Z63DispWin {
    const char *name;
    uint64_t base;
    uint32_t *words;
    uint32_t size;
    MemoryRegion region;
    qemu_irq irq;               /* unit windows: DISP_0 / DISP_1 */
    qemu_irq irq1;              /* Zf unit1 output1: INTID101 */
} Z63DispWin;

typedef struct Z63Display {
    /* 0x1548c000..0x15492fff, one region, dispatched per page. */
    MemoryRegion cplx;
    uint32_t cplx_regs[Z63_DISP_CPLX_SIZE / 4];

    /* 0x15480000 and 0x15484000. */
    Z63DispWin unit[Z63_DISP_UNIT_INST];
    uint32_t unit_regs[Z63_DISP_UNIT_INST][Z63_DISP_UNIT_STRIDE / 4];

    /* 0x15496000..0x15498fff and 0x15c80000..0x15c87fff: refuse-only, no
     * backing store. refuse[] carries only the name and base that the stop
     * message needs. */
    MemoryRegion txq;
    MemoryRegion win2;
    Z63DispWin refuse[2];
} Z63Display;

/* ------------------------------------------------------------------ */
/* Offset whitelists                                                   */
/*                                                                    */
/* These are not complete and cannot be. 0x402b64bc and 0x402b4f38 both   */
/* read `base + runtime offset`, so an offset absent below is still a      */
/* real register that has simply never been named by an immediate. The     */
/* whitelists are here so that the first access to one of those offsets   */
/* stops with its PC, which is a lead, instead of silently reading 0.      */
/* ------------------------------------------------------------------ */

/* 0x1548D000. Every offset the 325 band functions name. */
static bool z63_dsi_offset_known(hwaddr off)
{
    switch (off) {
    case 0x000: case 0x004: case 0x008: case 0x00c:
    case 0x010: case 0x014: case 0x018:
    case 0x02c: case 0x034: case 0x038: case 0x03c:
    case 0x040: case 0x044: case 0x048: case 0x04c:
    case 0x050: case 0x054: case 0x058: case 0x05c: case 0x060:
    case 0x068: case 0x06c: case 0x070: case 0x074:
    case 0x090: case 0x094: case 0x098: case 0x09c:
    case 0x0a0: case 0x0a4: case 0x0a8: case 0x0ac: case 0x0b0:
    case 0x0c0: case 0x0c8:
    case 0x100:
    case 0x140: case 0x144: case 0x148: case 0x14c:
    case 0x150: case 0x154: case 0x158: case 0x15c: case 0x160:
    case 0x190:
    case 0x400: case 0x404: case 0x408:
        return true;
    default:
        return false;
    }
}

static bool z63_dsi_read_only(hwaddr off)
{
    return off == Z63_DSI_VERSION || off == Z63_DSI_CMD_PKT_STATUS ||
           off == Z63_DSI_PHY_STATUS || off == Z63_DSI_INT_ST1 ||
           off == Z63_DSI_SDF_3D_ACT ||
           (off >= Z63_DSI_ACT_FIRST && off <= Z63_DSI_ACT_LAST &&
            !(off & 3));
}

/* The six control pages of the complex. */
static bool z63_disp_cplx_known(unsigned page, hwaddr off)
{
    switch (page) {
    case Z63_DISP_PAGE_SEL:
        /* 0x1548C000: one register, three 2-bit fields at [1:0] [5:4] [9:8],
         * written only, at 0x402b4688. */
        return off == 0x000;

    case Z63_DISP_PAGE_E:
        /* 0x1548E000: 0x402b4f58/0x402b4f98 (+0x000, +0x008),
         * 0x402b5060/0x402b503c (+0x004), 0x402b0528/0x402b0c70/0x402b4fd0
         * (+0x100 gate, +0x300), 0x402b5080/0x402b50a0 (+0x104),
         * 0x402b50bc/0x402b5108 (+0x200). */
        return off == 0x000 || off == 0x004 || off == 0x008 ||
               off == 0x100 || off == 0x104 || off == 0x200 || off == 0x300;

    case Z63_DISP_PAGE_F:
        /* 0x1548F000: +0x000 gate (0x402b0300/0x402b0528/0x402b0c70),
         * +0x100 (0x402b5148/0x402b5194), +0x200/+0x204
         * (0x402b51d4/0x402b5214). */
        return off == 0x000 || off == 0x100 ||
               off == 0x200 || off == 0x204;

    case Z63_DISP_PAGE_PWR:
        /* 0x15490000: four 0x100 sub-blocks at +0x000/+0x100/+0x200/+0x300
         * (dispatcher indices 10..13) plus two more at +0x800/+0x900 reached
         * through each unit descriptor's window-1 pointer. The accessors
         * come in pairs that pick one of two sub-blocks from a boolean
         * (0x402b60c0: `v4 = 0x15490280; if (a2) v4 = 0x15490380`) and the
         * two pairs have different register sets, so a per-offset whitelist
         * would have to guess which sub-block is which kind. Accepted as a
         * span instead; the rest of the page still stops. */
        return off < 0x400 || (off >= 0x800 && off < 0xa00);

    case Z63_DISP_PAGE_U0:
    case Z63_DISP_PAGE_U1:
        /* 0x15491000 and 0x15492000 are two identical instances.
         * +0x000 gate (0x402b0528/0x402b0c70); +0x100/+0x104
         * (0x402b5250/0x402b5298, 0x402b52d4/0x402b52f4); +0x200
         * (0x402b5310/0x402b5340); +0x300/+0x304/+0x308
         * (0x402b5398/0x402b53dc, 0x402b5420); and a 10 x 16-byte table at
         * +0x400..+0x49f written by the loop at 0x402b5454 (four 7-bit
         * values per word, mask 0x80808080 preserved). */
        return off == 0x000 || off == 0x100 || off == 0x104 ||
               off == 0x200 || off == 0x300 || off == 0x304 ||
               off == 0x308 || (off >= 0x400 && off < 0x4a0);

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* MIPI DSI host behaviour                                             */
/* ------------------------------------------------------------------ */

/* PHY_STATUS (0x0b0). The one register in this band a model must get right:
 * two call sites spin on it with no timeout.
 *
 *   0x402b4df0  while (PHY_STATUS & z63_dsi_phy_stop_ulps[n])        ;
 *               entered after PHY_ULPS_CTRL = 5 at 0x402b4dec
 *   0x402b4e98  while (~(tbl_3F0[n] | PHY_STATUS) & 0x1528)          ;
 *               entered after PHY_ULPS_CTRL = 15 at 0x402b4e94
 *
 * and two more that return busy (2) rather than looping:
 *
 *   0x402b4ccc  HS clock may be enabled only if PHY_STATUS contains every
 *               bit of z63_dsi_phy_stop_ulps[n]
 *   0x402b4d38  HS clock may be disabled only if PHY_STATUS contains the
 *               data lanes' stopstate bits 0x0010/0x0090/0x0290/0x0a90
 *
 * The value below satisfies all four, because the gate masks of the latter
 * three are subsets of z63_dsi_phy_stop_ulps[n]. Its inputs are the three
 * registers the firmware itself reads next to PHY_STATUS at those sites.
 *
 * Two labelled choices, neither measured:
 *
 *  - Only PHY_RSTZ bit 0 is an input. The firmware clears it before PHY
 *    reconfiguration and sets it after (0x402b475c, 0x402b47e4, 0x402b4808)
 *    and never writes bits 1..3, so nothing in the image says what those
 *    would do.
 *  - PHY_ULPS_CTRL is only ever written 0, 5 or 15 (0x402b4dec, 0x402b4e94,
 *    0x402b4eb4). "in ULPS iff the value is 5" reproduces exactly those
 *    three cases; any other value is an extrapolation.
 *
 * Bits 0 (phylock), 1 (direction) and 6 appear in none of the four firmware
 * masks, so this image never observes them and no value for them is
 * asserted here -- they read 0 because nothing establishes otherwise.
 */
static uint32_t z63_dsi_phy_status(const uint32_t *dsi)
{
    if (!(dsi[Z63_DSI_PHY_RSTZ / 4] & 1)) {
        return 0;
    }
    if (dsi[Z63_DSI_PHY_ULPS_CTRL / 4] == 5) {
        return 0;
    }
    return z63_dsi_phy_stop_ulps[dsi[Z63_DSI_PHY_IF_CFG / 4] & 3];
}

/* VID_SHADOW_CTRL (0x100) bit 8 is a one-shot request the hardware clears.
 * 0x402b493c reads the register, 0x402b4944 tests bit 8 and jumps to
 * 0x402b4a3c, which returns 4 -- so if the bit is ever still set when the
 * firmware looks, every shadow commit after the first fails. The commit path
 * sets bits 0 and 8 and writes the register at 0x402b4a6c.
 *
 * Latching live -> ACT here is what makes the readbacks at 0x402b4ec0 return
 * something other than zero; see z63_dsi_shadow for why that pairing is an
 * inference rather than a measurement. */
static void z63_dsi_shadow_latch(uint32_t *dsi)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(z63_dsi_shadow); i++) {
        dsi[z63_dsi_shadow[i][1] / 4] = dsi[z63_dsi_shadow[i][0] / 4];
    }
}

static uint64_t z63_dsi_read(Z63Display *d, hwaddr off, unsigned size)
{
    uint32_t *dsi = &d->cplx_regs[Z63_DSI_PAGE_OFF / 4];

    switch (off) {
    case Z63_DSI_PHY_STATUS:
        return z63_dsi_phy_status(dsi);

    /* Three read-only registers the firmware reads but never compares
     * against anything: 0x402b4ed4 splits 0x000 into a byte and a halfword
     * and stores both in a status struct; 0x402b4edc stores 0x074 in the
     * same struct; 0x402b063c hands 0x0c0 to a callback as message
     * 0x41030007. Reading 0 is a declared choice, like the board's
     * soc-revision: this image cannot tell the difference, and the real
     * values (a DWC design/revision/product id, a command-FIFO status and
     * an interrupt status) are unmeasured. No interrupt is modelled. */
    case Z63_DSI_VERSION:
    case Z63_DSI_CMD_PKT_STATUS:
    case Z63_DSI_INT_ST1:
        return 0;

    default:
        if (!z63_dsi_offset_known(off)) {
            z63_stop_read("DSI", Z63_DISP_CPLX_BASE + Z63_DSI_PAGE_OFF + off,
                          size);
            return 0;
        }
        /* GEN_PLD_DATA (0x070) is a payload FIFO on real hardware; the
         * firmware only ever writes it (0x402b4b54) so nothing here depends
         * on the read, and returning the last word written is the smallest
         * answer that is not invented. */
        return dsi[off / 4];
    }
}

static void z63_dsi_write(Z63Display *d, hwaddr off, uint64_t value,
                          unsigned size)
{
    uint32_t *dsi = &d->cplx_regs[Z63_DSI_PAGE_OFF / 4];
    hwaddr abs = Z63_DISP_CPLX_BASE + Z63_DSI_PAGE_OFF + off;

    if (!z63_dsi_offset_known(off)) {
        z63_stop_write("DSI", abs, size, value);
        return;
    }
    if (z63_dsi_read_only(off)) {
        /* The firmware never writes these. A write means either a new
         * firmware path or a wrong model, and both are worth a stop. */
        z63_stop_write("DSI-RO", abs, size, value);
        return;
    }

    if (off == Z63_DSI_VID_SHADOW_CTRL) {
        if (value & Z63_DSI_SHADOW_REQ) {
            z63_dsi_shadow_latch(dsi);
        }
        dsi[off / 4] = (uint32_t)value & ~Z63_DSI_SHADOW_REQ;
        return;
    }

    dsi[off / 4] = value;
}

/* ------------------------------------------------------------------ */
/* The complex: 0x1548c000..0x15492fff                                 */
/* ------------------------------------------------------------------ */

/*
 * Start, stop and clear. One driver routine of each writes a command into
 * the low two bits of a set of control words -- start 0x402b0868 writes 1
 * (0x11 with a flag on +0x1200/+0x1280, bit 8 from its last argument), stop
 * 0x402b0928 writes 2 (`&= ~3, |= 2` on +0x1200/+0x1280), clear 0x402b0a24
 * writes 0 -- to unit +0x100, +0x1200, +0x1280, +0x2100, +0x3100 and to
 * 0x15490000 + n*0x100. The status query 0x402b0b1c reads the same words
 * back and answers `(word & 3) == 3`. So a started block reads 3 in those
 * bits: the hardware moves 1 (start) to 3 (running).
 *
 * DECLARED CHOICE, not measured: the move is immediate. ../z9-qemu
 * (engine1548_control_write, WonderLantern, GPL-2.0-or-later) makes the
 * same choice for the same words on the Z 9, citing the same three
 * routines and query in that image, and says so; nothing here knows how
 * long the real block takes. Stop (2) and clear (0) read back as written.
 */
static inline uint64_t z63_disp_command(uint64_t value)
{
    return (value & 3) == 1 ? value | 3 : value;
}

static inline bool z63_disp_is_command(hwaddr off)
{
    return off == 0x100 || off == 0x1200 || off == 0x1280 ||
           off == 0x2100 || off == 0x3100;
}

static uint64_t z63_disp_cplx_read(void *opaque, hwaddr addr, unsigned size)
{
    Z63Display *d = opaque;
    unsigned page;
    hwaddr off;

    if (size != 4 || (addr & 3) || addr >= Z63_DISP_CPLX_SIZE) {
        z63_stop_read("DISP-CPLX", Z63_DISP_CPLX_BASE + addr, size);
        return 0;
    }
    page = addr >> 12;
    off = addr & 0xfff;

    if (page == Z63_DISP_PAGE_DSI) {
        return z63_dsi_read(d, off, size);
    }
    if (!z63_disp_cplx_known(page, off)) {
        z63_stop_read("DISP-CPLX", Z63_DISP_CPLX_BASE + addr, size);
        return 0;
    }
    /* Reads of an enumerated offset that has never been written return 0.
     * That is NOT a measured reset value -- 0x15490220 and 0x15490320, for
     * instance, are only ever read (0x402b65fc assembles them into one
     * 32-bit value) and their real reset state is unknown. */
    return d->cplx_regs[addr / 4];
}

static void z63_disp_cplx_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    Z63Display *d = opaque;
    unsigned page;
    hwaddr off;

    if (size != 4 || (addr & 3) || addr >= Z63_DISP_CPLX_SIZE) {
        z63_stop_write("DISP-CPLX", Z63_DISP_CPLX_BASE + addr, size, value);
        return;
    }
    page = addr >> 12;
    off = addr & 0xfff;

    if (page == Z63_DISP_PAGE_DSI) {
        z63_dsi_write(d, off, value, size);
        return;
    }
    if (!z63_disp_cplx_known(page, off)) {
        z63_stop_write("DISP-CPLX", Z63_DISP_CPLX_BASE + addr, size, value);
        return;
    }

    /* 0x1548E004 is the only latching register in the six control pages.
     * 0x402b5060 writes the literal 1; 0x402b503c is a getter that returns
     * true only when bits [1:0] both read 1, so the firmware expects the
     * hardware to set bit 1 in response.
     *
     * DECLARED CHOICE, not measured. 0x402b503c has no direct caller -- it
     * is reached through a dispatch table -- so whether anything loops on it
     * is unknown, and so is how long the real hardware takes. Bit 1 is set
     * immediately because that is the only behaviour that cannot hang; if
     * this turns out to matter, the firmware side to re-read is 0x402b503c.
     */
    if (page == Z63_DISP_PAGE_E && off == 0x004 && (value & 1)) {
        value |= 2;
    }
    /* 0x15490000/+0x100/+0x200/+0x300: start/stop/clear words, above. */
    if (page == Z63_DISP_PAGE_PWR && off < 0x400 && !(off & 0xff)) {
        value = z63_disp_command(value);
    }

    d->cplx_regs[addr / 4] = value;
}

static const MemoryRegionOps z63_disp_cplx_ops = {
    .read = z63_disp_cplx_read,
    .write = z63_disp_cplx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* Every MMIO access in all 325 band functions is LDR Wn / STR Wn --
     * verified on the widest reader, 0x402b4ec0, whose loads are all
     * `LDR W9` with the narrowing STRB/STRH applied to the destination
     * struct. The byte-offset addresses the decompiler prints
     * (MEMORY[0x1548E00A], MEMORY[0x1548E202], ...) are sub-fields of a
     * single 32-bit load, not registers. So a narrower access is a
     * surprise, and surprises should stop. */
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* The two DISP unit windows at 0x15480000 / 0x15484000                */
/*                                                                    */
/* Plain register files, in the idiom the board already uses for the     */
/* EMIO and VPIPE banks, and for the same reason: the offsets are not     */
/* enumerable. 0x402b64bc reads `base + runtime offset` for any of the   */
/* four sub-blocks, so no whitelist can be complete.                     */
/*                                                                    */
/* The offsets that ARE established, relative to the unit base:          */
/*                                                                    */
/*   +0x0000  0 or 1, a gate      0x402b0528, 0x402b0c70                */
/*   +0x0100  1/17/2/0 | flag<<8  0x402b0814, 0x402b0928, 0x402b0a24    */
/*   +0x1000  0 or 1, a gate      0x402b0528, 0x402b0c70, 0x402b0300    */
/*   +0x1100  [6:0] + bit 9 on unit 0 (0x402b03b4/0x402b03c8), bit 8 on  */
/*            unit 1 (0x402b03d0/0x402b03e4); mask 0xffffc080            */
/*   +0x1104  same shape, unit 1 only   0x402b03e8                      */
/*   +0x1200  1/17, or `&= ~3`    0x402b0814, 0x402b0a24                */
/*   +0x1280  same                0x402b0814, 0x402b0a24                */
/*   +0x2000  0 or 1, a gate      0x402b0528, 0x402b0c70, 0x402b0300    */
/*   +0x2100  1/2/0               0x402b0814, 0x402b0928, 0x402b0a24    */
/*   +0x3000  0 or 1, a gate      0x402b0528, 0x402b0c70, 0x402b0300    */
/*   +0x3100  1/2/0               0x402b0814, 0x402b0928, 0x402b0a24    */
/*                                                                    */
/* The five command words (+0x100, +0x1200, +0x1280, +0x2100, +0x3100)    */
/* read back 3 in their low bits after a start; see z63_disp_command.    */
/*                                                                    */
/* NO register in these windows is claimed to mean anything beyond that.  */
/* Reads return the last write and unwritten words read 0, which is the   */
/* board's existing trade for a bank whose offsets cannot be listed --    */
/* not a measured reset value.                                           */
/* ------------------------------------------------------------------ */

/*
 * Events. Each unit has a pending word at +0x1214 (and +0x1294 for its
 * second output) behind an enable mask at +0x1210 (+0x1290); its ISR,
 * 0x402b6638 for DISP_1 (INTID 100), reads +0x1214 and writes the value
 * back before its callback -- write-one-to-clear, as ../z9-qemu
 * (engine1548_pending_word, its 402b0aa4) reads the same pair. The line is
 * high while a pending bit is enabled. Only the scanout sets bits
 * (nikon-z63-scanout.h): bit 0 every period, bit 16 when a frame was
 * fetched; nothing else here raises one.
 */
#define Z63_DISP_R_MASK0     0x1210
#define Z63_DISP_R_PEND0     0x1214
#define Z63_DISP_R_MASK1     0x1290
#define Z63_DISP_R_PEND1     0x1294

static void z63_disp_irq_update(Z63DispWin *w)
{
    if (w->irq) {
        if (w->irq1) {
            /* Own 402b902c selects PEND0 for IRQ100, PEND1 for IRQ101. */
            qemu_set_irq(w->irq, w->words[Z63_DISP_R_PEND0 / 4] &
                                w->words[Z63_DISP_R_MASK0 / 4]);
            qemu_set_irq(w->irq1, w->words[Z63_DISP_R_PEND1 / 4] &
                                 w->words[Z63_DISP_R_MASK1 / 4]);
            return;
        }
        qemu_set_irq(w->irq,
                     (w->words[Z63_DISP_R_PEND0 / 4] &
                      w->words[Z63_DISP_R_MASK0 / 4]) ||
                     (w->words[Z63_DISP_R_PEND1 / 4] &
                      w->words[Z63_DISP_R_MASK1 / 4]));
    }
}

static uint64_t z63_disp_win_read(void *opaque, hwaddr addr, unsigned size)
{
    Z63DispWin *w = opaque;

    if (size != 4 || (addr & 3) || addr >= w->size) {
        z63_stop_read(w->name, w->base + addr, size);
        return 0;
    }
    return w->words[addr / 4];
}

static void z63_disp_win_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Z63DispWin *w = opaque;

    if (size != 4 || (addr & 3) || addr >= w->size) {
        z63_stop_write(w->name, w->base + addr, size, value);
        return;
    }
    if (z63_disp_is_command(addr)) {
        value = z63_disp_command(value);
    }
    if (w->irq && (addr == Z63_DISP_R_PEND0 || addr == Z63_DISP_R_PEND1)) {
        w->words[addr / 4] &= ~(uint32_t)value;
    } else {
        w->words[addr / 4] = value;
    }
    if (w->irq && (addr == Z63_DISP_R_PEND0 || addr == Z63_DISP_R_PEND1 ||
                   addr == Z63_DISP_R_MASK0 || addr == Z63_DISP_R_MASK1)) {
        z63_disp_irq_update(w);
    }
}

static const MemoryRegionOps z63_disp_win_ops = {
    .read = z63_disp_win_read,
    .write = z63_disp_win_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* 0x15496000..0x15498fff, and window 2 at 0x15c80000: refuse          */
/*                                                                    */
/* 0x15496000/0x15497000/0x15498000 is the band's largest block: one     */
/* driver at 0x402b6d14..0x402bb100 and 399 distinct 32-bit registers.   */
/* It was mapped but NOT reversed. docs/display-registers.md section 5   */
/* has the complete offset census and everything that was established:   */
/*                                                                    */
/*  - no polling loop exists anywhere on these three pages. All 325 band */
/*    functions were checked, and because every address here is built as */
/*    an immediate (never reached through a pointer) that census is       */
/*    complete. The only spin loops in the whole band are the two        */
/*    PHY_STATUS loops above.                                            */
/*  - twelve registers are read-only: 0x15497000, 0x15497400..410,       */
/*    0x154974a0/a4, 0x15497a84, 0x15498014, 0x15498020, 0x15498024.     */
/*    0x15497c00/c04 and 0x15497c80..cfc look read-only to a constant     */
/*    search but are the payload window: read back at 0x402b706c and      */
/*    written by the walking-pointer loop at 0x402b7284.                  */
/*  - 0x15497000 is read as one word and split into a byte [23:16] and a */
/*    halfword [15:0] (0x402b6d14) -- the shape of a packed design /     */
/*    revision / product id -- and never compared.                       */
/*  - 0x15497600 has paired set/clear accessors per bit (0x402b7a74 ff.  */
/*    and 0x402b7cd4 ff.) while 0x15497604 has set-only ones             */
/*    (0x402b7f34 ff.) that nonetheless have matching getters. Whether    */
/*    the set-only registers are write-1-to-clear status or plain enables */
/*    COULD NOT BE DECIDED; both readings fit.                           */
/*  - identity is probable, not proven: the interrupt table's neighbours  */
/*    (DISP_DWCH, HDMI_MASTER, HDMI_CEC), the 0x20-stride descriptor      */
/*    arrays, the 256-byte payload window at 0x15497c00 and the 28-byte   */
/*    one at 0x15497844 all point at an HDMI transmitter. Nothing in the  */
/*    image names it and it is not asserted here.                        */
/*                                                                    */
/* A plain register file would not hang anything, but twelve of these are  */
/* read-only with no established expectation, so a read returning 0 would */
/* be an invented device response. It stops instead, with the offset and  */
/* the PC. If a particular bring-up run needs to push past this, the      */
/* change is this one function body -- label it when you make it.         */
/*                                                                    */
/* Window 2 (0x15c80000/0x15c84000) shares these ops for a different      */
/* reason: not one offset in it is established at all, and it is outside  */
/* the 0x15400000..0x15700000 band this work covers. Mapping it only buys */
/* a block name in the stop message over the board's unmapped trap.       */
/* ------------------------------------------------------------------ */

static uint64_t z63_disp_refuse_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    Z63DispWin *w = opaque;

    z63_stop_read(w->name, w->base + addr, size);
    return 0;
}

static void z63_disp_refuse_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    Z63DispWin *w = opaque;

    z63_stop_write(w->name, w->base + addr, size, value);
}

static const MemoryRegionOps z63_disp_refuse_ops = {
    .read = z63_disp_refuse_read,
    .write = z63_disp_refuse_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

/* ------------------------------------------------------------------ */
/* Reset and wiring                                                    */
/* ------------------------------------------------------------------ */

/* Reset clears every register file. For the DSI host that means PHY_RSTZ
 * bit 0 is 0, so PHY_STATUS reads 0 -- which is exactly the state the
 * firmware's own bring-up assumes: 0x402b475c clears PHY_RSTZ bit 0 and
 * PWR_UP bit 0 before configuring the PHY and sets them afterwards, and the
 * HS-clock gate at 0x402b4ccc is only satisfied after that.
 *
 * No non-zero reset value is set anywhere in this model, because none was
 * measured. Where the firmware reads a register it never writes, the
 * comment at the read site says so. */
static void z63_display_reset(Z63Display *d)
{
    unsigned i;

    memset(d->cplx_regs, 0, sizeof(d->cplx_regs));
    for (i = 0; i < Z63_DISP_UNIT_INST; i++) {
        memset(d->unit_regs[i], 0, sizeof(d->unit_regs[i]));
    }
}

static void z63_display_init(Object *owner, MemoryRegion *sysmem,
                             DeviceState *gic, Z63Display *d)
{
    static const char *const unit_names[Z63_DISP_UNIT_INST] = {
        "DISP-UNIT0", "DISP-UNIT1",
    };
    unsigned i;

    z63_display_reset(d);

    /* 0x1548c000..0x15492fff, one region so that an address in a page's
     * unenumerated space still reaches this model and stops with the block
     * name rather than landing in the unassigned pool. */
    memory_region_init_io(&d->cplx, owner, &z63_disp_cplx_ops, d,
                          "z63-display-complex", Z63_DISP_CPLX_SIZE);
    memory_region_add_subregion(sysmem, Z63_DISP_CPLX_BASE, &d->cplx);

    /* The two DISP unit windows. One subregion each, so the stop message
     * names the unit. */
    for (i = 0; i < Z63_DISP_UNIT_INST; i++) {
        d->unit[i].name = unit_names[i];
        d->unit[i].base = Z63_DISP_UNIT_BASE + i * Z63_DISP_UNIT_STRIDE;
        d->unit[i].size = Z63_DISP_UNIT_STRIDE;
        d->unit[i].words = d->unit_regs[i];
        d->unit[i].irq = qdev_get_gpio_in(gic,
                                          Z63_GIC_SPI(Z63_SPI_DISP_0 + i));
        if (i == 1 && object_dynamic_cast(owner, MACHINE_TYPE_NAME("nikon-zf"))) {
            d->unit[i].irq1 = qdev_get_gpio_in(gic, Z63_GIC_SPI(Z63_SPI_DISP_1B));
        }
        memory_region_init_io(&d->unit[i].region, owner, &z63_disp_win_ops,
                              &d->unit[i], d->unit[i].name,
                              d->unit[i].size);
        memory_region_add_subregion(sysmem, d->unit[i].base,
                                    &d->unit[i].region);
    }

    /* 0x15496000+0x3000 was mapped refuse-only here while the block was
     * unidentified. It has since been reversed as the HDMI transmitter and
     * is owned by nikon-z63-hdmi.h, so this model must not claim it: two
     * regions over one span is a conflict QEMU resolves by priority rather
     * than by complaining. See docs/hdmi-registers.md. */

    /* Window 2: the units' lookup-table RAM. Mapped refuse-only while no
     * offset was established. The MCU-driven start then reached
     * sub_402B4088 (caller 0x402de948), which fills three 33-entry 11-bit
     * tables at +0x1000..+0x1040, +0x1080..+0x10c0 and +0x1100..+0x1140
     * of a unit's window (it stopped on 0x15c85000, unit 1) -- a table
     * written by a loop, which is RAM, and ../n-research/z8-qemu names
     * 0x15c80000 / 0x15c84000 the same way: LUT RAM of resources 0x1c7 /
     * 0x1c8. So it is RAM now. Nothing here applies the tables; the
     * scanout's output-LUT check (0x1530) still refuses a frame that would
     * need them. */
    memory_region_init_ram(&d->win2, NULL, "z63-display-lut",
                           (uint64_t)Z63_DISP_UNIT_INST * Z63_DISP_UNIT_STRIDE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, Z63_DISP_WIN2_BASE, &d->win2);
}

#endif /* HW_ARM_NIKON_Z63_DISPLAY_H */

/* ------------------------------------------------------------------ */
/* How to wire this into hw/arm/nikon-z63.c                            */
/*                                                                    */
/* Three edits. Nothing else in the board changes, and no existing      */
/* region moves: the spans used here (0x1548c000+0x7000,               */
/* 0x15480000+0x8000, 0x15496000+0x3000, 0x15c80000+0x8000) do not     */
/* overlap anything the board already maps, including the main DMA     */
/* banks at 0x155c0000 / 0x15600000 / 0x15680000.                      */
/*                                                                    */
/* 1. After the last #include (the `#include <libfdt.h>` line, ~line    */
/*    51), add:                                                         */
/*                                                                    */
/*        #include "hw/arm/nikon-z63-display.h"                       */
/*                                                                    */
/*    It has to come before NikonZ63State so the state field below     */
/*    resolves; the board's z63_stop_read / z63_stop_write are          */
/*    forward-declared inside the header for exactly that reason.       */
/*                                                                    */
/*    If hw/arm/nikon-z63-storage.h is included too, both headers       */
/*    forward-declare those two helpers. This one guards its pair with  */
/*    Z63_STOP_HELPERS_DECLARED; adding the same guard to the storage   */
/*    header makes the include order irrelevant under GCC's             */
/*    -Wredundant-decls (clang does not implement that warning).        */
/*                                                                    */
/* 2. In `typedef struct NikonZ63State { ... }` (~line 434), next to    */
/*    the other region groups -- e.g. just after                        */
/*    `MemoryRegion upk_region; uint32_t upk[...];` (~line 530) -- add: */
/*                                                                    */
/*        Z63Display display;                                          */
/*                                                                    */
/* 3. In machine init, next to the other memory_region_add_subregion    */
/*    calls -- e.g. just after the "Driver upload window" block         */
/*    (~line 2955) -- add:                                              */
/*                                                                    */
/*        /+ Display band: DSI complex, the two DISP unit windows, and  */
/*         + the unreversed 0x15496000 cluster. See                     */
/*         + docs/display-registers.md. +/                              */
/*        z63_display_init(OBJECT(machine), sysmem, &s->display);       */
/*                                                                    */
/*    (replace `/+ ... +/` with a real comment; written this way so     */
/*    this note does not close the comment it lives in)                 */
/*                                                                    */
/* z63_display_init() calls z63_display_reset() itself, so there is no  */
/* fourth edit unless the board later grows a machine-level reset hook  */
/* that should also re-zero these register files; in that case call     */
/* z63_display_reset(&s->display) from it.                             */
/* ------------------------------------------------------------------ */
