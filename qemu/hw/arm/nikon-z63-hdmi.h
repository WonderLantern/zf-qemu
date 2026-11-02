/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Nikon Z 6III (EXPEED 7) research machine -- HDMI transmitter,
 * 0x15496000 / 0x15497000 / 0x15498000.
 *
 * Reversed from the decrypted factory RTOS image eg2010_0200i1.bi
 * (sha256 736ceaac...1c47fda). Every behaviour below cites the firmware VA
 * that establishes it; file offset = VA - 0x40160000, and .data VAs map to
 * file offset 0x5099000 + (VA - 0xA02808000). The full register map, the
 * evidence for the identification, the reachability analysis and everything
 * this file deliberately refuses are in docs/hdmi-registers.md -- read that
 * before extending this.
 *
 * WHAT THE BLOCK IS
 *
 * The HDMI transmitter, source side. The identification is protocol-level:
 *
 *  - 0x15497A80 is a DDC/I2C master whose address field carries the 8-bit
 *    I2C slave address in [7:0]. The firmware's static request table at
 *    .data 0xA0280FDF8 (19 entries of {u32 op; u8 slave; u8 offset; u16 len;
 *    void *buf}, consumed at 0x402C36CC) uses slave 0xA8 -- the HDMI SCDC --
 *    with offsets 0x01 Sink Version, 0x02 Source Version, 0x10 Update_0,
 *    0x20 TMDS_Config, 0x21 Scrambler_Status, 0x30 Config_0, 0x31 Config_1,
 *    0x40 Status_Flags_0, 0x41 (2 bytes), 0x50 (9 bytes, the CED block).
 *    That is the HDMI 2.0/2.1 SCDC map verbatim. It writes Source Version
 *    and reads Sink Version, so it is the source.
 *  - 0x402C1940 reads EDID block n with segment = n >> 1, offset =
 *    (n << 7) & 0xff, length 128, through command 0x48 -- E-DDC segmented
 *    addressing -- and the read-back window at 0x15497C00 is 256 bytes,
 *    two EDID blocks (0x402B706C).
 *  - the 0x20-stride slots are HDMI packet slots: +0x00 holds HB0 [7:0],
 *    HB1 [15:8], HB2/length [23:16], and +0x04..+0x1F holds exactly 28
 *    body bytes -- four subpackets of seven. 0x402B8660 writes PB1..PB3 and
 *    never PB0, the checksum the hardware computes.
 *  - the RTOS objects this driver creates are DisSvH00..DisSvH10
 *    (0x402C3EE4); the display controller's are DisSvD00/DisSvD01.
 *
 * It is NOT the Synopsys DesignWare HDMI TX register map, despite the
 * DISP_DWCH entry in the SoC interrupt-source table at rodata 0x41847000:
 * DWC has four separate byte id registers at 0x0000-0x0003 and its frame
 * composer at 0x1000+, while this block has one packed id word at +0x000
 * (0x402B6D14) and fits in three pages of 32-bit registers. Which SoC
 * interrupt line it raises -- SPI 70 DISP_DWCH, 71 HDMI_MASTER or 72
 * HDMI_CEC -- is NOT established; see "the interrupt" below.
 *
 * The SoC's own physical-region table at .data 0xA02BD2380 calls
 * 0x15000000-0x15800000 "ISP_APB". That is an aperture name, not a block
 * name: the same window holds the two DISP units at 0x15480000/0x15484000,
 * the DesignWare MIPI DSI host at 0x1548D000 and the three main DMA banks.
 * This transmitter is reached over the ISP APB bus.
 *
 * WHAT IS MODELLED
 *
 *   - the DDC / E-DDC master and its 256-byte payload window, with the
 *     command byte's self-clear and the status register the client polls;
 *   - the write-1-to-clear interrupt status registers;
 *   - 0x15497008 bits [4:0] as read-only hardware status;
 *   - everything else in the 526-word whitelist as a plain register file,
 *     which is the correct answer for the ~380 registers the driver only
 *     ever read-modify-writes.
 *
 * WHAT IS REFUSED
 *
 *   - every offset outside the whitelist, and every access that is not an
 *     aligned 32-bit one: they stop the VM with the address and the PC.
 *     The driver names addresses only through MOVZ/MOVK immediates and
 *     computes no runtime offset into these pages, so the whitelist is
 *     complete for this firmware and a stop is a genuine lead.
 *   - the interrupt. See below.
 *
 * NOT MEASURED, AND LABELLED WHERE IT APPEARS BELOW: any reset value (every
 * register starts at 0 because nothing establishes otherwise), the DDC
 * result code for a sink-less transfer, which of bits 6 and 5 of 0x15497094
 * means "done" rather than "error", and the contents of the six read-only
 * registers listed in z63_hdmi_tx_read().
 *
 * This header must be included by hw/arm/nikon-z63.c; see the wiring note at
 * the end, which also says what must be removed from nikon-z63-display.h.
 */
#ifndef HW_ARM_NIKON_Z63_HDMI_H
#define HW_ARM_NIKON_Z63_HDMI_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "system/memory.h"

