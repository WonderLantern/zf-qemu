/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Nikon Z 6III (EXPEED 7) research machine -- the storage band,
 * 0x17800000 - 0x17900000.
 *
 * This is not a header in the usual sense: it is the storage device model,
 * written as one file so that nikon-z63.c can take it with a single
 * #include and nothing else. It uses z63_stop_read() / z63_stop_write()
 * from that file, so it must be included after they are defined. The exact
 * lines to add are at the bottom.
 *
 * The register map, with the firmware address behind every entry, is in
 * docs/storage-registers.md. Read that first; this file only repeats as
 * much of it as is needed to justify a line of code.
 *
 * ------------------------------------------------------------------
 * WHAT THE BLOCK IS
 * ------------------------------------------------------------------
 *
 * Two SD host controllers using the published SD Host Controller Standard
 * register layout, one of them SD-4.x with the UHS-II extension. The
 * identification is from the code, not from the address or a device tree:
 *
 *   - The image's own interrupt-source name table at VA 0x41847c20 reads
 *     EXS_SDIO_ALL / EXS_SDIO_SD_CARD / EXS_SDIO_PME, then
 *     EXS_UHS2_1_F_SDH30_{ALL,SD_CARD,WAKEUP} and
 *     EXS_UHS2_1_F_SDH40_{ALL,SD_CARD}. F_SDH30 and F_SDH40 are the
 *     Socionext SD Host Controller 3.0 and 4.0 IP names.
 *   - 0x40291254 writes 0x0CDB0000 to 0x1782000C, which as the standard
 *     {Transfer Mode, Command} pair is MAKE_CMD(STOP_TRANSMISSION, R1b |
 *     CRC | INDEX | ABORT) -- an SDHCI command word, verbatim.
 *   - 0x4029122C waits for (~present_state & 0x00F00000) == 0, the
 *     standard DAT[3:0] signal-level field.
 *   - Responses are masked with SD R1 error bit sets (0x073E0000 at
 *     0x4029122C; 0xC7380000, 0xC7180000, 0x21180000 on the byte-swapped
 *     UHS-II response).
 *   - 0x4028ECD4 performs the published UHS-II bring-up: Capabilities bit
 *     35 "UHS-II Supported", Host Control 2 UHS2_INTERFACE_ENABLE and
 *     UHS_MODE_SELECT = 7, VDD2 at 1.8 V, then Present State bit 31
 *     "UHS-II interface detection" and bit 30 "lane synchronization".
 *
 * Nothing here is inferred from the Z 6III Linux device tree. That tree's
 * sdhci@0x17830000 node is NOT in this component -- the image contains no
 * flattened device tree at all and never references 0x17830000.
 *
 * ------------------------------------------------------------------
 * THE THREE WINDOWS, AND THE ONE THING THAT IS NOT PROVEN
 * ------------------------------------------------------------------
 *
 *   0x17820000  "SDIO"   standard register set at +0x000,
 *                        vendor/PHY words at +0x100 .. +0x250.
 *                        Driven by 0x4028d24c-0x4029acfc.
 *   0x17828000  "UHS2L"  five registers: 0x60, 0x78, 0x7C, 0x94, 0xFC.
 *                        Driven only by the UHS-II bring-up/teardown
 *                        (0x4028ecd4, 0x4029b834, 0x402a2bf0, 0x402a2d14).
 *   0x17828200  "SDH40"  standard register set at +0x000 with the UHS-II
 *                        block at +0x080 and vendor words at +0x100.
 *                        Driven by 0x4029b154-0x402a39a8.
 *
 * That the SDH40 set is based at 0x17828200 and not at 0x17828000 is
 * proven, not assumed: +0x2C behaves as Clock Control (bit 0 enable, bit 1
 * stable polled at 0x4028EE68, bit 2 SD clock enable), +0x24 as Present
 * State (Command Inhibit, Space/Data Available, UHS-II detect at bit 31),
 * +0x40 as Capabilities (max block length extracted with UBFX #16,#2 at
 * 0x4029F304), +0x3E as Host Control 2 and +0x80..+0xCC as the UHS-II
 * block. Read as offsets from 0x17828000 those decode as nothing.
 *
 * What is NOT proven is what the 0x17828000 page is. Its 0xFC is read
 * exactly the way the SDIO file's 0xFC is -- as {Slot Interrupt Status,
 * Host Controller Version} gating ADMA2 -- which argues it is the F_SDH30
 * standard register set of the same slot. But its other four registers are
 * not at standard offsets. Both readings are left open here and the four
 * registers are modeled from observed behaviour only.
 *
 * ------------------------------------------------------------------
 * WHAT THIS MODEL DOES AND REFUSES TO DO
 * ------------------------------------------------------------------
 *
 * Every access in the whole band is a 32-bit LDR W / STR W; there is not
 * one byte or halfword access. 16-bit and 8-bit register fields are
 * reached by read-modify-write of the containing word (0x4028EE1C-0x4028EE34
 * preserves bits 16-31 while setting bit 0 of Clock Control). So the model
 * only answers word accesses and stops on anything else.
 *
 * Registers the firmware writes and reads back are plain storage: handing
 * back what the guest itself wrote invents nothing. Registers whose value
 * the *card* or the *link* produces are refused, because there is no card
 * and no link in this machine and a plausible-looking answer would be a
 * fabrication:
 *
 *   Buffer Data Port       refused both ways. A write would make the
 *                          firmware believe data left the host.
 *   Response registers     refused. These are card data.
 *   ADMA Error Status      refused. Only read after an error this model
 *                          never raises.
 *   Capabilities           refused unless caps_stub (see below).
 *   Host Controller Ver.   refused unless caps_stub.
 *   Vendor strap +0x104    refused until written, and the firmware never
 *                          writes it -- so it always stops, at 0x402A2FBC.
 *
 * The per-offset tables below come from the addresses the image actually
 * builds, so they are necessarily incomplete in one specific way: an
 * offset reached as base+offset never appears as a constant. This band
 * does that constantly -- 0x4028EEFC reaches 0x1782823C as
 * [0x17828224 + 0x18], and 0x4029F1E0 reaches 0x17828280 as
 * [0x17828240 + 0x40] -- and the offsets found that way are in the tables,
 * but there is no way to prove no others exist. An offset this model has
 * not seen therefore stops with its PC rather than being absorbed, which
 * is how the gap gets closed. (The board's EMIO comment makes the same
 * point about the 0x0e000000 banks, for the same reason.)
 *
 * Four things ARE answered, each a handshake the firmware itself defines,
 * and each marked DECLARED CHOICE where the model asserts an outcome that
 * real hardware could refuse: internal clock stable, Execute Tuning
 * completion, UHS-II software reset completion, and the two lower-page
 * sequencer status bits. They are listed at their classifiers below.
 *
 * Present State is answered with one explicit vector: an empty slot in an
 * idle controller. That is a declared configuration of the machine, not a
 * measurement of silicon, and it is the only configuration that needs no
 * invented card behaviour. Modeling an inserted card means modeling a card
 * -- QEMU has hw/sd/sd.c for that -- and is deliberately out of scope;
 * until then the firmware takes its own documented no-card paths and then
 * stops the first time it wants card data, which is exactly the lead that
 * should be followed next.
 *
 * ------------------------------------------------------------------
 * NOT MODELED ON PURPOSE
 * ------------------------------------------------------------------
 *
 * 0x17800000, 0x17840000 and 0x17880000 are inside the band but are driven
 * by a code region that shares nothing with the SD HAL: 0x40c5deb4-
 * 0x40c5e934, 0x40c5ea9c-0x40c6023c and 0x40c717ec-0x40c7187c respectively,
 * and not one function touches both. The 0x17880000 one materialises
 * 0x1788C110, the DesignWare USB 3 GCTL offset, and the same interrupt-name
 * table two rows below the SD entries reads EXS_USB3_TYPE_C_CONTROLLER, so
 * that one is a USB 3 host. They are left to the board's existing unmapped
 * trap, which stops with the PC -- a better starting point for them than a
 * storage model pretending to own them.
 *
 * No interrupt is wired. The name table gives eight interrupt names for
 * this band but the image's binding of name to SPI number was not traced,
 * so inventing an SPI would be inventing wiring.
 */