/* Defined by the board, below the device models. Forward-declared so this
 * header can be included before NikonZ63State is defined.
 *
 * hw/arm/nikon-z63-display.h declares the same two helpers behind the same
 * guard macro, so the include order does not matter; give the storage
 * header the same guard and GCC's -Wredundant-decls stays quiet there too
 * (clang does not implement that warning). */
#ifndef Z63_STOP_HELPERS_DECLARED
#define Z63_STOP_HELPERS_DECLARED
static void z63_stop_read(const char *block, hwaddr addr, unsigned size);
static void z63_stop_write(const char *block, hwaddr addr, unsigned size,
                           uint64_t value);
#endif

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* Three contiguous 4 KiB pages behind one driver, 0x402B6D14..0x402BB100.
 * 399 registers are named by an immediate; the walking-pointer loops reach
 * 127 more words (the packet-slot bodies and the DDC payload window), for
 * 526 in all. 279 of the 399 belong to accessors that have no caller
 * anywhere in the image -- see docs/hdmi-registers.md section 7. */
#define Z63_HDMI_BASE           0x15496000ULL
#define Z63_HDMI_PAGES          3
#define Z63_HDMI_SIZE           (Z63_HDMI_PAGES * 0x1000)

#define Z63_HDMI_PAGE_CFG       0   /* 0x15496000: 37 regs, not identified */
#define Z63_HDMI_PAGE_TX        1   /* 0x15497000: the transmitter proper   */
#define Z63_HDMI_PAGE_PHY       2   /* 0x15498000: PHY-shaped, all dead code*/

/* ------------------------------------------------------------------ */
/* Page 1 registers this file treats specially                         */
/* ------------------------------------------------------------------ */

/* Read-only packed id: 0x402B6D14 reads one word and splits it into a byte
 * [23:16] and a halfword [15:0] into a struct. Never compared. */
#define Z63_HDMI_ID             0x000

/* 0x402B763C (mask 0xFFFF3CFF), 0x402B8D1C (mask 0xFFFFBFFF) and
 * 0x402BA5C0 (|= 0x2000) all preserve bits [4:0], and three getters read
 * them: 0x402B6F78 (bit 3), 0x402B6F98 (bit 4), 0x402BA5E0 (bits 2,1,0).
 * So [4:0] are hardware status inside an otherwise RW register. Bit 3 is
 * the cable-present input the HDMI service debounces at 0x402C13E4. */
#define Z63_HDMI_STAT_CTRL      0x008
#define Z63_HDMI_STAT_RO_MASK   0x0000001fu
#define Z63_HDMI_STAT_HPD       (1u << 3)

/* Bits [15:8] are write-1-to-clear status. Proven, not inferred:
 * 0x402D0434 calls 0x402B8174, which writes 1s into exactly those eight
 * bits, and then loops up to 501 times at 1 ms reading them back through
 * 0x402BA458 until all eight are 0. The shift tables it uses are rodata
 * 0x4184B460 = {15,14,13,12} and 0x4184B470 = {11,10,9,8}. */
#define Z63_HDMI_IRQ_B          0x010
#define Z63_HDMI_IRQ_B_W1C      0x0000ff00u

/* The pair the ISR path uses. 0x402BA2D8 reads both, scatters ~26 bits into
 * a 104-byte struct and then writes back the values it read -- the
 * read-and-write-back-to-clear idiom, so a written 1 clears. 0x15497098 and
 * 0x1549709C are their enable masks (0x402B6D78 writes bit collections at
 * exactly the positions 0x402BA2D8 reads). */
#define Z63_HDMI_IRQ_A_STAT0    0x090
#define Z63_HDMI_IRQ_A_STAT1    0x094
#define Z63_HDMI_IRQ_A_EN0      0x098
#define Z63_HDMI_IRQ_A_EN1      0x09c

/* 0x402BA2D8 puts (STAT1 >> 6) & 1 at struct+96 and (STAT1 >> 5) & 1 at
 * struct+100, and 0x402C34D4 sets the DDC completion event flag when either
 * is 1. So bits 6 and 5 are the DDC "finished" pair. WHICH ONE IS "done"
 * AND WHICH IS "error" IS NOT ESTABLISHED; this model sets bit 6. */
#define Z63_HDMI_IRQ_A1_DDC_DONE  (1u << 6)

/* Read-only status read by accessors that have no caller in this build.
 * 0x400..0x410: 0x402B7598 pulls a 14-bit and a 13-bit field out of 0x400,
 * a 9-bit and a 16-bit out of 0x404, a 14-bit and a 13-bit out of 0x408,
 * an 11-bit and a byte out of 0x40C and three bits out of 0x410. Two 14/13
 * pairs look like live H/V position; nothing in the image confirms that.
 * 0x4A0/0x4A4: 0x402BAB5C assembles one 56-bit value. Not identified. */
#define Z63_HDMI_VSTAT0         0x400
#define Z63_HDMI_VSTAT4         0x410
#define Z63_HDMI_RO_A0          0x4a0
#define Z63_HDMI_RO_A4          0x4a4

/* Interrupt enable (every bit has a matching set and clear accessor:
 * 0x402B7A74.. and 0x402B7CD4.. for 0x600 bits 17-21 and 24-31,
 * 0x402B7B54.. and 0x402B7DB4.. for 0x610 bits [3:0]) and interrupt status
 * (set-only accessors 0x402B7F34.. for 0x604 bits [7:0] and 0x402B8074..
 * for 0x610 bits [11:8], read back by 0x402BA458).
 *
 * DECLARED, with the reasoning in docs/hdmi-registers.md section 4.2: the
 * status bits are modelled as "a written 1 does not stick". Whether they
 * are write-1-to-clear status or self-clearing one-shot packet-send
 * requests COULD NOT BE DECIDED -- two of them are set right after a packet
 * slot is filled (0x402CFE00 -> 0x402CFE08, 0x402CFE98 -> 0x402CFEA0) --
 * but both readings require exactly this behaviour, so one rule covers
 * them. If they are send requests, clearing immediately means the model
 * reports the packet sent at once, which is the only choice that cannot
 * stall. */
#define Z63_HDMI_IRQ_C_EN       0x600
#define Z63_HDMI_IRQ_C_STAT     0x604
#define Z63_HDMI_IRQ_C_STAT_W1C 0x000000ffu
#define Z63_HDMI_IRQ_C_EXT      0x610
#define Z63_HDMI_IRQ_C_EXT_W1C  0x00000f00u

/* ------------------------------------------------------------------ */
/* The DDC / E-DDC master                                              */
/*                                                                     */
/* 0x402B7034 writes                                                   */
/*     CMD = (cmd << 24) | ((len - 1) << 16) | addr16                  */
/* from an {u32 cmd; u16 addr16; u16 len} descriptor, preceded by a     */
/* discarded LDR WZR -- a pure write. addr16 is slave | (offset << 8)   */
/* for commands 0x40/0x44 and segment | (offset << 8) for 0x48.         */
/* ------------------------------------------------------------------ */

#define Z63_HDMI_DDC_CMD        0xa80
#define Z63_HDMI_DDC_STAT       0xa84
#define Z63_HDMI_DDC_ERRCLR     0xa88   /* 0x402B7490 writes 0xF9 to [7:0] */
#define Z63_HDMI_DDC_CFG        0xa9c   /* 0x402B6FE4; constant {0,1} at
                                         * rodata 0x4184D320, so always
                                         * bit 2 = 0, bits [1:0] = 1 */
#define Z63_HDMI_DDC_DATA       0xc00   /* 256 bytes, 0xc00..0xcfc */
#define Z63_HDMI_DDC_DATA_LEN   0x100

/* The go bit. 0x402B7010 is LDR W8,[CMD]; LSR #24; CMP #0; CSET -- "idle
 * iff bits [31:24] are zero" -- so the command byte self-clears. */
#define Z63_HDMI_DDC_CMD_SHIFT  24

/* The five command values the firmware issues, from the switch at
 * 0x402C3740..0x402C3798 that builds the descriptor. */
#define Z63_HDMI_DDC_CMD_READ   0x40  /* 0x402C3744: slave + offset read   */
#define Z63_HDMI_DDC_CMD_WRITE  0x44  /* 0x402C3770: slave + offset write  */
#define Z63_HDMI_DDC_CMD_SEGRD  0x48  /* 0x402C377C: E-DDC segmented read  */
#define Z63_HDMI_DDC_CMD_RECOV1 0x49  /* 0x402C378C: issued after result
                                       * 0x40 (0x402C3800)                 */
#define Z63_HDMI_DDC_CMD_RECOV2 0x4a  /* 0x402C3794: issued after result
                                       * 0x20, with 0xF9 first (0x402C3818)*/

/* STATUS: [7:0] is the transfer state and the client's only test on it is
 * "== 1 means busy" (0x402C38A0, 0x402C38CC). [15:8] is the result code and
 * the client recognises 0 (0x402C37D8), 0x20 (0x402C37D0) and 0x40
 * (0x402C37C8). */
#define Z63_HDMI_DDC_STATE_BUSY 0x01

/* DECLARED STAND-IN, not measured. There is no HDMI sink in this machine,
 * so a DDC transfer has nothing to talk to and must fail; 0x40 is one of
 * the only two failure codes the firmware recognises, and WHICH PHYSICAL
 * CONDITION EACH OF 0x20 AND 0x40 MEANS IS NOT ESTABLISHED.
 *
 * As shipped the client never reads this: with no interrupt it gives up at
 * the 1000 ms event timeout (0x402C390C) and 0x402C37BC returns 0 without
 * looking. The value is set correctly anyway so that wiring the interrupt
 * is a one-line change. Replay of 0x402C36CC / 0x402C3870 / 0x402C392C with
 * the interrupt hypothetically wired: the client issues the recovery
 * command 0x49, gets 0x40 again, restores the original command, gets 0x40
 * again, and 0x402C36CC returns 0 -- "no device", in finite time. Setting this to 0 would instead make the client
 * accept the transfer and copy 256 zero bytes out of the payload window as
 * if they were an EDID, which is exactly the invented device response this
 * project refuses. */
#define Z63_HDMI_DDC_RESULT_NO_SINK 0x40