#ifndef HW_ARM_NIKON_Z63_STORAGE_H
#define HW_ARM_NIKON_Z63_STORAGE_H

/* The board's refusal helpers, defined further down the same translation
 * unit in nikon-z63.c (around lines 791 and 801). They are forward-declared
 * here so that this file can be included ABOVE `typedef struct
 * NikonZ63State`, which is where the machine needs Z63StorageState as a
 * member. Nothing in this file is called before those definitions exist. */
static void z63_stop_read(const char *block, hwaddr addr, unsigned size);
static void z63_stop_write(const char *block, hwaddr addr, unsigned size,
                           uint64_t value);

/* Window bases and the span each one's own references bound.
 *
 * SDIO highest referenced offset  0x250 (0x4028D608)  -> 0x300
 * UHS2L highest referenced offset 0x0FC (0x4029B8CC)  -> 0x200
 * SDH40 highest referenced offset 0x118 (0x4029CAB0)  -> 0x200
 *
 * Nothing beyond those spans is mapped, so an access past them stops with
 * its PC through the board's unmapped trap rather than being absorbed. */
#define Z63_SDIO_BASE       0x17820000ULL
#define Z63_SDIO_SIZE       0x300
#define Z63_UHS2L_BASE      0x17828000ULL
#define Z63_UHS2L_SIZE      0x200
#define Z63_SDH40_BASE      0x17828200ULL
#define Z63_SDH40_SIZE      0x200

#define Z63_SD_FILES        3
#define Z63_SD_MAX_WORDS    (Z63_SDIO_SIZE / 4)

/* ------------------------------------------------------------------ */
/* Present State (+0x24 of either standard file).                      */
/*                                                                    */
/* Bit names are from the published SD Host Controller Standard. Only  */
/* the bits listed with a firmware address are ever read by this       */
/* image; the others are named for orientation only.                   */
/*                                                                    */
/*   0  Command Inhibit (CMD)   read 0x40290DCC, 0x402A1E90 (& 1)      */
/*   1  Command Inhibit (DAT)   read 0x40291308 (TST #3)               */
/*  10  Buffer Write Enable     read 0x402998A8, 0x402A1E90 (& 0x400)  */
/*  11  Buffer Read Enable      read 0x4028F7D8, 0x4029F374            */
/*  16  Card Inserted           read 0x4028F104 (TBZ #16)              */
/*  17  Card State Stable       not read                                */
/*  18  Card Detect Pin Level   not read                                */
/*  19  Write Protect Pin Level not read                                */
/* 20-23 DAT[3:0] Signal Level  read 0x4029122C tail                   */
/*  24  CMD Signal Level        not read                                */
/*  29  In Dormant State        read 0x402A2C38 (TBNZ #0x1D)           */
/*  30  Lane Synchronization    read 0x4028F0B8 (TBNZ #0x1E)           */
/*  31  UHS-II If Detection     read 0x4028EF4C (TBNZ #0x1F)           */
/* ------------------------------------------------------------------ */
#define Z63_SD_PS_CMD_INHIBIT    (1u << 0)
#define Z63_SD_PS_DAT_INHIBIT    (1u << 1)
#define Z63_SD_PS_SPACE_AVAIL    (1u << 10)
#define Z63_SD_PS_DATA_AVAIL     (1u << 11)
#define Z63_SD_PS_CARD_INSERTED  (1u << 16)
#define Z63_SD_PS_CARD_STABLE    (1u << 17)
#define Z63_SD_PS_CD_PIN_LEVEL   (1u << 18)
#define Z63_SD_PS_WP_PIN_LEVEL   (1u << 19)
#define Z63_SD_PS_DAT_LEVEL      (0xfu << 20)
#define Z63_SD_PS_CMD_LEVEL      (1u << 24)
#define Z63_SD_PS_DORMANT        (1u << 29)
#define Z63_SD_PS_LANE_SYNC      (1u << 30)
#define Z63_SD_PS_UHS2_DETECT    (1u << 31)