/* ------------------------------------------------------------------ */
/* Page 2 read-only registers                                          */
/*                                                                     */
/* 0x15498014 (0x402BADF0), 0x15498020 (0x402BAF28) and 0x15498024        */
/* (0x402BAF5C) are read and never written. All three readers, and every   */
/* other accessor on this page, have no caller in the image, so nothing in  */
/* this firmware can observe the answer. */
#define Z63_HDMI_PHY_RO_14      0x014
#define Z63_HDMI_PHY_RO_20      0x020
#define Z63_HDMI_PHY_RO_24      0x024

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct Z63Hdmi {
    MemoryRegion mr;

    /* One flat file for the three pages, indexed by (addr / 4). The DDC
     * payload window lives in it at page 1 + 0xc00. */
    uint32_t regs[Z63_HDMI_SIZE / 4];

    /* The one hardware input in the block: 0x15497008 bit 3, the
     * cable-present line the HDMI service debounces at 0x402C13E4.
     *
     * DECLARED false, and left false: there is no sink model here, and the
     * DDC engine answers "no sink" regardless (see
     * Z63_HDMI_DDC_RESULT_NO_SINK). Setting it true without also giving the
     * DDC engine something to talk to would only make the firmware spend
     * the 1000 ms twai_flg timeout at 0x402C390C on every EDID attempt.
     *
     * It costs nothing either way in this build: the branch that reads this
     * bit is dead. 0x402C13E4 consults it only when MEMORY[0xA0280FDF0] is
     * not 1, that word is 1 in .data (file offset 0x50A0DF0), and nothing
     * in the image writes it -- so cable presence actually comes from
     * sub_40C15BFC, another driver. */
    bool hpd;
} Z63Hdmi;

/* ------------------------------------------------------------------ */
/* Offset whitelist                                                    */
/*                                                                     */
/* Generated from the census in docs/hdmi-registers.md section 2 and     */
/* complete for this firmware: the driver builds every address with a     */
/* MOVZ/MOVK pair and never computes a runtime offset into these pages,   */
/* unlike the display complex next door, where 0x402B64BC does. An offset */
/* absent here is one no instruction in the image names, so a stop on it  */
/* is a real lead.                                                        */
/*                                                                     */
/* The slot ranges below cover whole 32-byte packet slots, not just the   */
/* words named by an immediate: the bodies are filled by walking-pointer  */
/* loops (0x402B8628 in 0x402B8578, 0x402B8E0C in 0x402B8D94, and friends) that an         */
/* immediate search cannot see.                                           */
/* ------------------------------------------------------------------ */

#define Z63_HDMI_R(o, lo, hi)   ((o) >= (lo) && (o) <= (hi))

static bool z63_hdmi_cfg_known(hwaddr off)      /* 0x15496000, 37 words */
{
    return off == 0x070 || Z63_HDMI_R(off, 0x08c, 0x094) || off == 0x0b0 ||
           off == 0x140 || off == 0x150 || off == 0x220 || off == 0x244 ||
           off == 0x250 || Z63_HDMI_R(off, 0x264, 0x268) ||
           Z63_HDMI_R(off, 0x27c, 0x280) || Z63_HDMI_R(off, 0x294, 0x298) ||
           Z63_HDMI_R(off, 0x2ac, 0x2fc);
}

static bool z63_hdmi_tx_known(hwaddr off)       /* 0x15497000, 470 words */
{
    return
        /* id, control, interrupt group A and B */
        Z63_HDMI_R(off, 0x000, 0x010) || off == 0x018 || off == 0x020 ||
        Z63_HDMI_R(off, 0x050, 0x05c) || Z63_HDMI_R(off, 0x090, 0x09c) ||
        off == 0x0d0 ||
        /* unreachable bulk tables (0x402B92DC, 0x402B935C, 0x402B970C,
         * 0x402B93EC, 0x402B97BC) */
        Z63_HDMI_R(off, 0x120, 0x130) || Z63_HDMI_R(off, 0x140, 0x148) ||
        Z63_HDMI_R(off, 0x150, 0x1d8) || Z63_HDMI_R(off, 0x220, 0x2fc) ||
        Z63_HDMI_R(off, 0x320, 0x3fc) ||
        /* video status (read-only) and the video/audio front end */
        Z63_HDMI_R(off, 0x400, 0x410) || Z63_HDMI_R(off, 0x420, 0x42c) ||
        Z63_HDMI_R(off, 0x43c, 0x454) || off == 0x460 ||
        Z63_HDMI_R(off, 0x4a0, 0x4b4) ||
        /* interrupt group C */
        Z63_HDMI_R(off, 0x600, 0x604) || Z63_HDMI_R(off, 0x610, 0x614) ||
        /* packet slots 0x620/640/660/680/6a0 */
        Z63_HDMI_R(off, 0x620, 0x6bf) ||
        /* packet slots 0x720/740/760/780/7a0/7c0 */
        Z63_HDMI_R(off, 0x720, 0x7df) ||
        Z63_HDMI_R(off, 0x810, 0x814) || off == 0x81c ||
        /* packet slots 0x820/840/860/880/8a0 */
        Z63_HDMI_R(off, 0x820, 0x8bf) ||
        /* packet slots 0x8e0 and 0x920 */
        Z63_HDMI_R(off, 0x8e0, 0x8ff) || Z63_HDMI_R(off, 0x920, 0x93f) ||
        off == 0x948 ||
        /* DDC master */
        Z63_HDMI_R(off, 0xa80, 0xa88) || off == 0xa9c ||
        /* 0x402B97BC's third table, the 0x402B9C80/0x402B9D0C block and
         * 0x402BA184's three words */
        Z63_HDMI_R(off, 0xb20, 0xbfc) ||
        /* DDC payload window */
        Z63_HDMI_R(off, 0xc00, 0xcfc);
}

static bool z63_hdmi_phy_known(hwaddr off)      /* 0x15498000, 19 words */
{
    return Z63_HDMI_R(off, 0x000, 0x014) || Z63_HDMI_R(off, 0x020, 0x04c) ||
           off == 0x058;
}

/* Registers the firmware only ever reads. A write to one of these means a
 * firmware path this pass never saw, or a wrong model; both are worth a
 * stop. 0x15497A84 is read-only too but is computed, not stored, so it is
 * handled in the write path by name. */
static bool z63_hdmi_tx_read_only(hwaddr off)
{
    return off == Z63_HDMI_ID ||
           Z63_HDMI_R(off, Z63_HDMI_VSTAT0, Z63_HDMI_VSTAT4) ||
           Z63_HDMI_R(off, Z63_HDMI_RO_A0, Z63_HDMI_RO_A4) ||
           off == Z63_HDMI_DDC_STAT;
}

static bool z63_hdmi_phy_read_only(hwaddr off)
{
    return off == Z63_HDMI_PHY_RO_14 || off == Z63_HDMI_PHY_RO_20 ||
           off == Z63_HDMI_PHY_RO_24;
}

/* ------------------------------------------------------------------ */
/* The DDC engine                                                      */
/* ------------------------------------------------------------------ */

/* Run one transfer, synchronously, inside the store to the command
 * register. Synchronous completion is a declared choice: it is the only
 * timing that cannot leave the client's 201-iteration busy pre-loop at
 * 0x402C38AC spinning, and the firmware cannot tell the difference, because
 * it never reads the state field between the write and the event wait.
 *
 * What this does NOT do is signal completion -- see "the interrupt" at the
 * bottom of this file. The caller at 0x402C390C waits on an event flag for
 * 1000 ms and then gives up.
 */
static void z63_hdmi_ddc_start(Z63Hdmi *h, uint32_t cmd_word)
{
    uint32_t *tx = &h->regs[(Z63_HDMI_PAGE_TX * 0x1000) / 4];
    uint32_t cmd = cmd_word >> Z63_HDMI_DDC_CMD_SHIFT;

    switch (cmd) {
    case Z63_HDMI_DDC_CMD_READ:
    case Z63_HDMI_DDC_CMD_SEGRD:
        /* Nothing answered, so nothing came back. The window is cleared
         * rather than left holding the previous transfer's bytes: on a
         * failed EDID read 0x402B706C copies the window out regardless
         * (0x402C3848 runs on the recovery path too), and handing the
         * firmware a stale SCDC write buffer and calling it an EDID would
         * be worse than handing it zeros. Declared, not measured. */
        memset(&tx[Z63_HDMI_DDC_DATA / 4], 0, Z63_HDMI_DDC_DATA_LEN);
        break;

    case Z63_HDMI_DDC_CMD_WRITE:
        /* The payload is already in the window: 0x402B7284 fills it with
         * the walking-pointer loop at 0x402B72E8 before the command is
         * issued (0x402C3768 precedes 0x402C38F8). Nothing consumes it. */
        break;

    case Z63_HDMI_DDC_CMD_RECOV1:
    case Z63_HDMI_DDC_CMD_RECOV2:
        /* Bus recovery. What the hardware does is not established; the
         * model does nothing and reports the same failure, which is what
         * makes 0x402C392C give up (see Z63_HDMI_DDC_RESULT_NO_SINK). */
        break;

    default:
        z63_stop_write("HDMI-DDC-CMD",
                       Z63_HDMI_BASE + Z63_HDMI_PAGE_TX * 0x1000 +
                       Z63_HDMI_DDC_CMD, 4, cmd_word);
        return;
    }

    /* The go bit self-clears: 0x402B7010 treats [31:24] == 0 as idle. */
    tx[Z63_HDMI_DDC_CMD / 4] = cmd_word & 0x00ffffffu;

    /* State [7:0] = 0, i.e. not busy (0x402C38A0 tests == 1); result
     * [15:8] (0x402B7458 is UBFX #8,#8). */
    tx[Z63_HDMI_DDC_STAT / 4] =
        (uint32_t)Z63_HDMI_DDC_RESULT_NO_SINK << 8;

    /* The bit the ISR path turns into the completion event. Nothing in the
     * model clears it, because the only reader, 0x402BA2D8, is unreachable
     * in this build. */
    tx[Z63_HDMI_IRQ_A_STAT1 / 4] |= Z63_HDMI_IRQ_A1_DDC_DONE;
}

/* ------------------------------------------------------------------ */
/* Page 1                                                              */
/* ------------------------------------------------------------------ */