/* DECLARED CONFIGURATION, not measured hardware: an empty slot in an idle
 * controller. Every bit is a decision, so every bit is justified:
 *
 *   CARD_STABLE set, CARD_INSERTED and CD_PIN_LEVEL clear
 *     -- "card detect has settled and there is no card". 0x4028F104
 *        branches on CARD_INSERTED, so this is the branch taken.
 *   WP_PIN_LEVEL set -- in the standard, 1 means "write enabled". Nothing
 *        in this image reads it; set rather than clear so that if some
 *        path starts reading it, it does not see a write-protected card
 *        that was never declared.
 *   DAT_LEVEL all four high -- the idle, pulled-up state of DAT[3:0].
 *        0x4029122C waits for exactly this before it considers the card
 *        not busy, so answering it is required to get past that wait.
 *   CMD_LEVEL set -- same reasoning, idle CMD line.
 *   CMD_INHIBIT, DAT_INHIBIT, SPACE_AVAIL, DATA_AVAIL clear
 *     -- no command in flight and no buffer ready. Clear SPACE_AVAIL and
 *        DATA_AVAIL are what keep the PIO loops at 0x402A3324 / 0x402A3670
 *        out of the Buffer Data Port, which this model refuses.
 *   DORMANT clear -- an idle controller is not dormant. 0x402A2C38 polls
 *        it until set, so that poll times out (250 ms) and the caller
 *        continues anyway, which is what it does on a real timeout too.
 *   LANE_SYNC and UHS2_DETECT clear -- there is no UHS-II card. 0x4028EF4C
 *        reads UHS2_DETECT and, finding it clear, runs the full UHS-II
 *        teardown at 0x4028EF64-0x4028F08C and returns failure; the caller
 *        at 0x4028D608 falls back to the SDIO path. That whole sequence is
 *        real firmware, exercised by this choice.
 */
#define Z63_SD_PRESENT_EMPTY                                           \
    (Z63_SD_PS_CARD_STABLE | Z63_SD_PS_WP_PIN_LEVEL |                  \
     Z63_SD_PS_DAT_LEVEL   | Z63_SD_PS_CMD_LEVEL)

/* Clock Control, the low halfword of +0x2C. */
#define Z63_SD_CLK_INT_EN        (1u << 0)
#define Z63_SD_CLK_INT_STABLE    (1u << 1)
#define Z63_SD_CLK_SD_EN         (1u << 2)
/* Software Reset, the top byte of the same word: bit 24 all, 25 cmd,
 * 26 dat. 0x402912B4 polls bits 25,26 for zero before issuing a command. */
#define Z63_SD_SWRESET_MASK      (7u << 24)

/* Host Control 2 lives in the top halfword of +0x3C, so its bit n is bit
 * n+16 of the word the firmware actually accesses. */
#define Z63_SD_HC2_VDD180        (1u << 19) /* HC2 bit 3,  0x402987C0    */
#define Z63_SD_HC2_DRV_TYPE      (3u << 20) /* HC2 bits 4-5              */
#define Z63_SD_HC2_EXEC_TUNING   (1u << 22) /* HC2 bit 6,  0x4029427C    */
#define Z63_SD_HC2_TUNED_CLK     (1u << 23) /* HC2 bit 7                 */
#define Z63_SD_HC2_UHS2_IF_EN    (1u << 24) /* HC2 bit 8,  0x4028EE10    */
#define Z63_SD_HC2_V4_MODE       (1u << 28) /* HC2 bit 12, 0x4028EF18    */

/* UHS-II Software Reset, low halfword of +0xC0. 0x402A0E3C spins while
 * bit 1 reads 1; 0x4029B764 skips status collection while it is set. */
#define Z63_SD_UHS2_RESET_SD     (1u << 1)

/* ------------------------------------------------------------------ */
/* Capabilities and Host Controller Version: refused unless asked for. */
/*                                                                    */
/* There is no capabilities dump anywhere in the image, so the real    */
/* values are not recoverable. What IS recoverable is which bits the    */
/* firmware tests, and therefore which value is coherent with a model  */
/* that implements PIO and nothing else. Setting caps_stub supplies     */
/* that value and logs it; leaving it clear stops at the first read,    */
/* which is 0x4028FFDC for the SDIO file and 0x4029B8B0 for SDH40.      */
/*                                                                    */
/* Capabilities[31:0] -- every bit a decision:                          */
/*   16-17 Max Block Length = 0 -> 512 bytes. Derived, not chosen: both  */
/*         PIO loops move blocks * 128 dwords (0x402A3324, 0x402A3670),  */
/*         i.e. 512 bytes per block, and 0x4029F304 only ever compares   */
/*         512 << n against the configured block size.                   */
/*   19 ADMA2, 20 ADMA1, 22 SDMA all clear. DECLARED: this model has no  */
/*         descriptor engine, so claiming a DMA mode would promise a     */
/*         transfer it cannot perform. With them clear, 0x4028FF18 and   */
/*         0x4029B834 both return "unavailable" for every mode, the DMA  */
/*         selector stays 0, and every transfer path takes its PIO arm.  */
/*         This is NOT a claim that the silicon lacks ADMA2 -- it plainly */
/*         has an ADMA Error Status register at +0x54.                    */
/*   18 8-bit, 26 VDD 1.8 V capable, and everything else clear:          */
/*         UNMEASURED. 26 is read (0x402954B0, TBZ #0x1A) and the bit==0  */
/*         branch is taken without the model knowing whether that is      */
/*         right. The others are never read.                             */
/*                                                                    */
/* Capabilities[63:32] -- bit 3 only. 0x4028EE9C/0x4028EEA0 reads it as   */
/*   "UHS-II Supported" and 0x4028ECD4 gives up immediately if it is      */
/*   clear. DECLARED set, consistent with the EXS_UHS2_1_F_SDH40_* names  */
/*   in the image's own interrupt table, because it is what lets the      */
/*   bring-up in docs/storage-registers.md 6.3 actually run.             */
/*                                                                    */
/* Host Controller Version at +0xFC is the one value here that the        */
/* firmware itself pins down. 0x4028FF18 requires the Vendor Version byte */
/* at 0xFF to equal 1 for ADMA2 and the Specification Version byte at     */
/* 0xFE to equal 1 for ADMA1, so 0x01010000 is the only word that passes  */
/* both tests. Specification Version 1 is SD Host Controller 2.00. The    */
/* low halfword is Slot Interrupt Status, 0 = no slot interrupt pending.  */
/* The SDH40 side's test is weaker: 0x4029B8D0 only needs >= 0x10000.     */
/* ------------------------------------------------------------------ */
#define Z63_SD_CAPS_LO_STUB   0x00000000u
#define Z63_SD_CAPS_HI_STUB   0x00000008u /* bit 3: UHS-II Supported */
#define Z63_SD_VERSION_STUB   0x01010000u