static uint64_t z63_hdmi_tx_read(Z63Hdmi *h, hwaddr off, unsigned size)
{
    uint32_t *tx = &h->regs[(Z63_HDMI_PAGE_TX * 0x1000) / 4];

    if (!z63_hdmi_tx_known(off)) {
        z63_stop_read("HDMI-TX",
                      Z63_HDMI_BASE + Z63_HDMI_PAGE_TX * 0x1000 + off, size);
        return 0;
    }

    switch (off) {
    case Z63_HDMI_STAT_CTRL:
        /* Bits [4:0] are hardware status and are not stored; bit 3 is
         * cable presence. The rest reads back what the driver wrote. */
        return (tx[off / 4] & ~(uint32_t)Z63_HDMI_STAT_RO_MASK) |
               (h->hpd ? Z63_HDMI_STAT_HPD : 0);

    /* Six read-only registers whose contents are UNMEASURED. Each is read
     * by exactly one accessor and every one of those accessors has no
     * caller in the image, so nothing in this firmware can observe the
     * answer; 0 is a labelled stand-in, like the board's soc-revision, not
     * a measured value. Reader for each is in docs/hdmi-registers.md 5.5:
     * 0x000 -> 0x402B6D14 (packed id), 0x400..0x410 -> 0x402B7598,
     * 0x4A0/0x4A4 -> 0x402BAB5C (one 56-bit value). */
    case Z63_HDMI_ID:
    case Z63_HDMI_VSTAT0:
    case Z63_HDMI_VSTAT0 + 4:
    case Z63_HDMI_VSTAT0 + 8:
    case Z63_HDMI_VSTAT0 + 12:
    case Z63_HDMI_VSTAT4:
    case Z63_HDMI_RO_A0:
    case Z63_HDMI_RO_A4:
        return 0;

    default:
        /* Everything else reads back the last write. For the ~380
         * registers the driver touches with a read-modify-write that is
         * the only answer consistent with the instruction stream. A word
         * never written reads 0, which is NOT a measured reset value --
         * no reset value in this block was measured. */
        return tx[off / 4];
    }
}

static void z63_hdmi_tx_write(Z63Hdmi *h, hwaddr off, uint64_t value,
                              unsigned size)
{
    uint32_t *tx = &h->regs[(Z63_HDMI_PAGE_TX * 0x1000) / 4];
    hwaddr abs = Z63_HDMI_BASE + Z63_HDMI_PAGE_TX * 0x1000 + off;
    uint32_t v = (uint32_t)value;
    uint32_t w1c;

    if (!z63_hdmi_tx_known(off)) {
        z63_stop_write("HDMI-TX", abs, size, value);
        return;
    }
    if (z63_hdmi_tx_read_only(off)) {
        z63_stop_write("HDMI-TX-RO", abs, size, value);
        return;
    }

    switch (off) {
    case Z63_HDMI_DDC_CMD:
        if (v >> Z63_HDMI_DDC_CMD_SHIFT) {
            z63_hdmi_ddc_start(h, v);
        } else {
            /* The firmware never writes a zero command byte; store it so a
             * readback is still truthful rather than stopping on a write
             * that asks the hardware to do nothing. */
            tx[off / 4] = v;
        }
        return;

    case Z63_HDMI_STAT_CTRL:
        /* Drop the hardware status bits. Every writer preserves them
         * (0x402B763C writes with mask 0xFFFF3CFF, 0x402B8D1C with
         * 0xFFFFBFFF, 0x402BA5C0 does |= 0x2000), so a read-modify-write would otherwise
         * store back whatever the read returned. */
        tx[off / 4] = v & ~(uint32_t)Z63_HDMI_STAT_RO_MASK;
        return;

    case Z63_HDMI_IRQ_B:
        w1c = Z63_HDMI_IRQ_B_W1C;
        break;
    case Z63_HDMI_IRQ_C_STAT:
        w1c = Z63_HDMI_IRQ_C_STAT_W1C;
        break;
    case Z63_HDMI_IRQ_C_EXT:
        w1c = Z63_HDMI_IRQ_C_EXT_W1C;
        break;
    case Z63_HDMI_IRQ_A_STAT0:
    case Z63_HDMI_IRQ_A_STAT1:
        w1c = 0xffffffffu;
        break;

    default:
        tx[off / 4] = v;
        return;
    }

    /* Write-1-to-clear over w1c, plain store elsewhere in the word. The
     * driver always arrives here through a read-modify-write, so the
     * non-w1c bits of `v` are the ones it just read back. */
    tx[off / 4] = (v & ~w1c) | (tx[off / 4] & w1c & ~v);
}

/* ------------------------------------------------------------------ */
/* Pages 0 and 2                                                       */
/* ------------------------------------------------------------------ */