/* ------------------------------------------------------------------ */
/* Lower-page sequencer status, 0x17828060 and 0x1782807C.             */
/*                                                                    */
/* DECLARED CHOICE. Each of these is read in exactly one situation in   */
/* the whole image -- inside a bounded wait for one specific value --   */
/* and the model answers that wait and claims nothing else. What the    */
/* registers control is not known (docs/storage-registers.md 4, 9).     */
/*                                                                    */
/*   0x60 bit 1  polled for 0 at 0x4028EFAC and 0x402A2CC8, 250 ms.     */
/*               Answered 0, i.e. "the sequencer is not busy".          */
/*   0x7C bit 0  polled for 1 at 0x4028F044, 250 ms.                    */
/*               Answered 1, i.e. "the sequence completed".             */
/*                                                                    */
/* Every other bit of both words reads 0 because nothing reads them.    */
/* Real hardware can fail either wait; this model cannot express that.  */
/* ------------------------------------------------------------------ */
#define Z63_SD_UHS2L_060_VALUE   0x00000000u
#define Z63_SD_UHS2L_07C_VALUE   0x00000001u

typedef enum Z63SdFileKind {
    Z63_SD_FILE_SDIO,
    Z63_SD_FILE_UHS2L,
    Z63_SD_FILE_SDH40,
} Z63SdFileKind;

/* Per-register policy. Everything not given a class below is Z63_SD_STOP:
 * no firmware address builds it, so there is nothing to model and a read
 * of it is a lead, not a value. */
typedef enum Z63SdClass {
    Z63_SD_STOP = 0,     /* refuse both directions                      */
    Z63_SD_CTRL,         /* read-modify-written by the guest: storage    */
    Z63_SD_WO,           /* only ever written: store, refuse reads       */
    Z63_SD_STRAP,        /* never written by the guest: refuse reads     */
    Z63_SD_CLOCK,        /* +0x2c, with the internal-clock handshake     */
    Z63_SD_PRESENT,      /* +0x24, the declared empty-slot vector        */
    Z63_SD_INTSTAT,      /* +0x30, write-1-to-clear                      */
    Z63_SD_HC2,          /* +0x3c, with Execute Tuning self-clearing     */
    Z63_SD_CAPS_LO,      /* +0x40                                        */
    Z63_SD_CAPS_HI,      /* +0x44                                        */
    Z63_SD_VERSION,      /* +0xfc                                        */
    Z63_SD_FIFO,         /* +0x20, Buffer Data Port: always refused      */
    Z63_SD_RESP,         /* response registers: always refused           */
    Z63_SD_UHS2_RESET,   /* +0xc0, bit 1 self-clearing                   */
    Z63_SD_UHS2_ERR,     /* +0xc4, write-1-to-clear                      */
    Z63_SD_SEQ_060,      /* lower page +0x60                             */
    Z63_SD_SEQ_07C,      /* lower page +0x7c                             */
} Z63SdClass;

typedef struct Z63StorageState Z63StorageState;

typedef struct Z63SdFile {
    Z63StorageState *s;
    const char *name;
    hwaddr base;
    uint32_t size;
    Z63SdFileKind kind;
    MemoryRegion region;
    uint32_t words[Z63_SD_MAX_WORDS];
    bool written[Z63_SD_MAX_WORDS];
} Z63SdFile;

struct Z63StorageState {
    Z63SdFile file[Z63_SD_FILES];
    /* Set by -machine storage-caps=stub (see the wiring note at the end).
     * Off by default so an unmodified run stops at the first Capabilities
     * read and says so, instead of being handed a declared value it never
     * asked for. */
    bool caps_stub;
};

/* ------------------------------------------------------------------ */
/* Classifiers. Each case carries the firmware address that justifies   */
/* it; docs/storage-registers.md has the full table and the rest of the */
/* evidence for each one.                                              */
/* ------------------------------------------------------------------ */