static uint64_t z63_hdmi_page_read(Z63Hdmi *h, unsigned page, hwaddr off,
                                   unsigned size)
{
    const char *name = page == Z63_HDMI_PAGE_CFG ? "HDMI-CFG" : "HDMI-PHY";
    bool known = page == Z63_HDMI_PAGE_CFG ? z63_hdmi_cfg_known(off)
                                           : z63_hdmi_phy_known(off);

    if (!known) {
        z63_stop_read(name, Z63_HDMI_BASE + page * 0x1000 + off, size);
        return 0;
    }
    if (page == Z63_HDMI_PAGE_PHY && z63_hdmi_phy_read_only(off)) {
        /* UNMEASURED, labelled stand-in, exactly as in z63_hdmi_tx_read():
         * read by 0x402BADF0 / 0x402BAF28 / 0x402BAF5C, none of which has a
         * caller anywhere in the image. */
        return 0;
    }
    return h->regs[(page * 0x1000 + off) / 4];
}

static void z63_hdmi_page_write(Z63Hdmi *h, unsigned page, hwaddr off,
                                uint64_t value, unsigned size)
{
    const char *name = page == Z63_HDMI_PAGE_CFG ? "HDMI-CFG" : "HDMI-PHY";
    hwaddr abs = Z63_HDMI_BASE + page * 0x1000 + off;
    bool known = page == Z63_HDMI_PAGE_CFG ? z63_hdmi_cfg_known(off)
                                           : z63_hdmi_phy_known(off);

    if (!known) {
        z63_stop_write(name, abs, size, value);
        return;
    }
    if (page == Z63_HDMI_PAGE_PHY && z63_hdmi_phy_read_only(off)) {
        z63_stop_write("HDMI-PHY-RO", abs, size, value);
        return;
    }
    h->regs[(page * 0x1000 + off) / 4] = (uint32_t)value;
}

/* ------------------------------------------------------------------ */
/* MemoryRegionOps                                                     */
/* ------------------------------------------------------------------ */

static uint64_t z63_hdmi_read(void *opaque, hwaddr addr, unsigned size)
{
    Z63Hdmi *h = opaque;
    unsigned page;
    hwaddr off;

    if (size != 4 || (addr & 3) || addr >= Z63_HDMI_SIZE) {
        z63_stop_read("HDMI", Z63_HDMI_BASE + addr, size);
        return 0;
    }
    page = addr >> 12;
    off = addr & 0xfff;

    if (page == Z63_HDMI_PAGE_TX) {
        return z63_hdmi_tx_read(h, off, size);
    }
    return z63_hdmi_page_read(h, page, off, size);
}

static void z63_hdmi_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    Z63Hdmi *h = opaque;
    unsigned page;
    hwaddr off;

    if (size != 4 || (addr & 3) || addr >= Z63_HDMI_SIZE) {
        z63_stop_write("HDMI", Z63_HDMI_BASE + addr, size, value);
        return;
    }
    page = addr >> 12;
    off = addr & 0xfff;

    if (page == Z63_HDMI_PAGE_TX) {
        z63_hdmi_tx_write(h, off, value, size);
        return;
    }
    z63_hdmi_page_write(h, page, off, value, size);
}