static Z63SdClass z63_sdio_class(hwaddr off)
{
    switch (off) {
    case 0x000: return Z63_SD_CTRL;   /* SDMA System Address / Arg 2
                                       * 0x40290144, 0x40293F74           */
    case 0x004: return Z63_SD_CTRL;   /* {Block Size, Block Count}
                                       * 0x40290848, 0x40293F94           */
    case 0x008: return Z63_SD_WO;     /* Argument 1, 32 write sites, never
                                       * read: 0x4028F700, 0x40290570 ...  */
    case 0x00c: return Z63_SD_CTRL;   /* {Transfer Mode, Command}: read at
                                       * 0x40291090 for Command bit 5,
                                       * written 0x0CDB0000 at 0x40291254  */
    case 0x010: case 0x014:
    case 0x018: case 0x01c:
        return Z63_SD_RESP;           /* Response 0-3: 0x402905FC (+30),
                                       * 0x40298E78 for +0x1c              */
    case 0x020: return Z63_SD_FIFO;   /* Buffer Data Port: 0x40299688 write
                                       * loop, 0x40298B98 read             */
    case 0x024: return Z63_SD_PRESENT;
    case 0x028: return Z63_SD_CTRL;   /* {Host Control, Power Control,
                                       * Block Gap, Wakeup}: byte-wise RMW
                                       * at 0x40293C00, bit 8 read back at
                                       * 0x4028FD28                        */
    case 0x02c: return Z63_SD_CLOCK;
    case 0x030: return Z63_SD_INTSTAT;
    case 0x034: return Z63_SD_CTRL;   /* Int Status Enable:  0x4028D698    */
    case 0x038: return Z63_SD_CTRL;   /* Int Signal Enable:  0x40293DCC    */
    case 0x03c: return Z63_SD_HC2;
    case 0x040: return Z63_SD_CAPS_LO;
    case 0x044: return Z63_SD_CAPS_HI;
    case 0x054: return Z63_SD_STRAP;  /* ADMA Error Status, read at
                                       * 0x40299C34, never written         */
    case 0x0fc: return Z63_SD_VERSION;
    case 0x100: return Z63_SD_CTRL;   /* vendor (F_SDH30_AHB_CONFIG's
                                       * offset): sub_402925BC clears bit 6
                                       * by RMW, read at 0x402926d8 with
                                       * the base held as 0x17820004       */
    case 0x108: case 0x10c:
        return Z63_SD_CTRL;           /* vendor, tuning-adjacent:
                                       * 0x4029435C, 0x402942E8            */
    case 0x124: return Z63_SD_CTRL;   /* vendor, |= 0x200 at 0x4028D7DC    */
    case 0x1e0: case 0x1e4:
        return Z63_SD_WO;             /* vendor pair, 0x4028D700:
                                       * 0x1e4 = 0x000F0101 then 0x1e0 = 0 */
    case 0x200: return Z63_SD_WO;     /* vendor gate: 0/1 around the 0x210
                                       * .. 0x250 writes (0x4028D638), and
                                       * 0/0x10000 at 0x4028ED2C           */
    case 0x204: return Z63_SD_CTRL;   /* vendor: 0x4028F8E4, 0x40298F3C    */
    case 0x210: return Z63_SD_CTRL;   /* vendor, |= 2 at 0x40292790        */
    case 0x224:
        /* Was Z63_SD_WO on the one write seen first. The MCU-driven start
         * found the other path in the same init, sub_4028D608 with X22 =
         * 0x178201e0: store 0x1100000, then read-modify-write --
         *
         *     LDR  W9, [X22,#0x44]     ; 0x4028d720, the read that stopped
         *     ORR  W9, W9, #0x20000
         *     STR  W9, [X22,#0x44]     ; then the same again with 0x10
         *
         * -- so it reads back what was written: storage. */
        return Z63_SD_CTRL;
    case 0x230:
        /* Was Z63_SD_WO on the strength of the one write at 0x4028D608.
         * The machine found the other path: sub_4028D2xx holds 0x17820200
         * in a register and read-modify-writes +0x30 of it --
         *
         *     LDR  W8, [X25,#0x30]     ; 0x4028d310, the read that stopped
         *     AND  W8, W8, #0xfffffff0
         *     ORR  W8, W8, #1
         *     STR  W8, [X25,#0x30]     ; 0x4028d31c
         *
         * -- so the low nibble is a field the firmware edits in place and
         * the register reads back. Storage, like any other control word;
         * no meaning is claimed for the nibble.                           */
        return Z63_SD_CTRL;
    case 0x250: return Z63_SD_WO;     /* vendor = 0x00003200, 0x4028D608   */
    default:    return Z63_SD_STOP;
    }
}

static Z63SdClass z63_uhs2l_class(hwaddr off)
{
    switch (off) {
    case 0x060: return Z63_SD_SEQ_060;
    case 0x078: return Z63_SD_WO;     /* enable: 1 at 0x4028EE18 before the
                                       * internal clock, 0 at 0x4028EFD4   */
    case 0x07c: return Z63_SD_SEQ_07C;
    case 0x094: return Z63_SD_CTRL;   /* link/PHY config plus a bit-0
                                       * strobe. 0x4028EF64: = 0, = 0x4050,
                                       * |= 1. 0x402A2D30: = 0x4441 or
                                       * 0x4041 then clears bit 0. Read-
                                       * modify-written, so it must read
                                       * back what was written             */
    case 0x0fc: return Z63_SD_VERSION;
    default:    return Z63_SD_STOP;
    }
}

static Z63SdClass z63_sdh40_class(hwaddr off)
{
    switch (off) {
    case 0x020: return Z63_SD_FIFO;   /* 0x402A3480 write, 0x402A37E4 read */
    case 0x024: return Z63_SD_PRESENT;
    case 0x028: return Z63_SD_CTRL;   /* Power Control = 0x0F at 0x4028EECC
                                       * then |= 0xB000 at 0x4028EEF8, i.e.
                                       * VDD2 on at 1.8 V -- the UHS-II
                                       * supply. 0x4029B5FC loops while
                                       * (word & 0x1100) != 0              */
    case 0x02c: return Z63_SD_CLOCK;
    case 0x030: return Z63_SD_INTSTAT;
    case 0x034: return Z63_SD_CTRL;   /* Int Status Enable, 42 sites       */
    case 0x038: return Z63_SD_CTRL;   /* Int Signal Enable                 */
    case 0x03c: return Z63_SD_HC2;
    case 0x040: return Z63_SD_CAPS_LO;
    case 0x044: return Z63_SD_CAPS_HI;
    case 0x054: return Z63_SD_STRAP;  /* ADMA Error Status, 0x402A3B54     */
    case 0x058: return Z63_SD_CTRL;   /* ADMA System Address: written at
                                       * 0x4029B93C, read back in the error
                                       * decoder 0x402A3AF8                */
    case 0x080: return Z63_SD_CTRL;   /* UHS-II Block Size, always RMW with
                                       * 0x7000 preserved: 0x4029F320      */
    case 0x084: return Z63_SD_WO;     /* UHS-II Block Count: 0x4029F690    */
    case 0x088: case 0x08c: case 0x090:
    case 0x094: case 0x098:
        return Z63_SD_WO;             /* UHS-II Command Packet, 20 bytes
                                       * written as a block and never read:
                                       * 0x402A1F84 .. 0x402A1FB0          */
    case 0x09c: return Z63_SD_WO;     /* {UHS-II Transfer Mode, Command},
                                       * one word at 0x402A0D8C            */
    case 0x0a0: case 0x0a4: case 0x0a8:
    case 0x0ac: case 0x0b0:
        return Z63_SD_RESP;           /* UHS-II Response: 0x4029F22C,
                                       * 0x402A1FDC, 0x402A2544            */
    case 0x0c0: return Z63_SD_UHS2_RESET;
    case 0x0c4: return Z63_SD_UHS2_ERR;
    case 0x0c8: return Z63_SD_CTRL;   /* UHS-II Err Int Status Enable      */
    case 0x0cc: return Z63_SD_CTRL;   /* UHS-II Err Int Signal Enable      */
    case 0x100: return Z63_SD_CTRL;   /* vendor, 0x4029CB90                */
    case 0x104: return Z63_SD_STRAP;  /* vendor strap: bit 6 selects which
                                       * value 0x402A2D14 writes to the
                                       * lower page's 0x94. Read at
                                       * 0x402A2FBC, never written, and
                                       * there is nothing to derive -- so
                                       * this one always stops             */
    case 0x114: return Z63_SD_CTRL;   /* vendor, 0x4029C9EC                */
    case 0x118: return Z63_SD_CTRL;   /* vendor, 0x4029CAB0                */
    default:    return Z63_SD_STOP;
    }
}

static Z63SdClass z63_sd_class(const Z63SdFile *f, hwaddr off)
{
    switch (f->kind) {
    case Z63_SD_FILE_SDIO:  return z63_sdio_class(off);
    case Z63_SD_FILE_UHS2L: return z63_uhs2l_class(off);
    case Z63_SD_FILE_SDH40: return z63_sdh40_class(off);
    default:                return Z63_SD_STOP;
    }
}

/* ------------------------------------------------------------------ */
/* Access handlers                                                     */
/* ------------------------------------------------------------------ */

static uint64_t z63_sd_read(void *opaque, hwaddr off, unsigned size)
{
    Z63SdFile *f = opaque;
    Z63StorageState *s = f->s;
    uint32_t stored;

    if (size != 4 || (off & 3) || off >= f->size) {
        z63_stop_read(f->name, f->base + off, size);
        return 0;
    }
    stored = f->words[off / 4];

    switch (z63_sd_class(f, off)) {
    case Z63_SD_CTRL:
        return stored;

    case Z63_SD_PRESENT:
        /* The declared empty-slot vector. See Z63_SD_PRESENT_EMPTY. */
        return Z63_SD_PRESENT_EMPTY;

    case Z63_SD_CLOCK:
        /* DECLARED CHOICE: Internal Clock Stable reads back 1 once, and
         * only once, the guest has set Internal Clock Enable. This is the
         * handshake at 0x4028EE34 -> 0x4028EE68 (10 ms budget) and
         * 0x4028D784 -> 0x4028D7AC; the model asserts that the clock
         * always locks, which real hardware is free not to do. The
         * Software Reset bits are already cleared at write time, so they
         * read 0 and the poll at 0x402912B4 passes. */
        if (stored & Z63_SD_CLK_INT_EN) {
            return stored | Z63_SD_CLK_INT_STABLE;
        }
        return stored & ~Z63_SD_CLK_INT_STABLE;

    case Z63_SD_INTSTAT:
        /* {Normal, Error} Interrupt Status. Write-1-to-clear storage and
         * nothing else: this model performs no command and no transfer, so
         * it raises no interrupt and this word stays 0. That is not a
         * fabrication, it is the truthful consequence of the model -- the
         * waits in 0x4029B5FC and its five helpers then hit their own
         * timeouts (250-10000 ms) and unwind through their own error
         * paths, which is real firmware. */
        return stored;

    case Z63_SD_HC2:
        /* Execute Tuning is cleared at write time (see the write handler),
         * so a read after a tuning request reports completion. */
        return stored;

    case Z63_SD_CAPS_LO:
        if (!s->caps_stub) {
            z63_stop_read(f->name, f->base + off, size);
            return 0;
        }
        return Z63_SD_CAPS_LO_STUB;

    case Z63_SD_CAPS_HI:
        if (!s->caps_stub) {
            z63_stop_read(f->name, f->base + off, size);
            return 0;
        }
        return Z63_SD_CAPS_HI_STUB;

    case Z63_SD_VERSION:
        if (!s->caps_stub) {
            z63_stop_read(f->name, f->base + off, size);
            return 0;
        }
        return Z63_SD_VERSION_STUB;

    case Z63_SD_UHS2_RESET:
        /* Bit 1 is cleared at write time, so the wait at 0x402A0E3C sees
         * the reset already finished. DECLARED CHOICE: an instantaneous,
         * always-successful reset. */
        return stored;

    case Z63_SD_UHS2_ERR:
        /* UHS-II Error Interrupt Status: write-1-to-clear storage. Stays 0
         * for the same reason as Z63_SD_INTSTAT. */
        return stored;

    case Z63_SD_SEQ_060:
        return Z63_SD_UHS2L_060_VALUE;

    case Z63_SD_SEQ_07C:
        return Z63_SD_UHS2L_07C_VALUE;

    case Z63_SD_STRAP:
        /* Read-only to the firmware, so there is no written value to hand
         * back and no way to derive one. Refuse rather than pick a branch
         * on the guest's behalf. */
        if (!f->written[off / 4]) {
            z63_stop_read(f->name, f->base + off, size);
            return 0;
        }
        return stored;

    case Z63_SD_WO:
        /* Write-only in every path in the image. A read means a path this
         * model has not seen, which is worth stopping for. */
        z63_stop_read(f->name, f->base + off, size);
        return 0;

    case Z63_SD_FIFO:
        /* Buffer Data Port. Answering this would mean inventing card data.
         * The empty-slot Present State keeps both PIO loops away from it,
         * so arriving here means something upstream changed. */
        z63_stop_read(f->name, f->base + off, size);
        return 0;

    case Z63_SD_RESP:
        /* Response registers are card data; see Z63_SD_FIFO. */
        z63_stop_read(f->name, f->base + off, size);
        return 0;

    case Z63_SD_STOP:
    default:
        z63_stop_read(f->name, f->base + off, size);
        return 0;
    }
}