static const MemoryRegionOps z63_hdmi_ops = {
    .read = z63_hdmi_read,
    .write = z63_hdmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* Every MMIO instruction in the driver is LDR Wn / STR Wn; the byte
     * addresses Hex-Rays prints (MEMORY[0x15497A85], MEMORY[0x1549701F],
     * MEMORY[0x15497095], MEMORY[0x15497812], MEMORY[0x15497402]) are
     * sub-fields of a single 32-bit load -- 0x402B7458 is the clear case,
     * LDR W8,[X8]; UBFX W0,W8,#8,#8. So a narrower access is a surprise,
     * and surprises should stop. */
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Reset and wiring                                                    */
/* ------------------------------------------------------------------ */

/* Reset clears the whole register file and drops the cable-present line.
 *
 * NO NON-ZERO RESET VALUE IS SET ANYWHERE IN THIS MODEL, because none was
 * measured. In particular the packed id at 0x15497000 reads 0, the six
 * read-only registers read 0, and the DDC status reads 0 -- which happens
 * to be the state the client's pre-check at 0x402C38A0 wants to see (not
 * busy) before the first transfer. */
static void z63_hdmi_reset(Z63Hdmi *h)
{
    memset(h->regs, 0, sizeof(h->regs));
    h->hpd = false;
}

static void z63_hdmi_init(Object *owner, MemoryRegion *sysmem, Z63Hdmi *h)
{
    z63_hdmi_reset(h);

    /* One region over all three pages, so that an address in a page's
     * unenumerated space still reaches this model and stops with the block
     * name rather than landing in the board's unassigned pool. */
    memory_region_init_io(&h->mr, owner, &z63_hdmi_ops, h,
                          "z63-hdmi", Z63_HDMI_SIZE);
    memory_region_add_subregion(sysmem, Z63_HDMI_BASE, &h->mr);
}

#endif /* HW_ARM_NIKON_Z63_HDMI_H */

/* ------------------------------------------------------------------ */
/* The interrupt: what is missing, and where to add it                 */
/*                                                                     */
/* 0x402B6BE8 is the status-dispatch routine. It calls 0x402BA2D8, which   */
/* reads and clears 0x15497090 / 0x15497094 into a 104-byte struct, and    */
/* hands that struct to MEMORY[0xA028084B0](103, ...). That callback chain */
/* reaches 0x402C34D4, which sets the event flag the DDC wait at           */
/* 0x402C390C is blocked on when struct+96 (0x15497094 bit 6) or           */
/* struct+100 (bit 5) is 1.                                                */
/*                                                                     */
/* 0x402B6BE8 has no code xref, and no 4- or 8-byte pointer to it exists   */
/* anywhere in text, rodata or .data, so it is installed from a table      */
/* built at run time that this pass could not locate. THE SoC INTERRUPT    */
/* LINE THIS BLOCK RAISES IS THEREFORE NOT ESTABLISHED. The candidates     */
/* from rodata 0x41847000 are SPI 70 DISP_DWCH, 71 HDMI_MASTER and 72      */
/* HDMI_CEC, and the board already uses that table's index as its SPI      */
/* number (SPI 173 UART_0_RX, 176 UART_0_TX).                              */
/*                                                                     */
/* Consequence: every DDC transfer costs the guest the firmware's full     */
/* 1000 ms twai_flg timeout and then reports failure. It cannot hang --    */
/* both loops in this block's call graph are bounded (201 ms at            */
/* 0x402C38AC, 501 ms at 0x402D0434).                                      */
/*                                                                     */
/* To add it once the number is known: give Z63Hdmi a `qemu_irq irq;`,     */
/* take it as a parameter of z63_hdmi_init(), and at the end of            */
/* z63_hdmi_ddc_start() raise it when the enable bit is set:               */
/*                                                                     */
/*     if (tx[Z63_HDMI_IRQ_A_EN1 / 4] & Z63_HDMI_IRQ_A1_DDC_DONE) {        */
/*         qemu_irq_pulse(h->irq);                                         */
/*     }                                                                   */
/*                                                                     */
/* That the enable for 0x15497094 is 0x1549709C is established             */
/* (0x402B6D78 writes bit collections at exactly the positions 0x402BA2D8  */
/* reads); that bit 6 is gated by bit 6 of the enable is an inference.     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* How to wire this into hw/arm/nikon-z63.c                            */
/*                                                                     */
/* Four edits, located by text anchor rather than line number because    */
/* other agents are editing the same file.                               */
/*                                                                     */
/* 0. FIRST, in hw/arm/nikon-z63-display.h, remove its refuse-only       */
/*    mapping of this same span, or the two models fight over            */
/*    0x15496000+0x3000. Delete the three lines in z63_display_init()    */
/*    that begin                                                         */
/*                                                                     */
/*        d->refuse[0].name = "DISP-TXQ";                                */
/*                                                                     */
/*    through the memory_region_add_subregion() call for &d->txq, and    */
/*    the `MemoryRegion txq;` field in struct Z63Display. Its            */
/*    Z63_DISP_TXQ_BASE / Z63_DISP_TXQ_SIZE defines can stay; nothing    */
/*    else uses them. If that header has already dropped the block,      */
/*    there is nothing to do here.                                       */
/*                                                                     */
/* 1. After the last #include (the `#include <libfdt.h>` line), add:     */
/*                                                                     */
/*        #include "hw/arm/nikon-z63-hdmi.h"                             */
/*                                                                     */
/*    It has to come before NikonZ63State so the state field below       */
/*    resolves. Order relative to nikon-z63-display.h and                */
/*    nikon-z63-storage.h does not matter: all three forward-declare     */
/*    z63_stop_read / z63_stop_write behind Z63_STOP_HELPERS_DECLARED.   */
/*                                                                     */
/* 2. In `typedef struct NikonZ63State { ... }`, next to the other       */
/*    device groups -- e.g. immediately after the line                   */
/*                                                                     */
/*        Z63Display display;                                           */
/*                                                                     */
/*    that nikon-z63-display.h's own wiring note adds -- add:            */
/*                                                                     */
/*        Z63Hdmi hdmi;                                                 */
/*                                                                     */
/* 3. In machine init, immediately after the line                        */
/*                                                                     */
/*        z63_display_init(OBJECT(machine), sysmem, &s->display);       */
/*                                                                     */
/*    (or, if the display model is not wired yet, next to the other      */
/*    memory_region_add_subregion calls, after the "Driver upload        */
/*    window" block) add:                                                */
/*                                                                     */
/*        /+ HDMI transmitter: DDC/E-DDC master, packet slots, PHY page. */
/*         + See docs/hdmi-registers.md. +/                              */
/*        z63_hdmi_init(OBJECT(machine), sysmem, &s->hdmi);             */
/*                                                                     */
/*    (replace `/+ ... +/` with a real comment; written this way so      */
/*    this note does not close the comment it lives in)                  */
/*                                                                     */
/* z63_hdmi_init() calls z63_hdmi_reset() itself, so there is no fifth   */
/* edit unless the board later grows a machine-level reset hook that     */
/* should also re-zero this register file; in that case call             */
/* z63_hdmi_reset(&s->hdmi) from it.                                     */
/*                                                                     */
/* The span 0x15496000+0x3000 does not overlap anything else the board   */
/* maps, including the main DMA banks at 0x155C0000 / 0x15600000 /       */
/* 0x15680000 and the display complex at 0x1548C000+0x7000.              */
/* ------------------------------------------------------------------ */