static void z63_sd_write(void *opaque, hwaddr off, uint64_t value,
                         unsigned size)
{
    Z63SdFile *f = opaque;
    uint32_t v = (uint32_t)value;
    uint32_t *reg;

    if (size != 4 || (off & 3) || off >= f->size) {
        z63_stop_write(f->name, f->base + off, size, value);
        return;
    }
    reg = &f->words[off / 4];

    switch (z63_sd_class(f, off)) {
    case Z63_SD_CTRL:
    case Z63_SD_WO:
    case Z63_SD_STRAP:
        *reg = v;
        f->written[off / 4] = true;
        return;

    case Z63_SD_CLOCK:
        /* Store the word but never let a Software Reset bit stick: the
         * firmware sets them and then polls them for zero (0x402912B4),
         * which is the standard self-clearing behaviour. Internal Clock
         * Stable is derived on read, so whatever the guest writes into
         * bit 1 (it writes back what it read, 0x4028EE1C-0x4028EE34) is
         * irrelevant. */
        *reg = v & ~Z63_SD_SWRESET_MASK;
        f->written[off / 4] = true;
        return;

    case Z63_SD_HC2:
        /* DECLARED CHOICE: Execute Tuning completes instantly and
         * successfully. 0x4029427C sets it (together with Sampling Clock
         * Select, in one ORR #0xC00000) and polls it for zero as "tuning
         * finished"; clearing it here answers that poll. Real hardware can
         * finish tuning and leave Sampling Clock Select clear to report
         * failure, which this model cannot express -- it keeps whatever
         * the guest wrote into that bit. */
        *reg = v & ~Z63_SD_HC2_EXEC_TUNING;
        f->written[off / 4] = true;
        return;

    case Z63_SD_INTSTAT:
    case Z63_SD_UHS2_ERR:
        /* Write-1-to-clear. The firmware clears exactly the bit it was
         * waiting on (0x402A32E0 writes 0x10, 0x402A3680 writes 0x20,
         * 0x402996A0 writes 0x10) and 0x402A3AF8 writes back the error
         * bits it has handled. */
        *reg &= ~v;
        f->written[off / 4] = true;
        return;

    case Z63_SD_UHS2_RESET:
        /* DECLARED CHOICE: the UHS-II software reset completes before the
         * next read. 0x402A0E3C spins for up to 250 ms while bit 1 reads
         * 1, so a self-clearing bit answers it; nothing about what the
         * reset actually resets is modeled. */
        *reg = v & ~Z63_SD_UHS2_RESET_SD;
        f->written[off / 4] = true;
        return;

    case Z63_SD_PRESENT:
    case Z63_SD_CAPS_LO:
    case Z63_SD_CAPS_HI:
    case Z63_SD_VERSION:
        /* Read-only in the standard, and never written by this firmware. */
        z63_stop_write(f->name, f->base + off, size, value);
        return;

    case Z63_SD_FIFO:
        /* Accepting a Buffer Data Port write would tell the firmware that
         * data left the host when nothing received it. Refuse. */
        z63_stop_write(f->name, f->base + off, size, value);
        return;

    case Z63_SD_RESP:
    case Z63_SD_SEQ_060:
    case Z63_SD_SEQ_07C:
        /* Never written anywhere in the image. */
        z63_stop_write(f->name, f->base + off, size, value);
        return;

    case Z63_SD_STOP:
    default:
        z63_stop_write(f->name, f->base + off, size, value);
        return;
    }
}

static const MemoryRegionOps z63_sd_ops = {
    .read = z63_sd_read,
    .write = z63_sd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* Every access in the band is a 32-bit LDR W / STR W, so a non-word
     * access is a path this model has not seen: let it reach the handlers,
     * which stop and report it, rather than having QEMU split or widen it
     * silently. */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl  = { .min_access_size = 1, .max_access_size = 8 },
};

/* ------------------------------------------------------------------ */
/* Reset and construction                                              */
/* ------------------------------------------------------------------ */

/* Reset values.
 *
 * Every control and configuration register comes up 0 and is marked
 * unwritten. 0 is NOT measured on this body: it is the reset value the
 * published SD Host Controller Standard gives for the control and
 * configuration registers, and some value has to be supplied because the
 * firmware's first access to most of them is a read-modify-write
 * (0x4028EE10 on Host Control 2, 0x4028EECC on Power Control, 0x4029F320
 * on UHS-II Block Size). The registers where a reset value would be a
 * claim about the hardware instead -- Capabilities, Host Controller
 * Version, the vendor strap, Present State, the data and response ports --
 * do not read from this array at all. */
static void z63_storage_reset(Z63StorageState *s)
{
    for (unsigned i = 0; i < Z63_SD_FILES; i++) {
        memset(s->file[i].words, 0, sizeof(s->file[i].words));
        memset(s->file[i].written, 0, sizeof(s->file[i].written));
    }
}

static void z63_create_storage(Z63StorageState *s, Object *owner,
                               MemoryRegion *sysmem)
{
    static const struct {
        const char *name;
        const char *rname;
        hwaddr base;
        uint32_t size;
        Z63SdFileKind kind;
    } files[Z63_SD_FILES] = {
        { "SDIO",  "z63-sdio",       Z63_SDIO_BASE,  Z63_SDIO_SIZE,
          Z63_SD_FILE_SDIO },
        { "UHS2L", "z63-uhs2-lower", Z63_UHS2L_BASE, Z63_UHS2L_SIZE,
          Z63_SD_FILE_UHS2L },
        { "SDH40", "z63-uhs2-sdh40", Z63_SDH40_BASE, Z63_SDH40_SIZE,
          Z63_SD_FILE_SDH40 },
    };

    z63_storage_reset(s);

    for (unsigned i = 0; i < Z63_SD_FILES; i++) {
        Z63SdFile *f = &s->file[i];

        f->s = s;
        f->name = files[i].name;
        f->base = files[i].base;
        f->size = files[i].size;
        f->kind = files[i].kind;
        assert(f->size / 4 <= Z63_SD_MAX_WORDS);

        memory_region_init_io(&f->region, owner, &z63_sd_ops, f,
                              files[i].rname, f->size);
        memory_region_add_subregion(sysmem, f->base, &f->region);
    }

    if (s->caps_stub) {
        error_report("Z63_STORAGE_CAPS_STUB: Capabilities and Host "
                     "Controller Version are being answered with declared "
                     "values, not measured ones: caps[31:0]=0x%08x "
                     "(PIO only, 512-byte blocks), caps[63:32]=0x%08x "
                     "(UHS-II supported), version=0x%08x (spec 2.00, "
                     "vendor 1 -- the only word 0x4028FF18 accepts). "
                     "See the derivation above Z63_SD_CAPS_LO_STUB in "
                     "nikon-z63-storage.h.",
                     Z63_SD_CAPS_LO_STUB, Z63_SD_CAPS_HI_STUB,
                     Z63_SD_VERSION_STUB);
    }
}

#endif /* HW_ARM_NIKON_Z63_STORAGE_H */

/* ------------------------------------------------------------------ */
/* WIRING -- the exact lines to add to hw/arm/nikon-z63.c              */
/* ------------------------------------------------------------------ */
/*                                                                    */
/* 1. Immediately ABOVE the line                                       */
/*                                                                    */
/*        typedef struct NikonZ63State NikonZ63State;                  */
/*                                                                    */
/*    (around line 427, just after the `#define Z63_ROM_SIZE` line),    */
/*    add:                                                             */
/*                                                                    */
/*        #include "hw/arm/nikon-z63-storage.h"                        */
/*                                                                    */
/*    It must be above the machine struct, because step 2 puts a        */
/*    Z63StorageState inside it. It does not need to be below           */
/*    z63_stop_read/z63_stop_write: those are forward-declared at the   */
/*    top of this file and nothing here runs before they are defined.   */
/*    Anywhere after nikon-z63.c's own #include block works.            */
/*                                                                    */
/* 2. In struct NikonZ63State, next to the other device state (e.g.    */
/*    just above the `uint8_t soc_revision;` pair at the end), add one  */
/*    field:                                                           */
/*                                                                    */
/*        Z63StorageState storage; / * SD host band, 0x17820000 * /     */
/*                                                                    */
/* 3. In z63_init(), beside the other memory_region_add_subregion      */
/*    calls (e.g. after the SPI controller block around line 2896),     */
/*    add two lines:                                                   */
/*                                                                    */
/*        / * SD host controllers (see docs/storage-registers.md). * /  */
/*        z63_create_storage(&s->storage, OBJECT(machine), sysmem);     */
/*                                                                    */
/*    `machine` and `sysmem` are already in scope there.               */
/*                                                                    */
/* 4. OPTIONAL, and only if you want the firmware to get past the      */
/*    first Capabilities read. Mirror the rom-page-stub property:       */
/*                                                                    */
/*    next to z63_prop_get_rom_page_stub (around line 3083):            */
/*                                                                    */
/*        static bool z63_prop_get_storage_caps_stub(Object *obj,        */
/*                                                   Error **errp)       */
/*        {                                                             */
/*            NikonZ63State *s = NIKON_Z63(obj);                        */
/*            return s->storage.caps_stub;                              */
/*        }                                                             */
/*                                                                     */
/*        static void z63_prop_set_storage_caps_stub(Object *obj,        */
/*                                                   bool value,         */
/*                                                   Error **errp)       */
/*        {                                                             */
/*            NikonZ63State *s = NIKON_Z63(obj);                        */
/*            s->storage.caps_stub = value;                             */
/*        }                                                             */
/*                                                                     */
/*    and next to the object_class_property_add_bool for               */
/*    "rom-page-stub" (around line 3160):                               */
/*                                                                     */
/*        object_class_property_add_bool(oc, "storage-caps-stub",        */
/*            z63_prop_get_storage_caps_stub,                           */
/*            z63_prop_set_storage_caps_stub);                          */
/*        object_class_property_set_description(oc, "storage-caps-stub", */
/*            "Answer the SD hosts' Capabilities and Host Controller "   */
/*            "Version registers with declared values instead of "       */
/*            "stopping. The values are derived from what the firmware " */
/*            "tests and are logged at startup; they are not measured "  */
/*            "from the camera. Off by default.");                      */
/*                                                                     */
/*    Then run with -machine storage-caps-stub=on.                      */
/*                                                                     */
/* 5. If the board ever grows a per-device reset path, call              */
/*    z63_storage_reset(&s->storage) from it. Construction already      */
/*    calls it once, so nothing is needed for a cold boot.              */
/*                                                                    */
/* ------------------------------------------------------------------ */
/* WHAT TO EXPECT ON THE FIRST RUN                                     */
/* ------------------------------------------------------------------ */
/*                                                                    */
/* Without storage-caps-stub: the SDIO init at 0x4028D608 runs the      */
/* vendor configuration and the internal-clock handshake, then stops on */
/* the Capabilities read at 0x4028FFDC. That stop is the expected       */
/* result, and the PC in the log is the one named here.                  */
/*                                                                    */
/* With storage-caps-stub=on: the SDIO probe selects PIO, the UHS-II    */
/* bring-up at 0x4028ECD4 runs all the way to the Present State bit 31  */
/* test at 0x4028EF4C, finds no UHS-II card, runs the full teardown and  */
/* returns failure to 0x4028D608, which falls back to the SDIO path.    */
/* That path then stops the first time it wants card data -- a Response  */
/* register read at or near 0x402905FC, or the Present State card-detect */
/* branch at 0x4028F104 taking its no-card arm. Which of those comes     */
/* first has not been predicted here; the log will say.                  */
/* ------------------------------------------------------------------ */
