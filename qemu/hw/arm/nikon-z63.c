// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nikon Z 6III (EXPEED 7) research machine — RTOS side bring-up.
 *
 * TCG-only. Boots the decrypted factory RTOS image ("eg"), which starts at
 * EL3 and drops to EL1 by itself, so rvbar is pointed straight at its entry.
 *
 * The address plan below is not inferred from other bodies: it comes from
 * this project's own analysis (docs/knowledge/z63-memory-map.md,
 * analysis/rtos/00-topology.md) and the Z 6III Linux device trees
 * (analysis/linux/10-devicetree.md):
 *
 *   0x0A00000000  DRAM base (PHYS_OFFSET on the Linux side)
 *   0x0A00000000  RTOS private area (120 MiB)
 *   0x0A07800000  Linux RAM start (not used by this machine yet)
 *   0x40000000    RTOS image window (boot header; module lands at 0x40160000)
 *   0x0821F000    boot mailbox (secondary-core entry points)
 *   0x0D000000    GICv3 distributor
 *   0x0D080000    GICv3 redistributor (four 0x20000 frames, CPU3 at 0x0D0E0000)
 *   0x0D154000    CPU release controller
 *   0x0F000000    nkj-uart console (register map from Nikon's own driver)
 *   0x155C0000    main DMA, bank 0 (banks at 0x15600000, 0x15680000)
 *
 * Everything outside an explicitly modeled region is an "unmodeled" trap:
 * the access is logged with its PC and the VM stops instead of inventing a
 * device response. Iterating on those stops is the modeling workflow.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "target/arm/arm-powerctl.h"
#include "hw/boards.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/boot.h"
#include "hw/loader.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "qobject/qlist.h"
#include "chardev/char-fe.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/gtimer.h"
/* Only for the LI payload's device tree, which ships as an FDT inside the
 * container and has to be patched before it is handed to Linux. QEMU's own
 * system/device_tree.c includes it the same way, and CONFIG_FDT is set in
 * this build (verified in build/config-host.h). */
#include <libfdt.h>

#define TYPE_NIKON_Z63 MACHINE_TYPE_NAME("nikon-z63")
#define NIKON_Z63(obj) OBJECT_CHECK(NikonZ63State, (obj), TYPE_NIKON_Z63)

/* Virtual SoC identity defaults. These are declared board choices, not
 * values measured from this body -- see the note above z63_top_read. The
 * revision nibble is 1 so the BSP variant path ((id >> 28) - 1) stores a
 * plausible variant and the identity getter takes its revision < 3
 * selector branch; neither is asserted to be what real silicon reports. */
#define Z63_SOC_REVISION_DEFAULT 1
#define Z63_SOC_CHIP_ID_DEFAULT  0

/* Physical layout (real values, see header comment) */
#define Z63_GIC_DIST_BASE   0x0D000000ULL
/* Redistributor base from the runtime dump's per-CPU pointer table at
 * 0x40164000: four 0x20000 frames starting at 0xd080000, so CPU3's frame
 * lands at 0xd0e0000 -- which is the base the Linux DTB reports (Linux runs
 * on CPU3). The EL3 boot code wakes CPU3's SGI frame at 0xd140000. */
#define Z63_GIC_REDIST_BASE 0x0D080000ULL
#define Z63_NUM_CPU         4
/* num-irq = internal(32) + SPI lines, a multiple of 32. The image's own
 * interrupt-name table (nikon-z63-spi.h) runs to SPI 278 (VMAP, INTID 310);
 * the Linux DTB only reaches 216, which is what 256 was sized from, and
 * that left the capture blocks' lines (MIPU 0x10d, MVD 0x134, ...) past
 * the SPI range, where QEMU takes a line for a PPI and asserts on it. */
#define Z63_GIC_NUM_IRQ     320

/* GIC GPIO line arithmetic. QEMU's gicv3_set_irq (hw/intc/arm_gicv3.c)
 * gives the layout itself:
 *
 *     [0..N-1]        SPI, delivered as INTID line + 32
 *     [N..N+31]       PPI/SGI for CPU 0
 *     [N+32..N+63]    PPI/SGI for CPU 1   ...
 *
 * with N = num-irq - GIC_INTERNAL, i.e. 320 - 32 = 288 here. So an SPI n
 * is line n (not n + 32), and a PPI p of CPU i is line N + i * 32 + p.
 *
 * Both of those were wrong until a WFI deadlock exposed them: the boot
 * armed the virtual timer at 40179df0 and then every CPU sat in the idle
 * loop forever, because PPI 27 was being delivered on line 59 -- SPI 27,
 * a line nothing raises -- instead of line N + i * 32 + 27. The UART
 * SPI was wrong the same way, off by 32. */
#define Z63_GIC_INTERNAL    32
#define Z63_GIC_SPI(irq)    (irq)
#define Z63_GIC_PPI(cpu, ppi) \
    ((Z63_GIC_NUM_IRQ - Z63_GIC_INTERNAL) + (cpu) * Z63_GIC_INTERNAL + (ppi))

#define Z63_UART_BASE       0x0F000000ULL
#define Z63_UART_SIZE       0x1000
/* The device tree gives serial@f000000 two interrupts, <0 0xAD 4> and
 * <0 0xB0 4>, which is SPI 173 and SPI 176. Nikon's driver requests them
 * as the port's rx irq and tx irq in that order, and the Z8 project's
 * verified Linux /proc/interrupts shows the same pair as INTID 205
 * nkj-uart-rx and INTID 208 nkj-uart-tx (INTID = SPI + 32). */
#define Z63_UART_IRQ_SPI    173  /* RX */
#define Z63_UART_TX_IRQ_SPI 176  /* TX */
#define Z63_UART_PORT_SIZE  0x10 /* UART_PORT_SIZE in the official driver */
/*
 * Three ports, 0x10 apart. The count is the firmware's own: its interrupt
 * name table at rodata 0x41847000 holds UART_0_RX, UART_1_RX, UART_2_RX at
 * SPI 173-175 and UART_0_TX, UART_1_TX, UART_2_TX at 176-178, and nothing
 * for a fourth. The region table gives the band 0x0f000000-0x0f200000.
 *
 * This model used to assert "UART_NR is 1: there is exactly one port, and
 * the firmware's uart_init only ever touches it". The machine falsified it:
 * sub_40C1A604 takes a second port's base out of a descriptor and brings it
 * up -- read-modify-write of the 16-bit UA_FIFO_CONTROL at +0x0c, then
 * UA_CONTROL bit 7, then UA_MODE bit 0 -- which stopped on 0x0f00001c, port
 * 1's +0x0c. The old claim came from the Linux driver's UART_NR, which says
 * how many ports Linux uses, not how many the SoC has.
 */
#define Z63_UART_PORTS      3
#define Z63_UART_RX_FIFO    128  /* the driver's declared fifosize */

/* Boot mailbox: the RTOS writes a 0x0400a151 block here describing each
 * secondary core's entry point, then rings the release controller. The
 * guest itself revealed the layout during boot (magic at +0xfc0, entries at
 * +0xfd8 + (cpu-1)*8) and the Z8 sibling machine models the same 4 KiB page. */
#define Z63_MAILBOX_BASE    0x0821F000ULL
#define Z63_MAILBOX_SIZE    0x1000
#define Z63_BOOT_MAGIC      0x0400A151
#define Z63_MAILBOX_MAGIC   (Z63_MAILBOX_BASE + 0xFC0)
#define Z63_MAILBOX_ENTRY   (Z63_MAILBOX_BASE + 0xFD8)

/* The 0x0820_0000 region: a service page the image does not contain.
 *
 * Once the GIC wiring below was fixed the boot ran far enough to call into
 * 0x08201000, which this machine does not map, and stopped on the fetch.
 * Six veneers in the whole 88 MB module build an address in that region, and
 * all six land in the one page 0x08201000..0x08201fff:
 *
 *   41668468  -> 082012f4      416684d0  -> 08201000
 *   416684a8  -> 08201350      416684e0  -> 082013e0
 *   416684f0  -> 0820102c      41668560  -> 082013f8
 *
 * each of them `adrp x16, #0x8201000 ; add x16, x16, #imm ; br x16`. That is
 * the shape of a call into a fixed service block, not of a relocation.
 *
 * The block is real: the image's own SOLID memory-region table at
 * 0x455c3810 declares [0x08201000, 0x08202000), and 0x455c3830 declares
 * [0x0821e000, 0x0821f000) -- the page below the boot mailbox above. The
 * region-name table next to it (image 0x455c39a8) is the SOLID list
 * OCROM / WorkSRAM / PCIe_mem / CoreSight / SNAP / PERI / UART / DDR / NoC /
 * ISP_APB / ... / EXS / DSP / LOADER / SOLID / SNOR_USER / CSRAM / ... /
 * SUB_PROC / ...
 *
 * But nothing in the image ever populates it. A store there would trap --
 * the region is unmapped -- and no such trap occurred; a read there would
 * trap too, and none occurred before the branch; and the main DMA engine,
 * the one silent writer, performed zero transfers for the whole boot. So
 * the contents come from a stage that runs before `eg`, which is the same
 * boot ROM already documented at Z63_CPU_TABLE_ADDR below.
 *
 * Unmapped is therefore the honest model: the page stays as it is and the
 * boot stops there. Mapping it as RAM would only move the stop to the first
 * instruction of whatever zeros happened to be there.
 *
 * What the boot is doing when it gets there, traced afterwards from three
 * QEMU runs. `-d exec` over the whole boot shows 41668468 executed exactly
 * once, as the last branch before the stop, and its caller is the method at
 * 0x40d70a54 reached through a `blr` from 0x40d6fbd8. That method is short:
 *
 *   add x0, sp, #0xc ; bl 0x40188e90      read a per-CPU/per-die value
 *   w0 = 0x0003000f  ; bl 0x40c012f8      config lookup: 0x40c012f8 indexes
 *                                         a table pointed at by 0xa02c2f350
 *                                         with (id - 0x30000), so 0x3000f is
 *                                         entry 15 of a config table
 *   bl 0x4018c198                         consume both values
 *   loop 313 times calling 0x40c0a0c4     a delay
 *   cmp w19, #1 ; msr daifset, #3         mask IRQ/FIQ
 *   bl 0x41668468 if w19 == 1, else 0x416684a8
 *
 * So this is the reset/shutdown half of the pair: a delay, interrupts off, and
 * a call into the ROM page. It is not an init path that wandered off.
 *
 * Where the argument comes from. 0x40d6fbd8 sits inside a small dispatcher at
 * 0x40d6fb64, which walks a table of 0x308-byte records:
 *
 *   x25 = 0x4408d798 + phase * 0x100
 *   for (x24 = 0; x24 != 7; x24++) {
 *       for (x23 = 0; x23 != 0x100; x23 += 8)      // 32 callbacks per record
 *           if (x25[x23]) x25[x23](phase);         // w0 = phase
 *       x25 += 0x308;
 *   }
 *
 * Each record is 0x100 bytes of callback slots, 0x200 of other data, and a
 * name pointer at +0x300. The seven names are PreparePhase, DataReadPhase,
 * FastbootPhase, ReleasePhase, NonReleasePhase and WaitShutdownPhase twice
 * (the last two share the string "NonReleasePhase" at 0x4408edbb; ReleasePhase
 * is the same address + 3, which is GCC suffix merging, not a second string).
 * The reset method is slot 0 of channel 6, the WaitShutdownPhase channel, for
 * phase 1 and phase 2; phase 0's channel 6 holds 0x40d709fc instead, which is
 * a near-copy of 0x40d70a54 ending in a different veneer. Channel 5 is the
 * other WaitShutdownPhase and reaches 0x416684d0. Every veneer in the page is
 * therefore a different ROM shutdown service, and the page is the reset
 * service block, not a register bank.
 *
 * All three running CPUs reach it, each with its own phase:
 *
 *   -d cpu -dfilter 0x40d6fb64+0x10 gives, at 0x40d6fb64's entry,
 *   CPU1 X00=1, CPU2 X00=2, CPU0 X00=0, and X01 = the target address in all
 *   three, which is the signature of `br x1` rather than a `bl`. So the three
 *   CPUs independently enter the same dispatcher with phases 0, 1 and 2, and
 *   CPU1 finishes its walk first: it reaches channel 6 with w19 == 1, takes the
 *   313-iteration delay, masks interrupts, calls 0x41668468, and the machine
 *   stops on the fetch at 0x082012f4. The other two CPUs are still part-way
 *   through their walks when the machine stops.
 *
 * That last point is the one worth carrying forward: the stop is not a device
 * that failed to answer during init. The image reached a deliberate, orderly,
 * system-wide shutdown -- delay, `daifset #3`, ROM reset call -- executed by
 * every CPU at once, gated on a config value read through 0x40c012f8. Whether
 * that shutdown is this stage's designed terminus (hand back to the ROM to
 * boot the next stage, which this machine does not supply) or the result of a
 * condition still mis-modelled is the open question; the config id 0x0003000f
 * and the value 0x40d70a54 passes to 0x4018c198 are where to look next. */

/* CPU release controller (Z8 sibling: "cpu-release" at 0x0d154000).
 *   +0x044 status: bit N set once CPU N has been released
 *   +0x244 write 1<<N to release CPU N, reading the mailbox above
 *   +0x248 clock control: plain storage (the GPU helper read-modify-writes it) */
#define Z63_RELEASE_BASE    0x0D154000ULL
#define Z63_RELEASE_SIZE    0x400
#define Z63_CPUSTAT_BASE    0x0C86A000ULL /* the cores' WFI status page */
#define Z63_CPUSTAT_SIZE    0x1000

#define Z63_IMAGE_WINDOW_BASE 0x40000000ULL /* RTOS image window (boot header) */
#define Z63_IMAGE_WINDOW_SIZE (128 * MiB)   /* header declares 0x08200000 */
/* The eg module itself is loaded at 0x40160000, not at the window base: the
 * runtime dump has the boot header at 0x40000000 and is byte-identical to the
 * factory module from 0x40160000 onward (verified: 65536/65536 bytes of the
 * first 64 KiB match). The header names 0x40160000 as its load/entry address
 * (three times), so the module is linked for that base and self-references
 * depend on it. */
#define Z63_IMAGE_LOAD_BASE  0x40160000ULL

/*
 * Where the loader leaves the image in DRAM.
 *
 * The firmware's *initial* translation tables do not map all of its text
 * flat. Three windows of it are mapped into DRAM off a base 0xa15c00000,
 * and the tables say so before any firmware code installs them: walking
 * TTBR0_EL1 (0xa04f08000, TCR 0x280193519 -- 4 KiB granule, T0SZ 25) on a
 * running machine gives, inside 0x40000000-0x451f8fff,
 *
 *     0x40000000-0x40167fff   identity
 *     0x40168000-0x40194fff   -> PA 0xa17200000      0x2d000
 *     0x40195000-0x40bfffff   identity
 *     0x40c00000-0x417fffff   -> PA 0xa16600000     0xc00000
 *     0x41800000-0x451f8fff   identity
 *
 * `sub_41189490`, which installs exactly these three windows, and
 * `sub_411893F8`, which bulk-copies 0x182b000 bytes from VA 0x40200000 into
 * the base, are never executed on the way to this state -- so the mapping
 * is the starting condition and the image is *expected to already be in
 * DRAM*. That is the loader's doing: its strings include "Program Load to
 * 0x" next to the serial-NOR and card read paths.
 *
 * The content rule is not guessed. The firmware re-copies the first window
 * itself -- `sub_411896D8` calls memcpy(0xa17200000, 0x40168000, 0x2d000)
 * at 0x4163aa38 -- and a dump of that DRAM afterwards is 45/45 pages
 * byte-identical to the image at the same VA. So each window holds the
 * image's own bytes for its VA, and that is what this places.
 *
 * Declared choice: that the real loader wrote these three windows and
 * nothing else of the image into DRAM. What is unmeasured is whether it
 * also placed the rest (the identity-mapped span needs no copy, so the
 * machine cannot tell) and what it left in the 0xa15c00000+0xa00000 gap
 * below the second window. Turn this off with -machine image-in-dram=off
 * to get the previous behaviour, in which those windows read as zeros and
 * every branch into them fetches 0x00000000.
 */
typedef struct Z63ImagePlacement {
    uint64_t va;        /* the image VA the window covers */
    uint64_t len;
    uint64_t pa;        /* where the firmware's tables expect to find it */
} Z63ImagePlacement;

static const Z63ImagePlacement z63_image_in_dram[] = {
    { 0x40200000ULL, 0x00a00000ULL, 0xa15c00000ULL },
    { 0x40c00000ULL, 0x00c00000ULL, 0xa16600000ULL },
    { 0x40168000ULL, 0x0002d000ULL, 0xa17200000ULL },
};
/* Zf's own static copy/remap consumers, checked against its captured
 * translation tables: model validation E9.
 * Only used by explicit image-in-dram=on, not the Zf reset default.
 * Full capacity prefill is a declared experiment; actual transfers are shorter.
 */
static const Z63ImagePlacement zf_image_in_dram[] = {
    { 0x40200000ULL, 0x00a00000ULL, 0xa15c00000ULL },
    { 0x40c00000ULL, 0x00a00000ULL, 0xa16600000ULL },
    { 0x40168000ULL, 0x00025128ULL, 0xa17000000ULL },
};
#define Z63_IMAGE_ENTRY      Z63_IMAGE_LOAD_BASE
#define Z63_BOOT_HEADER      Z63_IMAGE_WINDOW_BASE

/* DRAM. The size is not guessed: the RTOS builds its memory-region
 * descriptors at 40caa5fc, and that list gives this window twice, in two
 * groups of four, each group naming 0xa00000000 and 0xa20000000 with a
 * size of 0x20000000:
 *
 *   id 12: base a00000000 size 20000000     id 18: same base and size
 *   id 13: base a20000000 size 20000000     id 19: same base and size
 *   id 14: base 800000000 size 20000000     id 1a: same base and size
 *   id 15: base 860000000 size 20000000     id 1b: same base and size
 *
 * The two 0xa00000000 entries are why this is 1 GiB and not 512 MiB. The
 * 0x800000000 pair is a second, 8 GiB-spaced view of the same memory; no
 * access through it has been observed, so it is deliberately NOT mapped --
 * reaching it should stop and say so rather than quietly alias. */
/*
 * DSP local memory, 0x1b000000-0x1b800000.
 *
 * The crank stopped on sequential writes 0x1b200000, +4, +8 ... all from
 * pc 0x04163aa38, which is the image's memcpy -- the firmware is copying a
 * block in, which is what loading a DSP looks like. The driver cluster
 * 0x40c2dee0-0x40c2e340 names 0x1b000000 and 0x1b200000 as bases but an
 * offset scan finds only +0x000 at either, while the third base it names,
 * 0x1b800000, has a real eleven-register file (nikon-z63-dsp1b80.h). A base
 * with no register offsets that memcpy writes through is memory.
 *
 * Declared choice: the extent. 0x1b000000 is where the firmware's own
 * region table starts DSP, and 0x1b800000 is the next base this driver
 * names, so that is the span used here. Nothing measures the real size, and
 * nothing here knows whether this is one memory or two (the two bases are
 * 2 MiB apart). The cost of the choice is specific: .data also holds
 * 0x1b110000 and 0x1b1b0000, which fall inside this span -- if those are
 * registers rather than memory, this RAM hides them instead of stopping,
 * so they are the first thing to suspect if the DSP misbehaves.
 */
#define Z63_DSPMEM_BASE     0x1B000000ULL
#define Z63_DSPMEM_SIZE     0x00800000

/*
 * The other DSP memories: the first 8 MiB of the DSP region's 16 MiB slots.
 *
 * Measured once the XDMAC translated its bus addresses: through its DMA API
 * (sub_40C1B4D8, called back from 0x40caafec) the firmware copies 64 KiB
 * from DRAM to 0x1c200000, 64 KiB to 0x1d200000 and 32 KiB to 0x1f200000
 * -- each at +0x200000 in its 16 MiB slot, which is where sub_40C2DEF8 puts
 * the first DSP's program memory (0x1b200000, its data memory at slot +0).
 * The register files are elsewhere in each slot (0x1c800000, 0x1d800000,
 * 0x1fc00000), so this is a slot layout, not an offset from them: a first
 * guess of "registers - 0x600000" placed the last one at 0x1f600000 and the
 * copy to 0x1f200000 refuted it.
 *
 * DECLARED CHOICE, with the first one's caveat: the 8 MiB extent is the
 * first slot's, itself unmeasured, and a register inside these spans would
 * be hidden instead of stopping. Slot 0x1e000000 has no measured access
 * and stays unmapped.
 */
static const hwaddr z63_dspmem_more[] = {
    0x1C000000ULL, 0x1D000000ULL, 0x1F000000ULL,
};

#define Z63_DRAM_BASE       0xA00000000ULL
#define Z63_DRAM_SIZE       (1024 * MiB)

/* ------------------------------------------------------------------ */
/* Where the LI (Linux) payload goes                                  */
/*                                                                    */
/* Not a guess: the running image's own SOLID region table at         */
/* 0x455C3810 declares its Linux window, and the container's device   */
/* tree declares the same window. See z63_load_li for the full        */
/* derivation, including why the kernel, the initramfs and the device */
/* tree are placed in this order and why nothing here overlaps         */
/* share_mem.                                                          */
/* ------------------------------------------------------------------ */
#define Z63_LI_KERNEL_ADDR  0x0A07800000ULL  /* base of the Linux window */
#define Z63_LI_DTB_ADDR     0x0A08000000ULL  /* 32 MiB up, 2 MiB aligned */
#define Z63_LI_DTB_RESERVE  0x40000          /* room for the patched tree */
#define Z63_LI_INITRD_ADDR  0x0A09000000ULL  /* above the device tree */
#define Z63_LI_BLOCK_SHIFT  11               /* container blocks are 0x800 */

/*
 * share_mem, and why the Linux hand-off waits for it.
 *
 * The RTOS and the camera application talk through a reserved DRAM window
 * the device tree calls `share_mem`. The RTOS builds a named-object
 * directory at its base, and the application attaches to it by checking
 * for the header: `sub_405B08` in LaMainApp does
 * `memcmp(page + 8, "MASTER", 7)` and returns NULL on mismatch, at which
 * point `CIpcPorting_Initialize` prints `...[105], error.` and the whole
 * IPC layer is dead for that run. No IPC means no display mode set, no
 * menus, nothing.
 *
 * On the body the RTOS boots first and has published the directory long
 * before Linux userspace exists. Here the stand-in used to start Linux at
 * machine reset, so Linux -- which reaches userspace in about 3 seconds --
 * always lost the race against an emulated RTOS that needs tens of
 * seconds. Measured: the error appears at 3.1 s, and killing LaMainApp and
 * re-running it after the RTOS is up makes the error disappear entirely.
 *
 * So the hand-off now waits for the firmware's own signal: the appearance
 * of that header. This is still a stand-in for whatever the body really
 * does, but it is ordered by the firmware's progress instead of by machine
 * reset, which is the part that was wrong.
 *
 * The address is the device tree's: /reserved-memory/share_mem, which the
 * li container carries and which a running body confirms at
 * 0xA14C00000. -machine li-wait-ipc=off restores the old immediate
 * hand-off.
 */
#define Z63_SHARE_MEM_BASE  0x0A14C00000ULL
#define Z63_SHARE_MEM_POLL_NS  (10 * 1000 * 1000)   /* 10 ms */

/* The EL3 boot code dereferences a per-CPU pointer table at this address
 * (ldr x9, [0x40164000 + cpu*8]) to find each redistributor's TYPER, then
 * zeroes word 0 there to clear ProcessorSleep. The boot ROM patches the table
 * in after copying the image; the module's own bytes at that offset are
 * unrelated. */
#define Z63_CPU_TABLE_ADDR  0x40164000ULL

/* ------------------------------------------------------------------ */
/* Main DMA ("resources 17a..17f" on the Z8 sibling)                   */
/*                                                                    */
/* Three banks of two channels, channel stride 0x80. Found by trapping */
/* the first unmodeled access during boot: the EL3 code at 0x40160c38 */
/* (fill) and 0x40160b94 (copy) program it before the RTOS main entry. */
/* Register semantics were recovered from our own disassembly and then */
/* matched against the independent Z8 work (n-research,              */
/* z8-qemu/docs/native-main-dma.md), which documents the same protocol */
/* register for register -- including the completion status at +0x08.  */
/*                                                                    */
/*   +00 channel control (write 0 = reset, +10 <- 2 when idle)        */
/*   +04 IRQ mask, +08 IRQ/completion status (bit0 set when finished) */
/*   +14 transfer mode: 0x20 = copy, 0x21 = fill                       */
/*   +18 fill byte (copy requires 0)                                  */
/*   +1c source pitch, +20 destination pitch (bytes per row)          */
/*   +24 row bytes (0 encodes 0x8000), +28 row count (0 encodes 0x4000)*/
/*   +30/+34 source address low 32 / high 4 bits (copy only)          */
/*   +38/+3c destination address low 32 / high 4 bits                 */
/*   +10 write 1 = start                                              */
/* ------------------------------------------------------------------ */

#define Z63_DMA_BANKS       3
#define Z63_DMA_CHANNELS    (Z63_DMA_BANKS * 2)
#define Z63_DMA_BANK_SIZE   0x100 /* two channels of 0x80 */
#define Z63_DMA_CH_STRIDE   0x80
#define Z63_DMA_MAX_SPAN    (64 * MiB)

/* Pin configuration block ("gpio-top"): five banks of 16-bit registers. */
#define Z63_GPIO_TOP_BASE   0x0D151000ULL
#define Z63_GPIO_BANKS      5
#define Z63_GPIO_WORDS      25 /* registers 0x00..0x60 in each bank */

/* TOP block: identity word at offset 0, configuration words above it. */
#define Z63_TOP_BASE        0x0D150000ULL
#define Z63_TOP_SIZE        0x264 /* Zf also uses +0x258/+0x260 */

/* Clock/power configuration bank (CGN in the Linux DTB). */
#define Z63_CGN_BASE        0x0D152000ULL
#define Z63_CGN_SIZE        0x100

/* Peripheral control words, 0x00..0x24. */
#define Z63_PCTL_BASE       0x0D150300ULL
#define Z63_PCTL_WORDS      10

/* Configuration sequencer at 0x0d158000. */
#define Z63_SEQ_BASE        0x0D158000ULL
#define Z63_SEQ_SIZE        0x0A0

/* Identification/diagnostic block at 0x0d153000. */
#define Z63_ID_BASE         0x0D153000ULL
#define Z63_ID_SIZE         0x44

/* Configuration bank at 0x0d157000. */
#define Z63_CFG7_BASE       0x0D157000ULL
#define Z63_CFG7_SIZE       0x800

/* SPI controller at 0x0e700000, one 0x100 window per chip select. */
#define Z63_SPI_BASE        0x0E700000ULL
#define Z63_SPI_STRIDE      0x100
#define Z63_SPI_INST        12


/* Three flat register windows in 0x0e000000..0x0e210000. Each is sized to
 * the span the firmware's own bases bound: E0 by the six bases 0x0e000000 +
 * n*0x1000, E1 by the three bases 0x0e100000 + n*0x10000, E2 by the six
 * bases 0x0e200000 + n*0x100. */
#define Z63_EMIO_E0_BASE    0x0E000000ULL
#define Z63_EMIO_E0_SIZE    0x6000
#define Z63_EMIO_E1_BASE    0x0E100000ULL
#define Z63_EMIO_E1_SIZE    0x30000
#define Z63_EMIO_E2_BASE    0x0E200000ULL
#define Z63_EMIO_E2_SIZE    0x600

/* Two-instance register bank pair at 0x0c850000 and 0x16808000.
 *
 * 40c1b454 is the ninth call of the driver-init chain at 40c13f98. It sets
 * bit 28 of both banks and then walks eight DRAM descriptors at
 * 0x0a0280d388 (0x40 bytes each):
 *
 *   x8  = 0x0c850000  ldr w9, [x8]  ; orr w9, w9, #0x10000000 ; str w9, [x8]
 *   x10 = 0x16808000  ldr w8, [x10] ; orr w8, w8, #0x10000000 ; str w8, [x10]
 *
 * 40c1b598(w0, w1) and 40c1b5d0(w0, w1) reach the same pair through a csel
 * and set a single bit of the same word:
 *
 *   base = (w0 == 0) ? 0x0c850000 : 0x16808000
 *   *(u32 *)base = bfi(*(u32 *)base, w1, 24, 1)     -- bit 24
 *   *(u32 *)base = bfi(*(u32 *)base, w1, 20, 1)     -- bit 20
 *
 * So the two addresses are two instances of one bank, chosen by an index,
 * and the bank's first word carries bit 28 plus at least two bits written
 * by dedicated setters. No meaning is claimed for any of those bits.
 *
 * The per-channel registers sit at 0x0c850010 + n * 0x30, n = 0..7, and
 * 40c1b398 shows that they too are per-instance: it enters with
 * x21 = 0x0c850010, advances x21 by 0x30 and its descriptor cursor by 0x40
 * for eight iterations, and stores the resulting address into descriptor
 * +8. The same function reaches 0x16808000 + n * 0x30 through the madd at
 * 40c1b3f0, which is why 0x16808000 is treated as the second instance
 * rather than as a separate block.
 *
 * What the block is has NOT been established. It is not the IPCU: both
 * device trees put the ipcu node at 0x0c851000 with a size of 0x1000, the
 * page above, and neither device tree of any of the five revisions lists
 * 0x0c850000 or 0x16808000 at all. 0x16807f00, one page below the second
 * instance, is built by 40cab650 and 40cab89c -- outside the init chain --
 * and is included because it falls in the same two pages; it is reached as
 * a base with a scaled index (add x21, x8, w0, uxtw #3), so the entries
 * below it cannot be enumerated either. Modelling therefore follows the
 * auxiliary pair: exactly what the code addresses, plain storage, and no
 * claimed semantics. */
#define Z63_PAIR_A_BASE     0x0C850000ULL
#define Z63_PAIR_A_SIZE     0x1000
#define Z63_PAIR_B_BASE     0x16807000ULL
#define Z63_PAIR_B_SIZE     0x2000
#define Z63_PAIR_INST       2

/* A flat MMIO window: accepted whole, stored word by word, and named in the
 * stop message. Used by the banks whose register offsets cannot be listed. */
typedef struct Z63Win {
    const char *name;
    uint64_t base;
    uint32_t size;
    uint32_t *words;
    MemoryRegion region;
} Z63Win;

/* Video pipeline register array at 0x14000000. */
#define Z63_VPIPE_BASE      0x14000000ULL
#define Z63_VPIPE_STRIDE    0x4000
#define Z63_VPIPE_INST      112
#define Z63_VPIPE_SIZE      ((uint64_t)Z63_VPIPE_INST * Z63_VPIPE_STRIDE)

/* Five sibling blocks at 0x11800000 + n * 0x800000. */
#define Z63_BLK_BASE        0x11800000ULL
#define Z63_BLK_STRIDE      0x800000
#define Z63_BLK_INST        5
#define Z63_BLK_WINDOW      0x10000 /* highest register seen is +0xf018 */
#define Z63_BLK_CAP         0x02c   /* capability word: tested only for zero */
#define Z63_DDR_CHANNELS    4       /* instances 0..3; 4 writes to all */

/* Driver upload window at 0x14c00000. */
#define Z63_UPK_BASE        0x14C00000ULL
#define Z63_UPK_SIZE        0x2000 /* two 0x1ac-byte tables at +0x0000/+0x1000 */

#define Z63_DMA_IRQ_MASK    0x0b

static const hwaddr z63_dma_bases[Z63_DMA_BANKS] = {
    0x155C0000, 0x15600000, 0x15680000,
};

#define Z63_ROM_SIZE        Z63_IMAGE_WINDOW_SIZE

#include "hw/arm/nikon-z63-spi.h"
#include "hw/arm/nikon-z63-regwindow.h"
#include "hw/arm/nikon-z63-z9lab.h"
#include "hw/arm/nikon-z63-stm32.h"
#include "hw/arm/nikon-zf-rng.h"
#include "hw/arm/nikon-zf-ramdump.h"
#include "hw/arm/nikon-zf-gpio-input.h"
#include "hw/arm/nikon-zf-eye.h"
#include "hw/arm/nikon-zf-eye-i2c.h"
#include "hw/arm/nikon-zf-temperature.h"
#include "hw/arm/nikon-zf-display3e.h"
#include "hw/arm/nikon-zf-battery-standin.h"
#include "hw/arm/nikon-z63-storage.h"
#include "hw/arm/nikon-z63-display.h"
#include "hw/arm/nikon-z63-scanout.h"
#include "hw/arm/nikon-z63-toplcd.h"
#include "hw/arm/nikon-z63-hdmi.h"
#include "hw/arm/nikon-z63-usb.h"
#include "hw/arm/nikon-z63-usbhost.h"
#include "hw/arm/nikon-z63-sensorif.h"
#include "hw/arm/nikon-z63-isp-apb.h"
#include "hw/arm/nikon-z63-spibus.h"
#include "hw/arm/nikon-z63-aux.h"
#include "hw/arm/nikon-z63-vpipe-ctl.h"
#include "hw/arm/nikon-z63-blkfam.h"
#include "hw/arm/nikon-z63-ispband.h"
#include "hw/arm/nikon-z63-blk1524.h"
#include "hw/arm/nikon-z63-dev1cf.h"
#include "hw/arm/nikon-z63-codec.h"
#include "hw/arm/nikon-z63-dev1cc.h"
#include "hw/arm/nikon-z63-dev184.h"
#include "hw/arm/nikon-z63-noc.h"
#include "hw/arm/nikon-zf-sd.h"
#include "hw/arm/nikon-zf-sd.h"
#include "hw/arm/nikon-z63-xdmac.h"
#include "hw/arm/nikon-z63-nand.h"
#include "hw/arm/nikon-z63-gfx.h"
#include "hw/arm/nikon-z63-exs1784.h"
#include "hw/arm/nikon-z63-blk15d8.h"
#include "hw/arm/nikon-z63-dsp1c80.h"
#include "hw/arm/nikon-z63-dspipc.h"   /* their mailboxes */
#include "hw/arm/nikon-z63-dsp1b80.h"
#include "hw/arm/nikon-z63-spibus.h"
static uint64_t z63_current_pc(void);
#include "hw/arm/nikon-z63-extint.h"
#include "hw/arm/nikon-z63-hdmac.h"
#include "hw/arm/nikon-z63-i2c.h"
#include "hw/arm/nikon-z63-imu.h"
#include "hw/arm/nikon-z63-touch.h"
#include "hw/arm/nikon-z63-sio.h"
#include "hw/arm/nikon-z63-audio.h"
#include "hw/arm/nikon-z63-mcu.h"
#include "hw/arm/nikon-z63-stm32.h"    /* the MCU itself, as the board's peer */
#include "hw/arm/nikon-z63-mcukeys.h"   /* its buttons */
#include "hw/arm/nikon-z63-lens.h"      /* a synthetic lens on the mount */
#include "hw/arm/nikon-z63-wlsd.h"
#include "hw/arm/nikon-z63-stream154c.h"
#include "hw/arm/nikon-z63-dev199.h"
#include "hw/arm/nikon-zf-isp199-config.h"
#include "hw/arm/nikon-z63-tenc.h"
#include "hw/arm/nikon-z63-tdec.h"
#include "hw/arm/nikon-z63-yuvcomp.h"
#include "hw/arm/nikon-z63-nor.h"
#include "hw/arm/nikon-z63-ispfam.h"

typedef struct NikonZ63State NikonZ63State;

typedef struct Z63DmaBank {
    NikonZ63State *s;
    unsigned bank;
} Z63DmaBank;
/*
 * A handshake wire between the two chips (mcu-wires=). One end is a pin of
 * the body MCU (nikon-z63-stm32.h), the other a bit of one of the EXPEED's
 * GPIO_TOP data words: the firmware's pin descriptors name a port and a
 * bit, and port p's data word is word p of bank 0 (see z63_gpio_read), so
 * a wire is (bank, bit) on that side. The level is whatever the driving
 * chip last put there -- the MCU's ODR for its pins, the EXPEED's own GPIO
 * storage for its ports -- which is all a level-sensitive handshake line
 * needs. It is a wire, not a direction: either side may be the driver, as
 * the two firmwares' own uses decide.
 */
typedef struct Z63McuWire {
    uint8_t a53_bank, a53_bit;
    uint8_t mcu_port, mcu_pin;
    uint8_t level;              /* last level the EXPEED put on it */
    uint8_t named;              /* level changes logged so far */
} Z63McuWire;

/* Each wire's first changes either way are named on stderr, so an edge that
 * never arrives is as visible as one that does. */
#define Z63_MCU_WIRE_NAMED 8

#include "hw/pci-host/designware.h"
#include "hw/pci/pci_host.h"
#include "hw/arm/nikon-zf-pcie.h"

#define Z63_MCU_WIRES_MAX 16


typedef struct NikonZ63State {
    MachineState parent;

    MemoryRegion dram;
    MemoryRegion dspmem;
    MemoryRegion dspmem_more[3];
    MemoryRegion rom;
    MemoryRegion mailbox;
    MemoryRegion boot_header_ram;
    MemoryRegion unmapped;
    ZfPcieConfig zf_pcie;
    DeviceState *zf_pcie_host;
    MemoryRegion zf_dbi, zf_atu, zf_pcie_outbound;
    uint32_t zf_dbi_ro_enable, zf_dbi_speed_control;
    /* The boot ROM's service page. Mapped only when rom_page_stub is set;
     * see the long comment above z63_rompage_read. */
    MemoryRegion rom_page;
    bool rom_page_stub;

    bool uart_drop_reported[Z63_UART_PORTS];

    DeviceState *gic;
    CharFrontend serial;
    MemoryRegion uart_region;
    /* Per port: RX at SPI 173 + n, TX at 176 + n, from the name table. */
    qemu_irq uart_irq[Z63_UART_PORTS];
    qemu_irq uart_tx_irq[Z63_UART_PORTS];
    uint8_t uart_reg[Z63_UART_PORTS][Z63_UART_PORT_SIZE];
    uint8_t uart_rx[Z63_UART_RX_FIFO];
    unsigned uart_rx_head, uart_rx_count;

    /* The LI container to place in the Linux window, if -machine li-image=
     * named one. li_ready records that the bytes and the patched device
     * tree are in DRAM; the two addresses are what a later stage would hand
     * to CPU3. Nothing else consumes them yet. */
    char *li_image;
    bool li_ready;
    bool li_restage; /* explicit diagnostic loader stand-in */
    bool battery_standin; /* explicit synthetic EG report, not battery auth */
    char *mcu_battery_chardev; /* optional USART1 peer, absent by default */
    uint64_t li_kernel_addr;
    uint64_t li_dtb_addr;
    /* Set by -machine cpu3-entry=0xADDR to hand CPU3 to the LI payload at
     * reset instead of leaving it for the RTOS's release controller. See
     * the comment in z63_init. */
    uint64_t cpu3_entry;

    MemoryRegion dma_region[Z63_DMA_BANKS];
    Z63DmaBank dma_bank[Z63_DMA_BANKS];
    qemu_irq dma_irq[Z63_DMA_CHANNELS];
    bool dma_irq_unwired_reported;
    qemu_irq seq_irq;       /* SPI 192 PMU: clock-sequencer completion */
    uint32_t dma_regs[Z63_DMA_CHANNELS][0x48 / 4];

    MemoryRegion int_latch;
    MemoryRegion counter;
    uint32_t int_latch_value;
    uint32_t counter_value;

    MemoryRegion cpu_release;
    uint32_t release_status;
    /* System reset controller in the 0x0D154000 block, three words per
     * window: [0..2] = clear window 0x0D154240 + 4*i, [3..5] = set window
     * 0x0D154340 + 4*i. See the comment above z63_reset_masks. */
    uint32_t reset_word[6];
    MemoryRegion cpustat;        /* 0x0c86a000, see z63_cpustat_read */
    uint32_t cpustat_last;       /* last status read, for the log */
    unsigned cpustat_logged;
    ARMCPU *cpu[Z63_NUM_CPU];
    CPUClusterState a53_cluster;     /* cluster 0; the body MCU is 1 */

    MemoryRegion gpio_top;
    uint16_t gpio_config[Z63_GPIO_BANKS][Z63_GPIO_WORDS];
    unsigned gpio_poll_count[0x100];
    uint32_t gpio_input_level;
    bool gpio_input_level_set;

    MemoryRegion top;
    uint32_t top_words[Z63_TOP_SIZE / 4];

    MemoryRegion cgn;
    uint32_t cgn_words[Z63_CGN_SIZE / 4];

    MemoryRegion pctl;
    uint32_t pctl_words[Z63_PCTL_WORDS];

    MemoryRegion seq;
    uint32_t seq_words[Z63_SEQ_SIZE / 4];
    Z63RegWindow sensorif;
    Z63RegWindow isp_apb_1a0;
    bool seq_done;

    MemoryRegion id;
    MemoryRegion cfg7;
    uint32_t cfg7_words[Z63_CFG7_SIZE / 4];

    Z63Sio sio;                /* serial-bus controllers, nikon-z63-sio.h */
    Z63Audio audio;            /* codec on serial instance 10, -audio.h */
    Z63ExtInt extint;          /* EXT_INT edge detector, nikon-z63-extint.h */
    Z63Mcu mcu;                /* synthetic MCU peer, nikon-z63-mcu.h */
    Z63RegWindow wlsd_vendor;  /* Linux's SDIO host, nikon-z63-wlsd.h */
    Z63RegWindow top_sel;      /* TOP +0x500 */
    Z63RegWindow stream154c;   /* nikon-z63-stream154c.h */
    Z63RegWindow dev199;       /* nikon-z63-dev199.h */
    Z63RegWindow tenc, tenc2;  /* nikon-z63-tenc.h */
    Z63RegWindow tdec, tdec2;  /* nikon-z63-tdec.h */
    Z63RegWindow yuvcomp, codecdec; /* nikon-z63-yuvcomp.h */
    Z63RegWindow ispfam[17];   /* nikon-z63-ispfam.h */

    Z63RegWindow aux;
    Z63RegWindow vpipe_ctl;
    Z63Nand nand;              /* NAND flash controller, 0x17800000 */
    Z63Gfx gfx;                /* graphics engine, 0x15540000 */
    Z63Noc noc;                /* the NoC windows inside vpipe[] */
    Z63RegWindow exs1784;
    Z63RegWindow blk15d8;
    Z63RegWindow dsp1c80[ARRAY_SIZE(z63_dsp1c80)];
    Z63RegWindow dsp1b80;
    Z63RegWindow blkfam;
    Z63RegWindow ispband[ARRAY_SIZE(z63_ispband)];
    Z63RegWindow blk1524;
    Z63RegWindow dev1cf;
    Z63RegWindow codec;
    Z63RegWindow dev1cc[ARRAY_SIZE(z63_dev1cc)];
    Z63RegWindow dev184;         /* nikon-z63-dev184.h */
    Z63RegWindow rszhist;        /* 0x15c00000, see its creation */
    Z63XdmacCtrl xdmac[Z63_XDMAC_CTRLS];

    Z63Win emio[3];
    Z63Hdmac hdmac;            /* 0x0e100000, in place of EMIO-E1 */
    Z63I2c i2c;                /* 0x0e005000, over EMIO-E0's last page */
    Z63Imu imu;                /* the posture sensor on I2C bank 3 */
    Z63Touch touch;            /* the touch panel on I2C bank 0 */
    bool i2c_to_touch[Z63_I2C_BANKS]; /* the bank's addressed device */
    uint32_t emio_e0_words[Z63_EMIO_E0_SIZE / 4];
    uint32_t emio_e1_words[Z63_EMIO_E1_SIZE / 4];
    uint32_t emio_e2_words[Z63_EMIO_E2_SIZE / 4];

    /* The two instances of the 0x0c850000 / 0x16808000 bank pair. */
    Z63Win pair[Z63_PAIR_INST];
    uint32_t pair_a_words[Z63_PAIR_A_SIZE / 4];
    uint32_t pair_b_words[Z63_PAIR_B_SIZE / 4];

    MemoryRegion vpipe_region;
    uint32_t vpipe[Z63_VPIPE_SIZE / 4];

    MemoryRegion sib_region;
    uint32_t sib[Z63_BLK_INST][Z63_BLK_WINDOW / 4];
    bool ddr_req_busy[Z63_DDR_CHANNELS]; /* see z63_ddr_broadcast */

    MemoryRegion upk_region;
    uint32_t upk[Z63_UPK_SIZE / 4];

    Z63StorageState storage;   /* SD host band, 0x17820000 */
    Z63Display display;
    Z63Scanout scanout;
    MemoryRegion work_sram;
    MemoryRegion buffer_ext;
    MemoryRegion snor_user;
    hwaddr snor_report[24];
    unsigned snor_reported;
    uint8_t *snor_data;
    Z63Nor nor;                  /* NOR controller, nikon-z63-nor.h */
    char *snor_user_file;        /* display complex, 0x1548C000 */
    char *loader_file;           /* 0x40000000-0x40160000, the loader */
    char *nand_file;             /* the NAND chip; erased without it */
    bool image_in_dram;          /* place the image where the loader would */
    bool li_wait_ipc;            /* hand Linux over once share_mem is built */
    bool noc_windows;            /* provision the NoC bus windows */
    bool mcu_peer;               /* attach the synthetic MCU peer */
    char *mcu_image;             /* the MCU's own image; nikon-z63-stm32.h */
    char *mcu_pins;              /* MCU input levels to drive at reset */
    char *mcu_spi_peer;          /* which SPI is on which serial instance */
    char *mcu_wires;             /* handshake wires, mcu-wires= */
    Z63McuWire wires[Z63_MCU_WIRES_MAX];
    unsigned n_wires;
    bool wires_parsed;
    struct Z63McuSpiLink *mcu_spi_link;   /* the bridges, when mcu-spi-peer= */
    Z63Stm32 real_mcu;           /* the MCU itself, when mcu_image is set */
    bool real_mcu_built;
    bool storage_caps_stub;      /* SD capability words, declared values */
    bool gfx_skip_unmodelled;    /* nikon-z63-gfx.h, skip_unmodelled */
    char *scanout_dump;          /* host-side PPM of the scanout */
    char *scanout_live;          /* host-side live frame file, ditto */
    char *toplcd_live;           /* the top LCD's, nikon-z63-toplcd.h */
    Z63TopLcd toplcd;            /* on the real MCU's SPI2 */
    char *sio_sink;              /* write-only serial sinks, nikon-z63-sio.h */
    bool audio_codec;            /* nikon-z63-audio.h */
    bool z9_capture;             /* nikon-z63-z9lab.h */
    bool cfexpress;              /* a card on PCIe port 2, ditto */
    bool service_core_events;    /* CPU3's WFE sleeps until a SEV */
    bool zf_ramdump_zeros; /* opt-in diagnostic-only unknown words */
    bool zf_eye_ready, zf_eye_near;
    ZfEyeI2c zf_eye_i2c;
    ZfTemperature zf_temperature;
    ZfDisplay3e zf_display3e;
    bool i2c_to_display3e[4];
    bool i2c_to_temperature[4];
    bool i2c_to_eye[4];
    uint64_t zf_eye_changes;
    bool zf_gpio22_bit8_high; /* explicit one-pin external input */
    bool zf_gpio22_bit8_low; /* explicit SDK card-detect input */
    bool zf_multi_io_counter; /* isolated, default-off functional timer */
    bool zf_sensorif_aux; /* opt-in own-bound AUX0/1, same Z9 object */
    bool zf_sensorif_output_config; /* own15-word configuration island */
    bool zf_sensorif_group; /* two native bitmap/submit interfaces */
    bool zf_sensorif_primary; /* native control/IRQ and ordinary FRONT owner */
    uint64_t zf_sensorif_source_period_ns; /* explicit virtual input,0=off */
    bool zf_isp1a3_config; /* own two-bank core/lane/LUT configuration */
    bool zf_isp199_config; /* complete ordinary199/19a, original Z9 storage */
    bool zf_sensorif_source_stop; /* explicit empty status-source drain */
    bool zf_stream154c_empty; /* three-channel config and explicit empty-only state */
    bool zf_isp192_config; /* complete ordinary192/193, same Z9 owner */
    bool zf_isp_small_config; /* complete195/197/198 ordinary batch */
    bool zf_statistics; /* Z9 functional statistics bring-up */
    char *zf_sd_drive; /* explicit virtual SD backend */
    char *zf_microsd_drive; /* optional SDK PCI mode3 backend */
    bool zf_still; /* shared whole still pipeline, opt-in */
    bool zf_video_primary_source; /* opt-in functional primary Bayer source */
    bool zf_video_primary_exclusive; /* diagnostic primary-only ownership */
    bool zf_video_codec; /* opt-in existing downstream movie models */
    bool zf_video_host_codec; /* explicit host codec diagnostic, not VPU ISA */
    bool zf_video_retained_start; /* existing strict child admission, opt-in */
    bool zf_ccomp_feedback, zf_ccomp_request, zf_ccomp_level;
    QEMUTimer *zf_ccomp_timer;
    uint64_t zf_ccomp_requests, zf_ccomp_responses;

    char *z9_capture_props;
    char *z9_capture_raw;
    char *mcu_keys;              /* held keys, nikon-z63-mcu.h */
    char *touch_points;          /* fingers on the panel, nikon-z63-touch.h */
    char *mcu_rtc;               /* the MCU's calendar at power-on */
    char *power_off;             /* power-hold dropped: shutdown|continue */
    QEMUTimer *li_wait_timer;
    Z63Hdmi hdmi;              /* HDMI transmitter, 0x15496000 */
    Z63Usb usb;                /* DWC3 USB device, 0x17880000 */
    Z63UsbHost *usbhost;       /* the host on its port, nikon-z63-usbhost.h */
    char *usbip;               /* -machine usbip=, the host's USB/IP server */
    bool usb_cable, usb_cable_set; /* -machine usb-cable=, the VBUS pin */
    bool lens_on;                /* -machine lens=, nikon-z63-lens.h */
    char *lens_replies;          /* -machine lens-replies= */
    Z63Lens lens;

    uint8_t soc_revision; /* virtual; exposed as a machine property */
    uint8_t soc_chip_id;  /* virtual; exposed as a machine property */
} NikonZ63State;

#include "hw/arm/nikon-zf-dbi.h"

/* ------------------------------------------------------------------ */
/* Boot ROM preparation.                                              */
/*                                                                    */
/* The image is copied into the RAM window by hand (not via            */
/* load_image_targphys) on purpose: registering the file with the ROM  */
/* loader makes rom_reset re-copy it at every reset, which silently     */
/* wipes the two blocks the boot ROM patches in *after* the copy --     */
/* the header at 0x40000000 and the per-CPU redistributor pointer      */
/* table at 0x40164000.                                                */
/* ------------------------------------------------------------------ */

/* First 0x40 bytes of the window, verbatim from the runtime dump: the 0x30
 * byte header the boot ROM leaves describing the module, plus the
 * 55aa55aa/5555aaaa trailer that follows it. */
static const uint8_t z63_boot_header[0x40] = {
    0x51, 0xa1, 0x00, 0x04, 0x00, 0x00, 0x20, 0x08,
    0x00, 0x00, 0x20, 0x08, 0x00, 0x00, 0x00, 0x00,
    0x90, 0x4a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x16, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x16, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x16, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x55, 0xaa, 0x55, 0xaa, 0x55, 0x55, 0xaa, 0xaa,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ------------------------------------------------------------------ */
/* Main DMA                                                            */
/* ------------------------------------------------------------------ */

/* Anything the recovered protocol does not cover stops the VM instead of
 * being papered over -- same policy as the unmodeled-access trap. */
/*
 * Report what this board does to the interrupt controller.
 *
 * A level-triggered line the model raises and never lowers looks exactly
 * like a guest bug from the outside: a core spins in what appears to be a
 * handler and nothing says why. This prints the first assert and the first
 * deassert of each named line, and the assert count at the end, so a stuck
 * line is visible instead of inferred. It is diagnostics only and changes
 * no behaviour.
 */
typedef struct Z63IrqStat {
    const char *name;
    unsigned asserts;
    bool seen_high, seen_low;
} Z63IrqStat;

static Z63IrqStat z63_irq_stat[16];
static unsigned z63_irq_stat_used;

static void z63_set_irq(qemu_irq line, int level, const char *name)
{
    Z63IrqStat *st = NULL;

    for (unsigned i = 0; i < z63_irq_stat_used; i++) {
        if (!strcmp(z63_irq_stat[i].name, name)) {
            st = &z63_irq_stat[i];
            break;
        }
    }
    if (!st && z63_irq_stat_used < ARRAY_SIZE(z63_irq_stat)) {
        st = &z63_irq_stat[z63_irq_stat_used++];
        st->name = name;
    }
    if (st) {
        if (level) {
            st->asserts++;
            if (!st->seen_high) {
                st->seen_high = true;
                error_report("Z63_IRQ: %s asserted (first time)", name);
            }
        } else if (!st->seen_low) {
            st->seen_low = true;
            error_report("Z63_IRQ: %s deasserted (first time), %u asserts so"
                         " far", name, st->asserts);
        }
    }
    qemu_set_irq(line, level);
}

static void z63_dma_fail(unsigned bank, hwaddr offset, const char *what,
                         uint64_t value)
{
    ARMCPU *cpu = ARM_CPU(current_cpu);
    error_report("Z63_DMA %s bank=0x%08" HWADDR_PRIx " offset=+0x%" HWADDR_PRIx
                 " value=0x%" PRIx64 " pc=0x%09" PRIx64
                 " -- stopping (unsupported transfer?)",
                 what, (hwaddr)z63_dma_bases[bank], offset, value,
                 cpu ? cpu->env.pc : 0);
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, 3);
}

static bool z63_dma_reg(unsigned offset, unsigned size, unsigned *channel,
                        unsigned *reg)
{
    if (size != 4 || offset % 4 || offset >= Z63_DMA_BANK_SIZE) {
        return false;
    }
    *channel = offset / Z63_DMA_CH_STRIDE;
    *reg = offset % Z63_DMA_CH_STRIDE;
    /* The two source-address words are write-only; nothing reads them back. */
    return *reg <= 0x44 && *reg != 0x2c;
}

/* Perform the programmed transfer for real. The Z8 model paces itself in
 * 4096-byte bursts on a timer and publishes completion at the end; both the
 * boot path here and the later HAL path only ever observe completion via
 * +0x10/+0x08, so completing inline is equivalent and deterministic. */
static void z63_dma_start(NikonZ63State *s, unsigned channel, unsigned bank)
{
    uint32_t *r = s->dma_regs[channel];
    bool fill = r[0x14 / 4] == 0x21;
    uint32_t row_bytes = r[0x24 / 4] ? r[0x24 / 4] : 0x8000;
    uint32_t rows = r[0x28 / 4] ? r[0x28 / 4] : 0x4000;
    uint32_t src_pitch = r[0x1c / 4];
    uint32_t dst_pitch = r[0x20 / 4];
    uint64_t src = r[0x30 / 4] | ((uint64_t)r[0x34 / 4] << 32);
    uint64_t dst = r[0x38 / 4] | ((uint64_t)r[0x3c / 4] << 32);
    uint8_t pattern = r[0x18 / 4] & 0xff;
    uint64_t span;
    int row;

    if ((!fill && r[0x14 / 4] != 0x20) ||
        (fill && r[0x18 / 4] > 0xff) ||
        (!fill && r[0x18 / 4] != 0) ||
        !dst_pitch || dst_pitch < row_bytes ||
        (!fill && (!src_pitch || src_pitch < row_bytes)) ||
        (uint64_t)row_bytes * rows > Z63_DMA_MAX_SPAN) {
        z63_dma_fail(bank, 0x14, "unsupported transfer tuple", r[0x14 / 4]);
        return;
    }

    span = (uint64_t)(rows - 1) * dst_pitch + row_bytes;
    uint64_t src_span = (uint64_t)(rows - 1) * src_pitch + row_bytes;
    if (object_dynamic_cast(OBJECT(s), MACHINE_TYPE_NAME("nikon-zf")) &&
        ((!fill && src >= UINT64_C(0x800000000) && src < UINT64_C(0xa00000000) &&
          src + src_span > UINT64_C(0x900000000)) ||
         (dst >= UINT64_C(0x800000000) && dst < UINT64_C(0xa00000000) &&
          dst + span > UINT64_C(0x900000000)))) {
        error_report("ZF_DMA_EXTENT bank=%u ch=%u fill=%u src=%" PRIx64
            " dst=%" PRIx64 " rows=%u row_bytes=%u src_pitch=%u dst_pitch=%u"
            " raw_sizes=%x/%x src_end=%" PRIx64 " dst_end=%" PRIx64 " pc=%" PRIx64,
            bank, channel, fill, src, dst, rows, row_bytes, src_pitch, dst_pitch,
            r[0x24 / 4], r[0x28 / 4], src + src_span, dst + span, z63_current_pc());
    }
    {
        g_autofree uint8_t *buf = g_malloc(row_bytes);

        for (row = 0; row < rows; row++) {
            uint64_t dst_row = dst + (uint64_t)row * dst_pitch;
            uint64_t src_row = src + (uint64_t)row * src_pitch;

            if (fill) {
                memset(buf, pattern, row_bytes);
            } else {
                cpu_physical_memory_read(src_row, buf, row_bytes);
            }
            /* Row at a time; a bad address lands in the unmodeled trap. */
            cpu_physical_memory_write(dst_row, buf, row_bytes);
        }
    }

    (void)span;
    r[0x10 / 4] = 2;
    /*
     * Bit 0 was the model's completion flag. The installed handler tests a
     * different one: sub_40C1878C reads +0x08 and only treats it as a
     * completion when 0x10 is set, then reads +0x0c for the cause. Both are
     * set here -- 0x10 because that is what the handler reads, bit 0 because
     * it was already here and no evidence says to remove it.
     */
    r[0x08 / 4] |= 0x11;
}

static uint64_t z63_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    Z63DmaBank *b = opaque;
    NikonZ63State *s = b->s;
    unsigned channel, reg;

    if (!z63_dma_reg(offset, size, &channel, &reg)) {
        z63_dma_fail(b->bank, offset, "unsupported read", 0);
        return 0;
    }
    return s->dma_regs[b->bank * 2 + channel][reg / 4];
}

/* ------------------------------------------------------------------ */
/* The channel's interrupt follows status & mask.                      */
/*                                                                    */
/* The transfer itself was already modelled: z63_dma_start copies or   */
/* fills and sets the completion bit in +0x08. What was missing is the */
/* line, and without it the firmware's own "main DMA complete phase-1  */
/* hardware and interrupt entry" (0x40C18CE8) registers handlers for   */
/* channels that can never fire, so anything waiting on a DMA event    */
/* flag waits forever even though the bytes arrived. That is where the */
/* boot stopped: nothing labelled ran after it.                        */
/*                                                                    */
/* SPI numbers come from the image's own name table, so they are not a */
/* guess: HDMAC0_0..7 are 149..156, HDMAC1_0..7 157..164, HDMAC2_0..7  */
/* 165..172, and this board models two channels per bank.              */
/*                                                                    */
/* DECLARED CHOICE: the line is driven as a level, asserted while an   */
/* unmasked status bit is set and deasserted when the guest clears it.  */
/* The firmware clears +0x08 write-1-to-clear in its handler, which is  */
/* consistent with a level, but whether the real block pulses instead   */
/* is unmeasured. Masking is taken from +0x04, whose accepted bits the  */
/* board already restricts to Z63_DMA_IRQ_MASK.                        */
/* ------------------------------------------------------------------ */

static void z63_dma_update_irq(NikonZ63State *s, unsigned channel)
{
    const uint32_t *r = s->dma_regs[channel];
    /*
     * The 0x10 term is not gated by the mask at +0x04, because the firmware
     * only ever writes values within 0x0b there and the handler nonetheless
     * tests 0x10. Whether real silicon gates it is unmeasured; this is a
     * DECLARED CHOICE and the handler's own test is the whole evidence.
     */
    bool level = (r[0x08 / 4] & 0x10) != 0 ||
                 (r[0x08 / 4] & r[0x04 / 4] & Z63_DMA_IRQ_MASK) != 0;

    if (s->dma_irq[channel]) {
        z63_set_irq(s->dma_irq[channel], level, "dma");
    } else if (level && !s->dma_irq_unwired_reported) {
        s->dma_irq_unwired_reported = true;
        error_report("Z63_DMA: channel %u would raise its completion "
                     "interrupt, but this controller has no interrupt line "
                     "in the firmware's descriptors -- see the comment at "
                     "the dma_irq wiring. Nothing is delivered.", channel);
    }
}

static void z63_dma_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    Z63DmaBank *b = opaque;
    NikonZ63State *s = b->s;
    unsigned lchannel, reg, channel;
    uint32_t *r;
    bool allowed = value <= UINT32_MAX;

    if (!z63_dma_reg(offset, size, &lchannel, &reg)) {
        z63_dma_fail(b->bank, offset, "unsupported write", value);
        return;
    }
    channel = b->bank * 2 + lchannel;
    r = s->dma_regs[channel];

    switch (reg) {
    case 0x00:
    case 0x10:
        allowed &= value <= 1;
        break;
    case 0x04:
        allowed &= !(value & ~(uint64_t)Z63_DMA_IRQ_MASK);
        break;
    case 0x08:
        /* 0x10 joins the accepted set because the model now raises it; the
         * firmware's own writes here have only ever been within 0x0b. */
        allowed &= !(value & ~(uint64_t)(Z63_DMA_IRQ_MASK | 0x10));
        break;
    case 0x0c:
    case 0x44:
        allowed = false; /* meaning unresolved: never guess */
        break;
    case 0x14:
        allowed &= value == 0x20 || value == 0x21;
        break;
    case 0x18:
        allowed &= value <= 0xff;
        break;
    case 0x1c:
    case 0x20:
        allowed &= value <= 0x8000;
        break;
    case 0x24:
        allowed &= value < 0x8000;
        break;
    case 0x28:
        allowed &= value < 0x4000;
        break;
    case 0x34:
    case 0x3c:
        allowed &= value <= 0xf;
        break;
    case 0x40:
        allowed &= value <= 1; /* written by the HAL, never by boot code */
        break;
    default:
        break; /* full 32-bit pitches and address low words */
    }

    if (!allowed) {
        z63_dma_fail(b->bank, offset, "unsupported value", value);
        return;
    }

    if (reg == 0x10 && value == 1) {
        z63_dma_start(s, channel, b->bank);
        z63_dma_update_irq(s, channel);
    } else if (reg == 0x00) {
        memset(r, 0, 0x48);
        r[0] = value;
        r[0x10 / 4] = 2; /* idle */
        z63_dma_update_irq(s, channel);
    } else if (reg == 0x10) {
        r[reg / 4] = 2; /* idle */
    } else if (reg == 0x08) {
        r[reg / 4] &= ~(uint32_t)value; /* write 1 to clear */
        z63_dma_update_irq(s, channel);
    } else if (reg == 0x04) {
        r[reg / 4] = value;
        z63_dma_update_irq(s, channel);
    } else {
        r[reg / 4] = value;
    }
}

static const MemoryRegionOps z63_dma_ops = {
    .read = z63_dma_read,
    .write = z63_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void z63_create_dma(NikonZ63State *s)
{
    s->seq_irq = qdev_get_gpio_in(s->gic, Z63_GIC_SPI(Z63_SPI_PMU));

    /*
     * IDMAC: this controller's six channels are SPI 76..81.
     *
     * They were wired to HDMAC's lines once, which was wrong, and then left
     * unwired because the controller's own descriptors (flat 0x451fdd58, ids
     * 0x17a..0x17d) carry no interrupt number. The runtime handler table
     * settles it. `sub_40C00704(intid, arg, handler)` stores {handler, arg}
     * at 0xa02c2c750 + intid * 0x18, and dumping that table from a running
     * machine gives 199 installed handlers. Six of them are IDMAC0_0 through
     * IDMAC2_1 -- INTIDs 108..113, SPI 76..81 -- with handlers 0x40c1878c,
     * 0x40c1882c, 0x40c188cc, 0x40c1896c, 0x40c18a0c and 0x40c18aac, one per
     * channel and 0xa0 apart.
     *
     * And the first of them reads this controller:
     *
     *     sub_40C1878C:  v0 = MEMORY[0x155C0008];
     *                    if (v0 & 0x10) { ... MEMORY[0x155C000C] ... }
     *
     * 0x155c0000 is z63_dma_bases[0], so the block this board models as its
     * main DMA is IDMAC, and bank-major channel numbering lines up with the
     * SPI order: bank 0 channels 0 and 1 are SPI 76 and 77, bank 1 is 78 and
     * 79, bank 2 is 80 and 81.
     *
     * This matters well beyond the DMA. The ROM-to-RAM copy in
     * sub_40CAB028 -- the one sub_40D710DC runs before setting ROMRAM's
     * stage-6 bit -- waits on this completion, and ROMRAM's stage 6 is what
     * the whole display and menu lifecycle waits for. See
     * docs/serial-nor-resources.md for that chain.
     */
    for (unsigned ch = 0; ch < Z63_DMA_CHANNELS; ch++) {
        s->dma_irq[ch] = qdev_get_gpio_in(s->gic,
                                          Z63_GIC_SPI(Z63_SPI_IDMAC0_0 + ch));
    }

    for (unsigned bank = 0; bank < Z63_DMA_BANKS; bank++) {
        s->dma_bank[bank].s = s;
        s->dma_bank[bank].bank = bank;
        memory_region_init_io(&s->dma_region[bank], OBJECT(s), &z63_dma_ops,
                              &s->dma_bank[bank], "z63-main-dma",
                              Z63_DMA_BANK_SIZE);
        memory_region_add_subregion(get_system_memory(),
                                    z63_dma_bases[bank],
                                    &s->dma_region[bank]);
    }
    for (unsigned ch = 0; ch < Z63_DMA_CHANNELS; ch++) {
        s->dma_regs[ch][0x10 / 4] = 2; /* idle */
    }
}

/* ------------------------------------------------------------------ */
/* Unmodeled-access trap: log and stop, never invent a response.       */
/* ------------------------------------------------------------------ */

/*
 * The one reader that may read anything: the power-down register dump.
 * Every power-down -- the standby timer running out after a menu was left
 * open is enough -- saves the settings in sub_41003440, whose last call is
 * sub_40C079A0 -> sub_40C077EC: a copy of every region registered with
 * sub_40C076A8 into NOR record 83, which sub_40C07A2C later writes to the
 * card as A:\RAMDMP_E.BIN. Device regions are copied a word at a time by
 * the one load at 0x40c07914, and drivers register whole pages, not the
 * registers they use: the sensor module (sub_4115B74C) device 0x1508 at
 * 0x15080000 (0x3c4 bytes), +0x1000 (0x490), +0x3000 (0x554), +0x4000
 * (0x148), +0x5000 and +0x5400 (0x300 each) and device 0x150c at
 * 0x150c8000 and 0x150cb000 (0x3000 each), 0x150ce000 and 0x150ce400
 * (0x400 each); sub_40FD1ABC device 0x1528 at 0x15280000 (0xfd0).
 *
 * Nothing reads the record back but that copy to the card, so a word the
 * dump reads decides nothing, and stopping there would end every run at
 * its first power-down. That load, and no other, is let through where a
 * window or the unmapped trap would stop, and gets 0. DECLARED: an
 * unmodelled word reads 0 into the dump. The tripwire stays armed for
 * every other instruction. One line per region the dump enters.
 */
#define Z63_RAMDMP_LOAD_PC 0x40c07914ULL

static bool zf_isp199_diagnostic_read(hwaddr addr, unsigned size, uint64_t *value)
{
    ARMCPU *cpu = ARM_CPU(current_cpu);
    Object *machine = OBJECT(qdev_get_machine());
    if (!cpu || !object_dynamic_cast(machine, MACHINE_TYPE_NAME("nikon-zf")) ||
        !zf_ramdump_read_allowed(true, NIKON_Z63(machine)->zf_ramdump_zeros,
                                cpu->env.pc, addr, size)) {
        return false;
    }
    /* Same pre-existing RAMDMP gate; known config comes from its real owner.
     * Unknown words still take the explicitly labelled diagnostic zero path. */
    return z63_z9lab_zf_isp199_read(z63_z9lab, addr, size, value, true);
}

static bool zf_isp192_diagnostic_read(hwaddr addr, unsigned size, uint64_t *value)
{
    ARMCPU *cpu = ARM_CPU(current_cpu);
    Object *machine = OBJECT(qdev_get_machine());
    if (!cpu || !object_dynamic_cast(machine, MACHINE_TYPE_NAME("nikon-zf")) ||
        !zf_ramdump_read_allowed(true, NIKON_Z63(machine)->zf_ramdump_zeros,
                                cpu->env.pc, addr, size)) {
        return false;
    }
    return z63_z9lab_zf_isp192_read(z63_z9lab, addr, size, value, true);
}

static bool z63_ramdmp_load(const char *block, hwaddr addr, unsigned size)
{
    static const char *last_block;
    static hwaddr last;
    ARMCPU *cpu = ARM_CPU(current_cpu);

    Object *machine = OBJECT(qdev_get_machine());
    bool is_zf = object_dynamic_cast(machine, MACHINE_TYPE_NAME("nikon-zf"));

    if (!cpu) {
        return false;
    }
    if (is_zf) {
        if (!zf_ramdump_read_allowed(true, NIKON_Z63(machine)->zf_ramdump_zeros,
                                    cpu->env.pc, addr, size)) {
            return false;
        }
        error_report("ZF_RAMDMP_SYNTHETIC_ZERO: block=%s addr=0x%09" HWADDR_PRIx
                     " size=%u pc=0x%09" PRIx64 " diagnostic-only",
                     block, addr, size, cpu->env.pc);
        return true;
    }
    if (cpu->env.pc != Z63_RAMDMP_LOAD_PC) {
        return false;
    }
    if (block != last_block || addr < last || addr - last >= 0x1000) {
        error_report("Z63_RAMDMP: the power-down register dump reads %s "
                     "from 0x%09" HWADDR_PRIx " -- unmodelled words read 0",
                     block, addr);
    }
    last_block = block;
    last = addr;
    return true;
}

static uint64_t z63_unmapped_read(void *opaque, hwaddr addr, unsigned size)
{
    ARMCPU *cpu = ARM_CPU(current_cpu);
    uint64_t value;

    if (object_dynamic_cast(OBJECT(opaque), MACHINE_TYPE_NAME("nikon-zf")) &&
        zf_pcie_config_read(&NIKON_Z63(opaque)->zf_pcie, addr, size, &value)) {
        return value;
    }
    if (object_dynamic_cast(OBJECT(opaque), MACHINE_TYPE_NAME("nikon-zf")) &&
        addr == 0x17b800b4 && size == 4) {
        return zf_pcie_link_input(NIKON_Z63(opaque));
    }
    if (z63_front_read(addr, size, &value)) {
        return value;
    }
    if (z63_ramdmp_load("unmapped space", addr, size)) {
        return 0;
    }
    error_report("Z63_UNMAPPED read  addr=0x%09" HWADDR_PRIx " size=%u pc=0x%09" PRIx64
                 " -- stopping (unmodeled device?)", addr, size,
                 cpu ? cpu->env.pc : 0);
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, 3);
    return 0;
}

static void z63_unmapped_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    ARMCPU *cpu = ARM_CPU(current_cpu);

    if (object_dynamic_cast(OBJECT(opaque), MACHINE_TYPE_NAME("nikon-zf")) &&
        zf_pcie_config_write(&NIKON_Z63(opaque)->zf_pcie, addr, size, value)) {
        return;
    }
    if (z63_front_write(addr, value, size)) {
        return;
    }
    error_report("Z63_UNMAPPED write addr=0x%09" HWADDR_PRIx " size=%u value=0x%" PRIx64
                 " pc=0x%09" PRIx64 " -- stopping (unmodeled device?)",
                 addr, size, value, cpu ? cpu->env.pc : 0);
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, 3);
}

static const MemoryRegionOps z63_unmapped_ops = {
    .read = z63_unmapped_read,
    .write = z63_unmapped_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

/* Every device model stops the VM on an access it cannot justify, and always
 * reports the offending PC: finding the guest instruction that made the
 * access is what turns a stop into the next device to model. current_cpu is
 * whichever vCPU is executing, so the PC is the one that actually trapped. */
static void z63_stop_read(const char *block, hwaddr addr, unsigned size)
{
    ARMCPU *cpu = ARM_CPU(current_cpu);

    if (z63_ramdmp_load(block, addr, size)) {
        return;
    }
    error_report("Z63 %s unmodeled read  addr=0x%09" HWADDR_PRIx
                 " size=%u pc=0x%09" PRIx64 " -- stopping",
                 block, addr, size, cpu ? cpu->env.pc : 0);
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, 3);
}

static void z63_stop_write(const char *block, hwaddr addr, unsigned size,
                           uint64_t value)
{
    ARMCPU *cpu = ARM_CPU(current_cpu);

    error_report("Z63 %s unmodeled write addr=0x%09" HWADDR_PRIx
                 " size=%u value=0x%" PRIx64 " pc=0x%09" PRIx64
                 " -- stopping", block, addr, size, value,
                 cpu ? cpu->env.pc : 0);
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, 3);
}

/* ------------------------------------------------------------------ */
/* The boot ROM's service page at 0x08201000, as a labelled stub       */
/*                                                                     */
/* STATUS: this is NOT a model of anything. It is the one place where  */
/* the machine deliberately stands in for firmware it cannot have.     */
/* Read the whole comment before enabling it.                          */
/*                                                                     */
/* Why it is needed. The image jumps into this page and the page is    */
/* not in the image, not in the container, and not in any shipped      */
/* component: it is mask ROM inside the SoC. Searched and ruled out    */
/* the three other members of the same container -- ex2010_0200a0.bi   */
/* opens with a table whose entries are 0x080200xx values and does not */
/* decode as AArch64 at 0x1000, vr2010_010000.bi carries a NikonBVR    */
/* magic at +8, _tpj01_0210.bin opens with a jump table -- none of them */
/* is this page. A runtime dump cannot supply it either; nothing reads */
/* it back.                                                            */
/*                                                                     */
/* What is actually known, from the image alone. Exactly six           */
/* trampolines branch into 0x08201000..0x08202000, each of the form    */
/*   adrp x16, #0x8201000 ; add x16, x16, #imm ; br x16                */
/* so each has exactly one purpose and exactly one caller:             */
/*                                                                     */
/*   0x08201000  from 0x40d709e8  in the ROM-service wrapper at        */
/*                                0x40d709dc, right after calls to     */
/*                                0x41186ae4 and 0x40d66d4c            */
/*   0x0820102c  from 0x40d70a44  after daifset #3, as the tail of a   */
/*                                wrapper that then returns             */
/*   0x082012f4  from 0x40d70aa8  the w19==1 arm of the handler at     */
/*                                0x40d70a54. This is NOT a shutdown   */
/*                                path: the function sits in a group-6 */
/*                                service table and returns normally   */
/*                                to 0x40d70aac.                       */
/*   0x08201350  from 0x40d70ab0  the other arm of the same handler    */
/*   0x082013e0  from 0x40c9ce60  the Linux loader, after x2 is set to */
/*                                0x0000000A07800000                   */
/*   0x082013f8  from 0x40c9ce74  that loader's wait loop              */
/*                                                                     */
/* Each is entered with `bl`, so each is a call that is expected to    */
/* return. What any of them does is unknown and is not guessed here.   */
/*                                                                     */
/* What the stub does, and refuses to do. A fetch at one of those six  */
/* offsets returns `ret`, so the call returns to its caller and the    */
/* guest continues; every such entry is logged with the entry, the     */
/* call site and x0..x3, because that log is the whole point -- it is  */
/* how the flow past this page gets discovered. A fetch anywhere else  */
/* in the page still stops the VM the way every other unmodeled access */
/* does, so an entry we do not know about cannot be silently absorbed. */
/* No register is written, no memory is touched, and no value that the */
/* firmware could have read from the ROM is invented. A write to the   */
/* page stops as well.                                                 */
/*                                                                     */
/* It is off unless -machine rom-page=stub asks for it, so an          */
/* unmodified run still stops at the page and says so.                 */
/* ------------------------------------------------------------------ */

/*
 * BUFFER_EXT, 0x840000000-0x900000000, which the region table marks RAM.
 *
 * It was unmapped until the init sequence got far enough to use it: once the
 * XDMAC model let sub_40D710DC past its ROM-to-RAM copy, the kernel at
 * 0x40168a04 wrote 0x8ffffff90 -- the top of this band, the shape of a stack
 * -- and the machine stopped. The region table calls it RAM, so it is RAM
 * here. QEMU allocates it lazily, so the three gigabytes cost nothing until
 * touched.
 */
#define Z63_BUFFER_EXT_BASE 0x840000000ULL
#define Z63_BUFFER_EXT_SIZE 0x0c0000000ULL

/* WorkSRAM, from the region table at .data 0xa02bd2380. */
#define Z63_WORK_SRAM_BASE  0x08200000ULL
#define Z63_WORK_SRAM_SIZE  0x00200000

#define Z63_ROMPAGE_BASE    0x08201000ULL
#define Z63_ROMPAGE_SIZE    0x1000

static const struct {
    uint32_t offset;
    uint64_t caller;
} z63_rom_entries[] = {
    { 0x000,  0x40d709e8 },
    { 0x02c,  0x40d70a44 },
    { 0x2f4,  0x40d70aa8 },
    { 0x350,  0x40d70ab0 },
    { 0x3e0,  0x40c9ce60 },
    { 0x3f8,  0x40c9ce74 },
};

static uint64_t z63_rompage_read(void *opaque, hwaddr offset, unsigned size)
{
    ARMCPU *cpu = ARM_CPU(current_cpu);
    uint64_t pc = cpu ? cpu->env.pc : 0;

    for (size_t i = 0; i < ARRAY_SIZE(z63_rom_entries); i++) {
        if (offset != z63_rom_entries[i].offset) {
            continue;
        }
        if (pc != Z63_ROMPAGE_BASE + offset) {
            /* Not a fetch at the entry, so the stub would be answering a
             * data access with an instruction word. Report rather than do
             * that, because returning 0xd65f03c0 as data would be inventing
             * a ROM value. */
            z63_stop_read("ROMPAGE-DATA", Z63_ROMPAGE_BASE + offset, size);
            return 0;
        }
        error_report("Z63_ROMPAGE_STUB: boot-ROM service at 0x%09" PRIx64
                     " entered from 0x%09" PRIx64 " with"
                     " x0=0x%" PRIx64 " x1=0x%" PRIx64 " x2=0x%" PRIx64
                     " x3=0x%" PRIx64 " -- returning `ret`, which is a"
                     " labelled stand-in, not the ROM's behaviour",
                     (uint64_t)Z63_ROMPAGE_BASE + offset,
                     z63_rom_entries[i].caller,
                     /* xregs, not regs: `regs` is the AArch32 view and
                      * these cores run AArch64, so it printed the wrong
                      * thing (and the wrong width) for every service call. */
                     cpu ? cpu->env.xregs[0] : 0, cpu ? cpu->env.xregs[1] : 0,
                     cpu ? cpu->env.xregs[2] : 0, cpu ? cpu->env.xregs[3] : 0);
        return UINT64_C(0xd65f03c0); /* ret */
    }

    z63_stop_read("ROMPAGE", Z63_ROMPAGE_BASE + offset, size);
    return 0;
}

static void z63_rompage_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    z63_stop_write("ROMPAGE", Z63_ROMPAGE_BASE + offset, size, value);
}

/*
 * The six service entries, kept answering `ret` over the firmware's own page.
 *
 * WorkSRAM is RAM, so sub_40D710DC's copy from 0x40195000 lands and ROMRAM
 * reaches its stage-6 bit -- that part is real progress and this overlay does
 * not touch it. What it does is keep the six known entry words reading as
 * `ret`, which is what `rom-page-stub` has always claimed to do.
 *
 * The reason is the loader. The copied page is genuine code, and it branches
 * into 0x40000000-0x40160000, which the partition table calls the loader and
 * which is in neither the update package nor the NOR dump -- both start at
 * 0x40160000. Entering it reads zeros, which is `udf`, and the kernel logs a
 * fault at 0x40168a04 to an address that is itself bad. Six `ret`s get the
 * machine considerably further than that.
 *
 * So this is a stand-in for a **missing firmware component**, not for mask
 * ROM; the comment above z63_rompage_read used to say the latter and was
 * wrong. Everything outside the six words is the firmware's own page, and
 * turning the option off gives the real page in full. See
 * docs/serial-nor-resources.md.
 */
static bool z63_rompage_entry(hwaddr off)
{
    for (size_t i = 0; i < ARRAY_SIZE(z63_rom_entries); i++) {
        if (off == z63_rom_entries[i].offset) {
            return true;
        }
    }
    return false;
}

static uint64_t z63_rompage_keep_read(void *opaque, hwaddr off, unsigned size)
{
    NikonZ63State *s = opaque;
    const uint8_t *sram = memory_region_get_ram_ptr(&s->work_sram);
    uint64_t v = 0;

    if (size == 4 && !(off & 3) && z63_rompage_entry(off)) {
        return UINT64_C(0xd65f03c0);          /* ret */
    }
    memcpy(&v, sram + (Z63_ROMPAGE_BASE - Z63_WORK_SRAM_BASE) + off, size);
    return v;
}

static void z63_rompage_keep_write(void *opaque, hwaddr off, uint64_t value,
                                   unsigned size)
{
    NikonZ63State *s = opaque;
    uint8_t *sram = memory_region_get_ram_ptr(&s->work_sram);

    memcpy(sram + (Z63_ROMPAGE_BASE - Z63_WORK_SRAM_BASE) + off, &value, size);
}

static const MemoryRegionOps z63_rompage_keep_ops = {
    .read = z63_rompage_keep_read,
    .write = z63_rompage_keep_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static const MemoryRegionOps z63_rompage_ops = {
    .read = z63_rompage_read,
    .write = z63_rompage_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* nkj-uart, modelled from Nikon's own driver                          */
/*                                                                     */
/* The register map is not inferred. It is the published source:       */
/* nkj_uart.c out of the official OSS release for these bodies (kept   */
/* at reference-from-z8-research/nkj_uart.c, Copyright 2018 Nikon),    */
/* 0x10 bytes per port -- which is also what the device tree says,     */
/* `reg = <0 0xf000000 0 0x10>`, and what UART_PORT_SIZE means.        */
/*                                                                     */
/* The previous stand-in answered every read with 0x62 and treated     */
/* every offset below 9 as a transmit register. Both were wrong: the   */
/* data register is +0x04, and the firmware never writes it at all.    */
/*                                                                     */
/*   0x00 UA_MODE          ENABLE = bit0                               */
/*   0x01 UA_CONTROL       TX_ENABLE=0 RX_ENABLE=1 TX_INT_IDLE=2       */
/*                         TX_INT=3 RX_INT=4 RESET=7                   */
/*   0x02 UA_CONFIG        CS8..CS9, ODD_PARITY=3, PARITY_ENABLE=4      */
/*   0x03 UA_STATUS        TX_BUSY=0 TX_EMPTY=1 RX_READY=2             */
/*                         OVERRUN_ERR=3 FRAME_ERR=4 PARITY_ERR=5      */
/*                         ERR_CLR=7                                   */
/*   0x04 UA_DATA          transmit (driver uses writew) / receive     */
/*   0x06/0x07 UA_BAUD     16-bit, divisor = uartclk / baud - 1        */
/*   0x0C UA_FIFO_CONTROL  16-bit; FIFO1_ENABLE=0 FIFO2_ENABLE=1       */
/*                         FIFO1_RESET=2 FIFO2_RESET=3                 */
/*                         TX_INT_ENABLE=9 TX_REQUESTED=10             */
/*                         RX_IDLE_ENABLE=11                           */
/*   0x0E UA_FIFO_BYTE     high byte = capacity, low byte = occupancy  */
/*                                                                     */
/* Every config write is a latch, and that is not a shortcut: the       */
/* driver read-modify-writes UA_CONTROL (start_tx sets TX_ENABLE and    */
/* TX_INT, stop_tx clears TX_INT) and reads UA_FIFO_CONTROL back        */
/* before OR-ing into it, so those two must return what was written.    */
/*                                                                     */
/* Transmit is instantaneous here, so TX_BUSY reads 0 and TX_EMPTY      */
/* reads 1 -- which is what both the driver's spin loops and the        */
/* firmware's own +0x03 poll are waiting for. The baud register is      */
/* kept but not interpreted: the driver computes 120000000/115200 - 1   */
/* = 1040 = 0x410 itself and simply stores it, and the firmware's       */
/* uart_init writes exactly that same 0x410, so nothing in the guest    */
/* depends on the model knowing the clock.                             */
/*                                                                     */
/* The TX interrupt is a level the driver clears itself: start_tx sets  */
/* TX_INT, TX_INT_ENABLE and TX_REQUESTED, the ISR drains the buffer,   */
/* and stop_tx clears TX_INT and TX_INT_ENABLE, so the line falls       */
/* again and cannot storm. Without it the tty write path would never    */
/* drain. Kernel printk does not need it at all: nkj_console_putchar    */
/* polls UA_STATUS and writes UA_DATA directly.                        */
/*                                                                     */
/* RX is a 128-byte FIFO, which is the fifosize the driver declares.    */
/* RX_READY is level, and the driver's rx_chars loop clears it by       */
/* reading UA_DATA, so the line falls when the FIFO empties. A byte     */
/* arriving with no room sets OVERRUN_ERR, the bit the driver already   */
/* watches for.                                                        */
/*                                                                     */
/* One write is deliberately left unexplained rather than guessed at:   */
/* the firmware's uart_init does `+0x03 <- 1`, writing TX_BUSY into     */
/* UA_STATUS, while the official driver only ever writes ERR_CLR there. */
/* What bit 0 means on a status write is not documented anywhere, so    */
/* it is accepted and given no effect.                                 */
/*                                                                     */
/* Console output needs no interrupt; console *input* does -- and the   */
/* device tree is in the way. `console = "disabled"` makes nkj_uart_    */
/* probe set is_real_serial = false, which skips request_irq entirely   */
/* and turns start_tx into a discard. nkj_console_putchar ignores that  */
/* flag, so TX still works and RX silently does not. A device tree      */
/* carrying that property has to have it removed before a prompt can    */
/* be answered, which is presumably what the Z8 project ended up doing  */
/* to get RX and TX counts in /proc/interrupts.                        */
/* ------------------------------------------------------------------ */

static void z63_uart_update_irqs(NikonZ63State *s, unsigned port)
{
    uint8_t ctl = s->uart_reg[port][0x01];
    uint16_t fc = s->uart_reg[port][0x0c] |
                  ((uint16_t)s->uart_reg[port][0x0d] << 8);
    /* Only port 0 has a backend, so only port 0 can have a byte waiting. */
    bool rx = port == 0 && s->uart_rx_count != 0;

    /* RX: held while a byte is waiting and the driver asked for it. */
    z63_set_irq(s->uart_irq[port],
                rx && (ctl & 0x02) && (ctl & 0x10),
                port == 0 ? "uart0-rx" :
                port == 1 ? "uart1-rx" : "uart2-rx");

    /* TX: held while the driver wants room. It clears both enables in
     * stop_tx, which is what makes this self-terminating. */
    z63_set_irq(s->uart_tx_irq[port],
                (ctl & 0x08) && (fc & 0x0200) && (fc & 0x0400),
                port == 0 ? "uart0-tx" :
                port == 1 ? "uart1-tx" : "uart2-tx");
}

static int z63_uart_can_receive(void *opaque)
{
    NikonZ63State *s = opaque;
    return (int)sizeof(s->uart_rx) - (int)s->uart_rx_count;
}

static void z63_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    NikonZ63State *s = opaque;

    for (int i = 0; i < size; i++) {
        if (s->uart_rx_count == sizeof(s->uart_rx)) {
            s->uart_reg[0][0x03] |= 0x08; /* UA_STATUS_OVERRUN_ERR */
            continue;
        }
        s->uart_rx[(s->uart_rx_head + s->uart_rx_count) % sizeof(s->uart_rx)] =
            buf[i];
        s->uart_rx_count++;
    }
    z63_uart_update_irqs(s, 0);
}

static void z63_uart_event(void *opaque, QEMUChrEvent event)
{
}

static uint64_t z63_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;

    unsigned port = addr / Z63_UART_PORT_SIZE;
    hwaddr off = addr % Z63_UART_PORT_SIZE;

    if (port >= Z63_UART_PORTS) {
        z63_stop_read("UART", Z63_UART_BASE + addr, size);
        return 0;
    }

    switch (off) {
    case 0x03: {
        uint8_t st = s->uart_reg[port][0x03] & ~0x01; /* TX_BUSY never true */

        st |= 0x02;                             /* TX_EMPTY: instant TX */
        if (port == 0 && s->uart_rx_count) {
            st |= 0x04;
        } else {
            st &= ~0x04;
        }
        return st;
    }
    case 0x04:
        if (port != 0 || !s->uart_rx_count) {
            /* The driver only reads the data register once RX_READY is
             * set, so this is an access the hardware would not see. No
             * meaning is claimed for the value. Ports 1 and 2 have no
             * backend, so RX_READY is never set for them. */
            return 0;
        }
        {
            uint8_t ch = s->uart_rx[s->uart_rx_head % sizeof(s->uart_rx)];

            s->uart_rx_head = (s->uart_rx_head + 1) % sizeof(s->uart_rx);
            s->uart_rx_count--;
            z63_uart_update_irqs(s, 0);
            qemu_chr_fe_accept_input(&s->serial);
            return ch;
        }
    case 0x0e:
        /* Capacity in the high byte, occupancy in the low byte. Transmit
         * is instantaneous, so nothing is ever queued. */
        return (uint64_t)s->uart_reg[port][0x0f] << 8;
    default:
        return s->uart_reg[port][off];
    }
}

/* The PC of whichever core is making the current access, for diagnostics. */
static uint64_t z63_current_pc(void)
{
    CPUState *cs = current_cpu;

    return cs ? ARM_CPU(cs)->env.pc : 0;
}

static void z63_uart_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    NikonZ63State *s = opaque;
    uint8_t v = value & 0xff;

    unsigned port = addr / Z63_UART_PORT_SIZE;
    hwaddr off = addr % Z63_UART_PORT_SIZE;

    if (port >= Z63_UART_PORTS) {
        z63_stop_write("UART", Z63_UART_BASE + addr, size, value);
        return;
    }

    switch (off) {
    case 0x01:
        if (v & 0x80) {
            /* UA_CONTROL_RESET is "reset & disable all" in the driver, and
             * it is treated as a command bit: the bit reads back zero.
             *
             * It clears the block except UA_MODE (+0x00). Clearing that too
             * was the first choice here, on the grounds that it could not be
             * observed from either guest; that turned out to be wrong and is
             * the reason this comment exists. The RTOS resets the port from
             * 0x4017a014 while the LI payload holds the console, and the LI
             * nkj driver never writes UA_MODE again -- it inherits a port the
             * RTOS has already enabled. With UA_MODE cleared, every console
             * character the kernel wrote to +0x04 was dropped by the transmit
             * gate below, so Linux booted to userspace with a completely
             * silent console. Preserving UA_MODE across the reset is the
             * conservative choice: it keeps a port that one guest enabled
             * usable by the other. Which registers the real block actually
             * clears is still unmeasured. */
            uint8_t mode = s->uart_reg[port][0x00];

            memset(s->uart_reg[port], 0, Z63_UART_PORT_SIZE);
            s->uart_reg[port][0x00] = mode;
            break;
        }
        s->uart_reg[port][0x01] = v;
        break;
    case 0x03:
        if (v & 0x80) {
            s->uart_reg[port][0x03] &= ~0x38; /* UA_STATUS_ERR_CLR */
        }
        /* Bit 0 is written by the firmware's uart_init and has no
         * documented meaning on a status write. No effect is claimed. */
        break;
    case 0x04:
        if (!(s->uart_reg[port][0x00] & 0x01)) {
            /* UA_MODE_ENABLE gates transmit. */
            if (!s->uart_drop_reported[port]) {
                s->uart_drop_reported[port] = true;
                error_report("Z63_UART%u: transmit of 0x%02x from pc=0x%"
                             PRIx64 " dropped because UA_MODE_ENABLE is clear"
                             " (mode=0x%02x control=0x%02x)", port,
                             (unsigned)(v & 0xff), z63_current_pc(),
                             s->uart_reg[port][0x00], s->uart_reg[port][0x01]);
            }
        } else if (port == 0) {
            uint8_t ch = v;

            qemu_chr_fe_write(&s->serial, &ch, 1);
        } else if (!s->uart_drop_reported[port]) {
            /*
             * Ports 1 and 2 have no backend. On a body they go somewhere --
             * the name table says they exist and sub_40C1A604 enables one --
             * but what is on the far end is not known here, and this machine
             * offers one chardev. Discarding the byte is a DECLARED CHOICE:
             * it is what a port with nothing attached does for transmit, and
             * it is reported once per port so a guest that is talking to a
             * peripheral this way cannot do it unnoticed.
             */
            s->uart_drop_reported[port] = true;
            error_report("Z63_UART%u: transmit of 0x%02x from pc=0x%" PRIx64
                         " discarded -- this port has no backend, see the "
                         "comment in z63_uart_write", port,
                         (unsigned)(v & 0xff), z63_current_pc());
        }
        break;
    default:
        s->uart_reg[port][off] = v;
        break;
    }

    z63_uart_update_irqs(s, port);
}

static const MemoryRegionOps z63_uart_ops = {
    .read = z63_uart_read,
    .write = z63_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    /* The driver mixes readb/writeb with readw/writew, and UA_BAUD and
     * UA_FIFO_CONTROL are 16-bit registers reached as two byte halves.
     * Doing this at byte granularity lets the core split any access. */
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

/* ------------------------------------------------------------------ */
/* GICv3                                                               */
/* ------------------------------------------------------------------ */

static void z63_create_gic(NikonZ63State *s, int num_cpu)
{
    DeviceState *gicdev = qdev_new(TYPE_ARM_GICV3);
    QList *redist;
    int i;

    s->gic = gicdev;
    qdev_prop_set_uint32(gicdev, "revision", 3);
    qdev_prop_set_uint32(gicdev, "num-cpu", num_cpu);
    qdev_prop_set_uint32(gicdev, "num-irq", Z63_GIC_NUM_IRQ);
    qdev_prop_set_bit(gicdev, "has-security-extensions", true);

    /* One redistributor region covering every CPU */
    redist = qlist_new();
    qlist_append_int(redist, num_cpu);
    qdev_prop_set_array(gicdev, "redist-region-count", redist);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(gicdev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gicdev), 0, Z63_GIC_DIST_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(gicdev), 1, Z63_GIC_REDIST_BASE);

    for (i = 0; i < num_cpu; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        static const int timer_irq[] = {
            [GTIMER_PHYS]     = 30, /* NS EL1 physical */
            [GTIMER_VIRT]     = 27,
            [GTIMER_HYP]      = 26,
            [GTIMER_SEC]      = 29, /* S EL1 physical  */
            [GTIMER_S_EL2_PHYS] = 20,
        };

        for (unsigned t = 0; t < ARRAY_SIZE(timer_irq); t++) {
            qdev_connect_gpio_out(cpudev, t,
                                  qdev_get_gpio_in(gicdev,
                                                   Z63_GIC_PPI(i, timer_irq[t])));
        }
        sysbus_connect_irq(SYS_BUS_DEVICE(gicdev), i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gicdev), i + num_cpu,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gicdev), i + 2 * num_cpu,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gicdev), i + 3 * num_cpu,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
}

/* ------------------------------------------------------------------ */
/* Bootstrap latches                                                  */
/*                                                                    */
/* Two single-register blocks the EL3 bootstrap touches right next to  */
/* the GIC: an implementation-specific interrupt latch at 0x0d100004   */
/* (immediate 0xffffffff) and the system counter enable at 0x0d140000  */
/* (a lone 1, written per core while waking it). Both are plain        */
/* storage; the sibling machine models them the same way.              */
/* ------------------------------------------------------------------ */

#define Z63_INT_LATCH_BASE  0x0D100004ULL
#define Z63_COUNTER_BASE    0x0D140000ULL

static uint64_t z63_latch_read(void *opaque, hwaddr offset, unsigned size)
{
    return *(uint32_t *)opaque;
}

static void z63_int_latch_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    *(uint32_t *)opaque = value;
}

static void z63_counter_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    if (offset || value != 1) {
        error_report("Z63_COUNTER unmodeled write offset=+0x%" HWADDR_PRIx
                     " value=0x%" PRIx64 " -- stopping", offset, value);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, 3);
        return;
    }
    *(uint32_t *)opaque = value;
}

static const MemoryRegionOps z63_int_latch_ops = {
    .read = z63_latch_read,
    .write = z63_int_latch_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps z63_counter_ops = {
    .read = z63_latch_read,
    .write = z63_counter_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* GPIO / pin configuration ("gpio-top", our DTB's PDR block)         */
/*                                                                    */
/* Five banks of pin configuration registers at 0x0d151000, offsets    */
/* 0x00..0x60, 16-bit payload. Writes carry a write mask in the high   */
/* half and the value in the low half, so a read-modify-write only     */
/* touches the pins it names -- the image programs pin muxes here.     */
/* ------------------------------------------------------------------ */

static bool z63_gpio_offset(hwaddr addr, unsigned size)
{
    return size == 4 && !(addr & 3) && addr / 0x100 < Z63_GPIO_BANKS &&
           (addr % 0x100) / 4 < Z63_GPIO_WORDS;
}

/* "B12=a3:2+E3=a3:3+D12=a4:3": this MCU pin and this EXPEED port bit are
 * the same wire. The separator is "+" because "," belongs to -machine;
 * inside an entry ":" separates the port bit's two numbers. Parsed once,
 * and every wire is reported, so a wrong pairing is visible in the log
 * rather than only in a stall - the same rule mcu-spi-peer= follows. */
static void z63_wires_parse(NikonZ63State *s)
{
    g_auto(GStrv) items;

    s->wires_parsed = true;
    s->n_wires = 0;
    if (!s->mcu_wires || !*s->mcu_wires) {
        return;
    }
    items = g_strsplit_set(s->mcu_wires, "+", -1);
    for (char **it = items; *it; it++) {
        Z63McuWire w;
        char mcu[8] = "", wire[8] = "";
        char port;
        unsigned pin;

        if (!**it) {
            continue;
        }
        if (sscanf(*it, "%7[^=]=%7s", mcu, wire) != 2 ||
            sscanf(mcu, "%c%u", &port, &pin) != 2 ||
            (port = g_ascii_toupper(port)) < 'A' || port > 'K' || pin > 15 ||
            sscanf(wire, "a%hhu:%hhu", &w.a53_bank, &w.a53_bit) != 2 ||
            w.a53_bank >= Z63_GPIO_BANKS || w.a53_bit > 15) {
            error_report("mcu-wires: '%s' is not <port><pin>=a<port>:<bit> "
                         "(e.g. B12=a3:2); ignoring it", *it);
            continue;
        }
        if (s->n_wires == Z63_MCU_WIRES_MAX) {
            error_report("mcu-wires: more than %u wires; '%s' ignored",
                         Z63_MCU_WIRES_MAX, *it);
            continue;
        }
        w.mcu_port = port - 'A';
        w.mcu_pin = pin;
        s->wires[s->n_wires++] = w;
        error_report("Z63_MCU_WIRE: P%c%u <-> EXPEED port %u bit %u "
                     "(declared, see mcu-wires=)",
                     port, pin, w.a53_bank, w.a53_bit);
    }
}

/* What the MCU is putting on a wired line: its ODR bit, the reading a pin
 * configured as an output gives. Asked every time the EXPEED reads that
 * port, which needs no callback and cannot miss a change. */
static uint16_t z63_wires_mcu_levels(NikonZ63State *s, unsigned bank,
                                     uint16_t value)
{
    for (unsigned i = 0; s->real_mcu_built && i < s->n_wires; i++) {
        Z63McuWire *w = &s->wires[i];

        if (w->a53_bank == bank &&
            ((s->real_mcu.gpio[w->mcu_port].moder >> (2 * w->mcu_pin)) & 3)
            == 1) {
            /* Only an output of the MCU drives the line. A wire whose MCU
             * end is an input is driven by the EXPEED, and there the
             * EXPEED's own storage is already the answer -- overlaying the
             * MCU's ODR bit would mask what the firmware itself put there. */
            unsigned level = (s->real_mcu.gpio[w->mcu_port].odr >>
                              w->mcu_pin) & 1;

            value = (value & ~(1u << w->a53_bit)) | (level << w->a53_bit);
        }
    }
    if (s->real_mcu_built && bank == 3) {
        /* The shutter button's contacts (nikon-z63-mcukeys.h). */
        value = (value & ~((1u << Z63_RELEASE_HALF_BIT) |
                           (1u << Z63_RELEASE_FULL_BIT))) |
                z63_mcukeys_contacts(&s->real_mcu);
    }
    return value;
}

/* The EXPEED wrote a wired port: hand the bit to the MCU's pin, which is
 * what that firmware would see on the part (through SYSCFG to EXTI, see
 * z63_stm32_gpio_drive). */
static void z63_wires_drive_mcu(NikonZ63State *s, unsigned bank,
                                uint16_t value)
{
    for (unsigned i = 0; s->real_mcu_built && i < s->n_wires; i++) {
        Z63McuWire *w = &s->wires[i];

        if (w->a53_bank == bank) {
            unsigned level = (value >> w->a53_bit) & 1;
            Z63Stm32Gpio *g = &s->real_mcu.gpio[w->mcu_port];

            if (level != w->level &&
                ((g->moder >> (2 * w->mcu_pin)) & 3) != 1 &&
                w->named < Z63_MCU_WIRE_NAMED) {
                w->named++;
                error_report("Z63_MCU_WIRE: EXPEED port %u bit %u -> P%c%u "
                             "= %u (pc=0x%" PRIx64 ")", w->a53_bank,
                             w->a53_bit, 'A' + w->mcu_port, w->mcu_pin,
                             level, z63_current_pc());
            }
            w->level = level;
            z63_stm32_gpio_drive(g, w->mcu_pin, level);
        }
    }
}

/* The top LCD's lines, as MCU pins (port 0 = A): chip select, display-on,
 * the start pulse and the ready line (nikon-z63-toplcd.h). The first three
 * are the pins seen to move from the display module's hooks (sub_8032350
 * and sub_8032360, sub_8031FA8, sub_8032058); the last is the pin
 * sub_8032690 hands to HAL_GPIO_Init. */
#define Z63_TOPLCD_SCS_PORT     0       /* PA4 */
#define Z63_TOPLCD_SCS_PIN      4
#define Z63_TOPLCD_DISP_PORT    4       /* PE8 */
#define Z63_TOPLCD_DISP_PIN     8
#define Z63_TOPLCD_START_PORT   3       /* PD11 */
#define Z63_TOPLCD_START_PIN    11
#define Z63_TOPLCD_READY_PORT   3       /* PD1 */
#define Z63_TOPLCD_READY_PIN    1

/* The MCU changed an output pin: on the board that is an edge into the
 * EXPEED's EXT_INT block (nikon-z63-extint.h). The firmware's own line
 * descriptors put EXT_INT_n on port 3 bit n, which is why a bank-3 wire
 * drives line = bit; port 4 has no line named by anything read so far, and
 * its one wired bit (the ACK line) is polled rather than waited on, so it
 * is left to the readable level. z63_mcu_lines() does the same job for the
 * synthetic peer. */
static void z63_wires_pin_out(void *opaque, unsigned port, unsigned pin,
                              bool level)
{
    NikonZ63State *s = opaque;

    /* The top LCD's inputs (nikon-z63-toplcd.h). */
    if (port == Z63_TOPLCD_SCS_PORT && pin == Z63_TOPLCD_SCS_PIN) {
        z63_toplcd_scs(&s->toplcd, level);
    } else if (port == Z63_TOPLCD_DISP_PORT && pin == Z63_TOPLCD_DISP_PIN) {
        z63_toplcd_disp(&s->toplcd, level);
    } else if (port == Z63_TOPLCD_START_PORT &&
               pin == Z63_TOPLCD_START_PIN) {
        z63_toplcd_start(&s->toplcd, level);
    }
    /* The lens's power paths A (PB0) and B (PA9), nikon-z63-lens.h. */
    if ((port == 1 && pin == 0) || (port == 0 && pin == 9)) {
        z63_lens_power(&s->lens);
    }
    if (!s->wires_parsed) {
        z63_wires_parse(s);
    }
    for (unsigned i = 0; i < s->n_wires; i++) {
        Z63McuWire *w = &s->wires[i];

        if (w->mcu_port != port || w->mcu_pin != pin) {
            continue;
        }
        if (w->named < Z63_MCU_WIRE_NAMED) {
            w->named++;
            error_report("Z63_MCU_WIRE: P%c%u -> EXPEED port %u bit %u = %u",
                         'A' + port, pin, w->a53_bank, w->a53_bit, level);
        }
        if (w->a53_bank == 3) {
            z63_extint_set(&s->extint, w->a53_bit, level);
        }
    }
}

/* The MCU's buttons changed: the shutter button's contacts on the EXPEED
 * side are edges into EXT_INT_4 and 5 (nikon-z63-mcukeys.h). */
static void z63_release_contacts(NikonZ63State *s)
{
    uint16_t closed = z63_mcukeys_contacts(&s->real_mcu);

    z63_extint_set(&s->extint, Z63_RELEASE_HALF_BIT,
                   (closed >> Z63_RELEASE_HALF_BIT) & 1);
    z63_extint_set(&s->extint, Z63_RELEASE_FULL_BIT,
                   (closed >> Z63_RELEASE_FULL_BIT) & 1);
}

/*
 * Inputs the board drives on GPIO_TOP that nothing else models.
 *
 * The USB cable: logical pin 0xc8, descriptor .data 0xa0280ab38 = {0xc8,
 * port 14, bit 10}, read through sub_40C15BFC by the cyclic handler
 * sub_403204D0 every 20 ms, four equal samples to change (0xa02815924 /
 * 0xa02815928); 1 is a cable with VBUS. Its debounced edge starts the
 * task "USB" (sub_40321D7C) and, through the connection manager, the
 * gadget (nikon-z63-dwc3.h). -machine usb-cable=on|off, settable at run
 * time; on by default when usbip= is given.
 */
#define Z63_USB_VBUS_WORD       0x038   /* port 14 */
#define Z63_USB_VBUS_BIT        10

static void zf_ccomp_tick(void *opaque)
{
    NikonZ63State *s = opaque;
    if (!s->zf_ccomp_request || s->zf_ccomp_level) { return; }
    s->zf_ccomp_level = true;
    s->zf_ccomp_responses++;
    z63_extint_set(&s->extint, 7, true);
    error_report("ZF_CCOMP: response GPIO3.7 high, requests=%" PRIu64
                 " responses=%" PRIu64, s->zf_ccomp_requests, s->zf_ccomp_responses);
}

static void zf_ccomp_write(NikonZ63State *s, uint16_t value)
{
    bool request = value & (1u << 12);
    if (request && !s->zf_ccomp_request) {
        s->zf_ccomp_requests++;
        timer_mod(s->zf_ccomp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        error_report("ZF_CCOMP: request GPIO3.12 high, pc=%" PRIx64, z63_current_pc());
    } else if (!request) {
        timer_del(s->zf_ccomp_timer);
        s->zf_ccomp_level = false;
        z63_extint_set(&s->extint, 7, false);
    }
    s->zf_ccomp_request = request;
}

static uint16_t z63_gpio_board_inputs(NikonZ63State *s, hwaddr addr,
                                      uint16_t value)
{
    value = zf_eye_gpio_word(s->zf_eye_ready,zf_eye_output_near(&s->zf_eye_i2c,s->zf_eye_near),addr,value);
    if (s->zf_still && addr == 0x20) {
        int level = z63_z9lab_zf_frame_gpio_level(z63_z9lab);
        if (level >= 0) { value = (value & ~(1u << 5)) | (level << 5); }
    }
    if (s->zf_ccomp_feedback && addr == 0x00c) {
        value = (value & ~(1u << 7)) | ((uint16_t)s->zf_ccomp_level << 7);
    }
    if (addr == Z63_USB_VBUS_WORD) {
        value = (value & ~(1u << Z63_USB_VBUS_BIT)) |
                (s->usb_cable << Z63_USB_VBUS_BIT);
    }
    if (addr == Z63_LENS_RDY_WORD) {
        value = z63_lens_rdy_word(&s->lens, value);     /* pin 41 */
    }
    if (object_dynamic_cast(OBJECT(s), MACHINE_TYPE_NAME("nikon-zf"))) {
        bool inserted = zf_sd_second_present(OBJECT(s));
        bool force_low = s->zf_gpio22_bit8_low;
        value = zf_gpio22_bit8_word(true,
            s->zf_gpio22_bit8_high || (!force_low && !inserted),
            force_low || inserted, addr, value);
    }
    return value;
}

static uint64_t z63_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;

    if (!z63_gpio_offset(addr, size)) {
        z63_stop_read("GPIO_TOP", Z63_GPIO_TOP_BASE + addr, size);
        return 0;
    }
    /*
     * With gpio-input-level set, every bank's input register answers that
     * word instead of what the guest last wrote. This is a MEASUREMENT
     * KNOB, not a model: the real values are per pin and are not known
     * here. It exists because every pin currently reads 0 -- sub_40C15BFC
     * reads a bank base out of the runtime array at 0xa02dc41b8 and returns
     * (*base >> pin[+8]) & 1 -- and camera switches are usually active low,
     * so all-zero may be telling the application that every switch is
     * closed. Flipping the whole word is the cheapest way to find out
     * whether GPIO is the gate on the display coming up. Default 0, which
     * is exactly the previous behaviour.
     */
    if (s->gpio_input_level_set && (addr % 0x100) == 0x010) {
        return s->mcu_peer ? z63_mcu_gpio(&s->mcu, 4, s->gpio_input_level)
                           : s->gpio_input_level;
    }
    /* The pins the MCU drives (nikon-z63-mcu.h): port p's data word is at
     * 4 * p in the first bank. With the real MCU the same wires come from
     * mcu-wires=, which overlays only the bits that are wired. */
    if (s->mcu_peer && (addr == 0x00c || addr == 0x010)) {
        return z63_mcu_gpio(&s->mcu, addr / 4,
                            s->gpio_config[0][addr / 4]);
    }
    if (!s->wires_parsed) {
        z63_wires_parse(s);
    }
    if (s->real_mcu_built && addr < 0x100 && !(addr % 4)) {
        return z63_gpio_board_inputs(s, addr,
            z63_wires_mcu_levels(s, addr / 4, s->gpio_config[0][addr / 4]));
    }

    /*
     * Diagnostics for the one lead this block has given. Every register in
     * it reads back what was written, so a register the firmware reads far
     * more often than it writes is a register it expects the outside world
     * to change -- a switch, a door, a battery contact. In a 40-second idle
     * run +0x10 of bank 0 was read 161 times and always answered 0, which is
     * the shape the memory note calls "a guest polling a model that cannot
     * change". This names the reader so the next step is a code address
     * rather than a guess. It is removed once that is known.
     */
    if (addr == 0x010 && ++s->gpio_poll_count[addr / 4] == 20) {
        error_report("Z63_GPIO_TOP: +0x010 read 20 times, last from "
                     "pc=0x%" PRIx64 ", always answering 0x%x -- an input "
                     "this model never changes", z63_current_pc(),
                     s->gpio_config[0][4]);
    }
    return z63_gpio_board_inputs(s, addr,
                                 s->gpio_config[addr / 0x100][(addr % 0x100) / 4]);
}

/*
 * The EXPEED's power-hold line: port 3 bit 11. The pin setup drives it
 * high at boot -- sub_41187B00 writes 0x0d15100c = 0xffff0800 from its
 * table (0x44b17708), as the loader's sub_40003794 does -- and each of the
 * image's three ways to switch the body off drives it low and nothing
 * else does: the standby power-down sub_4019502C as its last store before
 * WFI (0x401952e8, 0x0d15100c = 0x08000000: mask bit 11, value 0), the
 * loader's power-off sub_4009360C (0x40093628, after waiting on port 9's
 * input) and the emergency power-off in sub_40C07EB8 (0x40c08020, through
 * sub_40C15BB8 after ten bad readings of an input), each followed by a
 * spin or WFI. The body MCU watches it (its power manager cuts the
 * EXPEED's rails when the input it polls after the power-off frame reads
 * low, sub_803C6A8 -> sub_803C7F4, keeping one rail, DRAM's, for a
 * standby), so it is the EXPEED's "keep my power on".
 *
 * What the MCU does with the line is not modelled (it is not in the
 * default mcu-wires=), nor is the wake: the MCU powering the EXPEED back
 * up on a key, which the EXPEED's start-up takes as a resume when port 3
 * bit 6 reads high (the DRAM init's mode 3; sub_40D6F7F0 at 0x40d6f844).
 * The power-off property decides what the machine does: shutdown (the
 * default) asks QEMU for a guest shutdown, as a camera that switched
 * itself off -- -action shutdown=pause keeps the VM for inspection;
 * continue only logs, leaving the EXPEED in its WFI and the MCU running.
 */
#define Z63_POWER_HOLD_WORD     0x00c   /* port 3 */
#define Z63_POWER_HOLD_BIT      (1u << 11)

static void z63_power_hold_dropped(NikonZ63State *s)
{
    bool shutdown = !s->power_off || !strcmp(s->power_off, "shutdown");

    error_report("Z63_POWER: the EXPEED dropped its power-hold line (GPIO "
                 "port 3 bit 11, pc=0x%" PRIx64 ") -- %s", z63_current_pc(),
                 shutdown ? "the camera switched itself off; shutting down "
                            "(power-off=shutdown)"
                          : "power-off=continue, the EXPEED idles");
    if (shutdown) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

/* The I2C banks' devices: the touch panel at bank 0 address 0x55
 * (nikon-z63-touch.h), everything else the posture sensor's endpoint,
 * which answers its own address and NACKs and names the rest. */
static void zf_eye_refresh(NikonZ63State *s)
{
    if(s->zf_eye_ready) {
        z63_extint_set(&s->extint,ZF_EYE_EXTINT,!zf_eye_output_near(&s->zf_eye_i2c,s->zf_eye_near));
    }
}

static bool z63_bus_start(void *opaque, unsigned bank, uint16_t address,
                          bool receive, bool repeated)
{
    NikonZ63State *s = opaque;

    /* zf_eye_ready is the existing board-class discriminator, not guest state. */
    s->i2c_to_display3e[bank] = s->zf_eye_ready && bank == 0 && address == 0x3e;
    if (s->i2c_to_display3e[bank]) return zf_display3e_start(&s->zf_display3e, bank, address, receive);
    s->i2c_to_temperature[bank] = s->zf_eye_ready && bank==3 && address==0x48;
    if(s->i2c_to_temperature[bank]) { return zf_temperature_start(&s->zf_temperature,bank,address,receive); }
    s->i2c_to_eye[bank] = s->zf_eye_ready && bank==3 && address==0x38;
    if(s->i2c_to_eye[bank]) {
        return zf_eye_i2c_start(&s->zf_eye_i2c,bank,address,receive);
    }
    s->i2c_to_touch[bank] = bank == Z63_TOUCH_BANK &&
                            address == Z63_TOUCH_ADDRESS;
    if (s->i2c_to_touch[bank]) {
        return z63_touch_start(&s->touch, receive);
    }
    return z63_imu_start(&s->imu, bank, address, receive, repeated);
}

static bool z63_bus_write(void *opaque, unsigned bank, uint8_t byte)
{
    NikonZ63State *s = opaque;

    if (s->i2c_to_display3e[bank]) return zf_display3e_write(&s->zf_display3e, byte);
    if(s->i2c_to_temperature[bank]) { return zf_temperature_write(&s->zf_temperature,byte); }
    if(s->i2c_to_eye[bank]) {
        bool ack=zf_eye_i2c_write(&s->zf_eye_i2c,byte);
        if(ack) { zf_eye_refresh(s); }
        return ack;
    }
    return s->i2c_to_touch[bank] ? z63_touch_write(&s->touch, byte)
                                 : z63_imu_write(&s->imu, bank, byte);
}

static bool z63_bus_read(void *opaque, unsigned bank, uint8_t *byte)
{
    NikonZ63State *s = opaque;

    if (s->i2c_to_display3e[bank]) return zf_display3e_read(&s->zf_display3e, byte);
    if(s->i2c_to_temperature[bank]) { return zf_temperature_read(&s->zf_temperature,byte); }
    if(s->i2c_to_eye[bank]) { return false; } /* unbound reads are never zero-filled */
    return s->i2c_to_touch[bank] ? z63_touch_read(&s->touch, byte)
                                 : z63_imu_read(&s->imu, bank, byte);
}

static void z63_bus_stop(void *opaque, unsigned bank)
{
    NikonZ63State *s = opaque;

    if (s->i2c_to_display3e[bank]) {
        zf_display3e_stop(&s->zf_display3e); s->i2c_to_display3e[bank] = false; return;
    }
    if(s->i2c_to_temperature[bank]) {
        zf_temperature_stop(&s->zf_temperature);s->i2c_to_temperature[bank]=false;return;
    }
    if(s->i2c_to_eye[bank]) {
        zf_eye_i2c_stop(&s->zf_eye_i2c);s->i2c_to_eye[bank]=false;return;
    }
    if (s->i2c_to_touch[bank]) {
        s->touch.selected = false;
    } else {
        z63_imu_stop(&s->imu, bank);
    }
}

static const Z63I2cEndpoint z63_bus_endpoint = {
    .start = z63_bus_start,
    .write = z63_bus_write,
    .read = z63_bus_read,
    .stop = z63_bus_stop,
};

static void z63_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    NikonZ63State *s = opaque;
    uint16_t *config;
    uint16_t mask;

    if (!z63_gpio_offset(addr, size)) {
        z63_stop_write("GPIO_TOP", Z63_GPIO_TOP_BASE + addr, size, value);
        return;
    }
    config = &s->gpio_config[addr / 0x100][(addr % 0x100) / 4];
    mask = value >> 16;
    uint16_t previous = *config;
    *config = (*config & ~mask) | ((uint16_t)value & mask);
    if (s->zf_eye_ready && addr == 0x24 &&
        ((previous ^ *config) & (1u << 14))) {
        error_report("ZF_ACCESS_LED: %s GPIO139 group9/bit14 pc=%" PRIx64,
                     (*config & (1u << 14)) ? "on" : "off", z63_current_pc());
    }
    if (s->zf_still && addr == 0x20) {
        z63_z9lab_zf_capture_gpio(z63_z9lab, previous, *config);
    }
    if (s->zf_ccomp_feedback && addr == 0x00c && (mask & (1u << 12))) {
        zf_ccomp_write(s, *config);
    }
    if (s->mcu_peer && addr < 0x100 && !(addr % 4)) {
        z63_mcu_gpio_out(&s->mcu, addr / 4, *config);
    }
    if (!s->wires_parsed) {
        z63_wires_parse(s);
    }
    if (addr < 0x100 && !(addr % 4)) {
        z63_wires_drive_mcu(s, addr / 4, *config);
    }
    if (addr == Z63_AUDIO_CS_PORT * 4) {
        z63_audio_cs(&s->audio, *config);
    }
    if (addr == Z63_LENS_REQ_WORD && (mask & (1u << Z63_LENS_REQ_BIT))) {
        z63_lens_request(&s->lens, (*config >> Z63_LENS_REQ_BIT) & 1);
    }
    if (addr == Z63_POWER_HOLD_WORD && (mask & Z63_POWER_HOLD_BIT) &&
        !(value & Z63_POWER_HOLD_BIT)) {
        z63_power_hold_dropped(s);
    }
    if (addr == Z63_TOUCH_POWER_PORT * 4 &&
        (mask & (1u << Z63_TOUCH_POWER_BIT))) {
        z63_touch_power(&s->touch, (*config >> Z63_TOUCH_POWER_BIT) & 1);
    }
}

static const MemoryRegionOps z63_gpio_ops = {
    .read = z63_gpio_read,
    .write = z63_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* TOP block at 0x0d150000                                             */
/*                                                                    */
/* Offset 0 is the SoC identity word -- bits 0..4 a chip id, bits      */
/* 28..31 the silicon revision. This body's own getters read it the    */
/* same way the Z8 sibling project documents: 0x40c2eb44 returns       */
/* {word & 0x1f, word >> 28}, and 0x40c2eb88 picks offset 0x0c when    */
/* the revision is below 3 and 0x10 otherwise, then returns             */
/* {(w >> 16) & 0x1f, w & 0x1f}. So 0x0c and 0x10 are the identity     */
/* selector pair, and all three reads below are read-only.             */
/*                                                                    */
/* The revision nibble is NOT measured. The BSP variant getter at      */
/* 0x41188eac stores ((id >> 28) - 1) to 0x0a04ceedec, and no dump we  */
/* hold reaches that address, so the value is a declared virtual board  */
/* choice exposed as the soc-revision machine property rather than a    */
/* silently invented constant.                                         */
/*                                                                    */
/* Everything else the firmware touches in this page is a plain 32-bit  */
/* configuration word: the TOP init loop at 0x41187b00 does and/or/orr */
/* read-modify-writes on the 0x2xx words, which only round-trip if the  */
/* register stores what was written, and 0x40c2ebc8 builds 0x180..0x188 */
/* from a byte array. No completion, lock or clock status is invented.  */
/* ------------------------------------------------------------------ */

static bool z63_top_is_config(hwaddr addr)
{
    switch (addr) {
    case 0x004: /* mode selector written with 8 by the TOP init loop */
    case 0x008: /* 3-bit selector written by 0x40c2eb6c */
    case 0x180: case 0x184: case 0x188: /* pin/display clock setup */
    case 0x208: case 0x20c: case 0x210: case 0x214:
    case 0x21c: case 0x220: case 0x224:
    case 0x234: case 0x23c: case 0x240: case 0x248:
    case 0x258: case 0x260: /* Zf TOP masked configuration RMW */
        return true;
    default:
        return false;
    }
}

static uint64_t z63_top_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) ||
        (addr >= 0x250 && !object_dynamic_cast(OBJECT(s), MACHINE_TYPE_NAME("nikon-zf")))) {
        z63_stop_read("TOP", Z63_TOP_BASE + addr, size);
        return 0;
    }
    switch (addr) {
    case 0x000: /* identity: chip id | revision nibble */
        return (s->soc_chip_id & 0x1f) | ((uint32_t)s->soc_revision << 28);
    case 0x00c: /* identity selectors; virtual board default 0 */
    case 0x010:
        return 0;
    default:
        if (z63_top_is_config(addr)) {
            return s->top_words[addr / 4];
        }
        z63_stop_read("TOP", Z63_TOP_BASE + addr, size);
        return 0;
    }
}

static void z63_top_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || !z63_top_is_config(addr) ||
        (addr >= 0x250 && !object_dynamic_cast(OBJECT(s), MACHINE_TYPE_NAME("nikon-zf")))) {
        z63_stop_write("TOP", Z63_TOP_BASE + addr, size, value);
        return;
    }
    s->top_words[addr / 4] = value;
}

static const MemoryRegionOps z63_top_ops = {
    .read = z63_top_read,
    .write = z63_top_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Clock / power configuration bank at 0x0d152000                      */
/*                                                                    */
/* This firmware programs it as a table of constant 32-bit words:      */
/* 0x41186ec0 writes 0x0d152090/094 and then 0x0d1520a0..0x0d1520dc    */
/* straight from immediate values, and 0x401950d0 writes the same bank  */
/* again from base 0x0d1520a0. Neither reads anything back, and the     */
/* Z 6III Linux DTB calls this page CGN (clock generator).             */
/*                                                                    */
/* The Z8 sibling project models the 0xa0..0xe0 stretch as 2-bit gate   */
/* command fields taken from the official clk-cgn.c. That protocol is   */
/* deliberately NOT applied here: this body's writes are whole-word     */
/* constants, not the pair-encoded commands clk-cgn.c describes, and    */
/* nothing reads them back, so the minimal model that satisfies the     */
/* observed accesses is plain storage. The whitelist is the union of    */
/* the offsets this firmware is seen to write and the ones the sibling  */
/* project documents for the same bank; everything else still stops.    */
/* ------------------------------------------------------------------ */

static bool z63_cgn_is_config(hwaddr addr)
{
    if (addr >= 0x0a0 && addr <= 0x0e0) {
        return true;
    }
    switch (addr) {
    case 0x000: case 0x008: case 0x010: case 0x014: case 0x018:
    case 0x040: case 0x044: case 0x048: case 0x04c:
    case 0x060: case 0x064: case 0x068: case 0x06c:
    case 0x070: case 0x074: case 0x078:
    case 0x080: /* 41187034 reads it into wzr as a flush */
    case 0x090: case 0x094:
    case 0x0d8: /* 41842158, 4184221c */
        return true;
    default:
        return false;
    }
}

static uint64_t z63_cgn_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || !z63_cgn_is_config(addr)) {
        z63_stop_read("CGN", Z63_CGN_BASE + addr, size);
        return 0;
    }
    return s->cgn_words[addr / 4];
}

static void z63_cgn_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || !z63_cgn_is_config(addr)) {
        z63_stop_write("CGN", Z63_CGN_BASE + addr, size, value);
        return;
    }
    s->cgn_words[addr / 4] = value;
}

static const MemoryRegionOps z63_cgn_ops = {
    .read = z63_cgn_read,
    .write = z63_cgn_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Peripheral control words at 0x0d150300                              */
/*                                                                    */
/* 0x41186f58 programs this block from immediates right after the CGN   */
/* bank, and the values it uses line up with the per-word write masks   */
/* the Z8 sibling project recovered from its own ROM table (their       */
/* ROM4164c568/60a/64e). Every word this body writes fits that table    */
/* exactly:                                                            */
/*                                                                    */
/*   +00 0x00280001  +04 0x000007ff  +08 0x000f3fff  +0c 0x000013ff    */
/*   +10 0x6c3007fb  +20 0xff07fbf3  +24 0x00000000                    */
/*                                                                    */
/* -- each is a subset of its mask. That agreement across two separate  */
/* firmwares is why the table is reused here rather than re-derived.    */
/* +18 and +1c have a zero mask, so they are not populated words and    */
/* still stop. Writes that set bits outside their mask also stop: that  */
/* is the sibling's rule and nothing here justifies relaxing it.        */
/*                                                                    */
/* The registers store what was written -- the firmware performs its    */
/* own locked read-modify-write on them. No device or transfer          */
/* completion is invented.                                             */
/* ------------------------------------------------------------------ */

static const uint32_t z63_pctl_masks[Z63_PCTL_WORDS] = {
    0xffaffbffU, /* +00 */
    0x00000fffU, /* +04 */
    0x00ff3fffU, /* +08 */
    0x000f3fffU, /* +0c */
    0xffff3fffU, /* +10 */
    0x0000000fU, /* +14 */
    0x00000000U, /* +18 -- not a populated word */
    0x00000000U, /* +1c -- not a populated word */
    0xffbffbffU, /* +20 */
    0x00000fffU, /* +24 */
};

static bool z63_pctl_word(hwaddr addr, unsigned size, uint64_t value,
                          bool write)
{
    if (size != 4 || (addr & 3) || addr / 4 >= Z63_PCTL_WORDS) {
        return false;
    }
    if (!z63_pctl_masks[addr / 4]) {
        return false;
    }
    return !write || !(value & ~(uint64_t)z63_pctl_masks[addr / 4]);
}

static uint64_t z63_pctl_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;

    if (!z63_pctl_word(addr, size, 0, false)) {
        z63_stop_read("PCTL", Z63_PCTL_BASE + addr, size);
        return 0;
    }
    return s->pctl_words[addr / 4];
}

static void z63_pctl_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    NikonZ63State *s = opaque;

    if (!z63_pctl_word(addr, size, value, true)) {
        z63_stop_write("PCTL", Z63_PCTL_BASE + addr, size, value);
        return;
    }
    s->pctl_words[addr / 4] = value;
}

static const MemoryRegionOps z63_pctl_ops = {
    .read = z63_pctl_read,
    .write = z63_pctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Configuration sequencer at 0x0d158000                               */
/*                                                                    */
/* 0x41187018 runs exactly the handshake the Z8 sibling project         */
/* documents, with the same two magic constants, the same poll bit and  */
/* the same ack value:                                                 */
/*                                                                    */
/*     write 0x0d158040 = 0xcdce0c34   (configuration)                 */
/*     write 0x0d158000 = 0x0000aa02   (command)                       */
/*     poll  0x0d15800c until bit 5                                     */
/*     write 0x0d158010 = 0x20         (acknowledge)                    */
/*                                                                    */
/* The done bit is NOT hardwired high. This is an explicit              */
/* configuration -> command -> complete -> acknowledge state machine,    */
/* so the poll only succeeds because the command actually arrived; a     */
/* machine that simply returned 0x20 would be asserting completion that  */
/* nothing performed. The acknowledge clears it again. Nothing further   */
/* is inferred about what the sequencer configures -- no PLL lock, no    */
/* rate, no clock status.                                               */
/*                                                                    */
/* 0x0d158080..0x09c receives a descending byte run 0x1f..0x00 packed   */
/* into words and is stored verbatim; its meaning is unknown.            */
/* ------------------------------------------------------------------ */

#define Z63_SEQ_DONE   0x20
#define Z63_SEQ_CONFIG 0xCDCE0C34
#define Z63_SEQ_CMD    0x0000AA02

/* Accepted offsets, each one taken from an instruction that builds it:
 *
 *   000  40c2f450, 41186f98   command register (handshake below)
 *   008  40c2f1dc             `str wzr, [0d158008]` after copying 00c into 010
 *   00c  41186f98             status; this is the word the handshake polls
 *   010  40c2f1dc             ack
 *   018  40c2f3b8, 41187098   read for its bits and read into wzr as a flush
 *   020  40c2f494             `str w9, [x21, #0x20]` with x21 = 0d158000,
 *                             the first of four consecutive stores into the
 *                             block -- 020, 040, 010, 008 -- followed by the
 *                             command word at 000. The first value the boot
 *                             puts here is 0x08000000 (bit 27).
 *   040  41186f98             config written before the command; also the
 *                             second of those four stores (w10)
 *   050  40c2f254             byte-order word, written from a per-unit pair
 *   080..09c  41186f98, 40c2f254  eight words; 40c2f254 fills them with
 *                             00010203, 04050607 ... 1c1d1e1f, i.e. the
 *                             identity byte order of a 32-byte lane table
 *
 * Anything else in the block, and every width other than 4, still stops. */
static bool z63_seq_is_word(hwaddr addr)
{
    switch (addr) {
    case 0x000: case 0x008: case 0x00c: case 0x010: case 0x018:
    case 0x020: case 0x040: case 0x050:
        return true;
    default:
        return addr >= 0x080 && addr <= 0x09c;
    }
}

static uint64_t z63_seq_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;
    uint32_t value;

    if (size != 4 || (addr & 3) || !z63_seq_is_word(addr)) {
        z63_stop_read("SEQ", Z63_SEQ_BASE + addr, size);
        return 0;
    }
    value = s->seq_words[addr / 4];
    if (addr == 0x00c && s->seq_done) {
        value |= Z63_SEQ_DONE;
    }
    return value;
}

static void z63_seq_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || !z63_seq_is_word(addr)) {
        z63_stop_write("SEQ", Z63_SEQ_BASE + addr, size, value);
        return;
    }
    s->seq_words[addr / 4] = value;

    /* A start command is 0xAA00 in the upper half with two request bits
     * below it. The narrow 0xAA02-after-0xCDCE0C34 pairing the boot code was
     * reversed from is kept, but the display path writes
     * 0xAA00 | (2 * b) | a from 0x40C2F300 with its own computed +0x040
     * value, so the general form has to be accepted or that command is
     * silently ignored.
     *
     * Completion is signalled two ways because the firmware uses both. The
     * status bit at +0x00c is what the boot code polls. The interrupt is what
     * the display path needs: 0x40C2F300 programs +0x020, +0x040, +0x010 and
     * +0x008, writes the start command, and then -- because 0x40C2F4E8 passes
     * a null callback -- blocks in sub_40D32638 on an event flag only the
     * sequencer's handler sets. Without the line the module entry round stops
     * there, in the sixth of its twenty-one modules, and nothing downstream
     * ever starts.
     *
     * SPI 192, PMU, is not a guess: the firmware's own PLL resource and IRQ
     * constructor at 0x40C2F120 installs its handler with `MOV W0, #0xE0`
     * into sub_40C00704, which takes a GIC INTID, and INTID 224 is SPI 192.
     *
     * DECLARED CHOICE: completion is immediate and always successful, and the
     * line is held as a level until the guest acknowledges at +0x010 with the
     * done bit -- the same write the status bit already keys on. How long the
     * real sequencer takes, and whether it can refuse a command, are
     * unmeasured. */
    if (addr == 0x000 &&
        ((value & 0xff00) == 0xaa00 ||
         (value == Z63_SEQ_CMD &&
          s->seq_words[0x040 / 4] == Z63_SEQ_CONFIG))) {
        s->seq_done = true;
        if (s->seq_irq) {
            z63_set_irq(s->seq_irq, 1, "sequencer");
        }
    } else if (addr == 0x010 && (value & Z63_SEQ_DONE)) {
        s->seq_done = false;
        if (s->seq_irq) {
            z63_set_irq(s->seq_irq, 0, "sequencer");
        }
    }
}

static const MemoryRegionOps z63_seq_ops = {
    .read = z63_seq_read,
    .write = z63_seq_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};


/* ------------------------------------------------------------------ */
/* SNOR_USER: the serial NOR beyond the firmware image                  */
/*                                                                    */
/* The SoC's own region table calls 0x45980000-0x48000000 SNOR_USER,    */
/* and the image carries its partition map at rodata 0x4531fd24 --      */
/* 0x28 per entry, starting with the loader at 0x40000000 and the      */
/* SOLID image at 0x40160000, then eighteen-plus partitions inside     */
/* SNOR_USER. Those partitions hold factory-written, per-unit bodies:  */
/* parameter tables, calibration, sequence resources. They are in no   */
/* firmware package, so this board has nothing to put there.           */
/*                                                                    */
/* It used to fall inside the 128 MiB image window and read back as    */
/* zeros, which is why nothing stopped and also why nothing worked:    */
/* the readers validate a body before using it and zeros fail. The     */
/* ROMRAM subsystem (event flag 71, named in the image at 0x44b13d6c)  */
/* therefore never reaches its stage-6 bit, and everything downstream  */
/* of it -- the display subscriber, the menus -- never starts. The     */
/* whole chain is in docs/serial-nor-resources.md.                     */
/*                                                                    */
/* Giving it its own region changes no value: it still reads zero.     */
/* What it adds is that the board can now say *who* reads it and       */
/* *where*, which is the evidence needed before putting any synthetic  */
/* body here. Each distinct partition is reported once, with the       */
/* offset and the PC.                                                 */
/* ------------------------------------------------------------------ */

#define Z63_SNOR_USER_BASE   0x45980000ULL
#define Z63_SNOR_USER_SIZE   0x02680000
#define Z63_SNOR_REPORTS     24

static uint64_t z63_snor_user_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;
    hwaddr partition = addr & ~0x1ffffULL;   /* 128 KiB, the common size */
    uint64_t v = 0;
    unsigned i;

    if (s->snor_data && addr + size <= Z63_SNOR_USER_SIZE) {
        memcpy(&v, s->snor_data + addr, size);
    }

    for (i = 0; i < s->snor_reported; i++) {
        if (s->snor_report[i] == partition) {
            return v;
        }
    }
    if (s->snor_reported < Z63_SNOR_REPORTS) {
        s->snor_report[s->snor_reported++] = partition;
        error_report("Z63_SNOR_USER: read of 0x%09" HWADDR_PRIx
                     " (partition +0x%06" HWADDR_PRIx ") from pc=0x%09" PRIx64
                     " size %u -> 0x%" PRIx64 "%s",
                     Z63_SNOR_USER_BASE + addr, partition, z63_current_pc(),
                     size, v,
                     s->snor_data ? "" : " -- this region is empty on this "
                     "board; see docs/serial-nor-resources.md");
    }
    return v;
}

static void z63_snor_user_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    NikonZ63State *s = opaque;

    /* Only the data of a page program (nikon-z63-nor.h) goes this way. */
    if (z63_nor_window_write(&s->nor,
                             Z63_SNOR_USER_BASE - Z63_NOR_FLASH_VA + addr,
                             value, size)) {
        return;
    }
    z63_stop_write("SNOR_USER (written outside a page program)",
                   Z63_SNOR_USER_BASE + addr, size, value);
}

static const MemoryRegionOps z63_snor_user_ops = {
    .read = z63_snor_user_read,
    .write = z63_snor_user_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Identification block at 0x0d153000                                  */
/*                                                                    */
/* 40d6f9b0 reads +0x00 and compares it with 0x53; only if it matches   */
/* does 40d6f9bc read +0x04 and compare it with 0x00030001; only if     */
/* that also matches does 40d6f9d0 read +0x40 and throw the result      */
/* away. This is the same conditional diagnostic the Z8 notes describe  */
/* at their 40d69678, down to both constants, so the sibling project    */
/* reached the same conclusion: it is a diagnostic, and the constants   */
/* are what the code compares against, not a measured reset value.      */
/*                                                                    */
/* The model returns 0, i.e. "does not match". That is a declaration,   */
/* not a measurement: no dump in this project covers this block. The    */
/* consequence is bounded and visible -- the firmware skips one read    */
/* whose only effect is a bus flush -- and choosing 0x53 would be       */
/* fabricating a value to reach a branch nothing in the code depends    */
/* on. The block is read-only; a write stops.                           */
/* ------------------------------------------------------------------ */

static uint64_t z63_id_read(void *opaque, hwaddr addr, unsigned size)
{
    if (size != 4 || (addr & 3) ||
        (addr != 0x000 && addr != 0x004 && addr != 0x040)) {
        z63_stop_read("ID", Z63_ID_BASE + addr, size);
        return 0;
    }
    return 0;
}

static void z63_id_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    z63_stop_write("ID", Z63_ID_BASE + addr, size, value);
}

static const MemoryRegionOps z63_id_ops = {
    .read = z63_id_read,
    .write = z63_id_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Configuration bank at 0x0d157000                                    */
/*                                                                    */
/* 40c2f024 works on +0x010: it reads the word, ORs in bit 0 and        */
/* writes it back, then does the same to +0x004, and folds two bits of  */
/* its argument into +0x008. 40c2f088 and 40c2f0ac do the same around   */
/* +0x790 and +0x794. Those read-modify-write sequences only behave if  */
/* the read returns what was last written, so this is plain storage     */
/* with no seed value: a word that was never written reads back 0, and  */
/* the model does not claim that 0 is what silicon reports.             */
/* ------------------------------------------------------------------ */

static bool z63_cfg7_is_word(hwaddr addr)
{
    switch (addr) {
    case 0x010: case 0x014: case 0x018:
    case 0x790: case 0x794:
        return true;
    default:
        return false;
    }
}

static uint64_t z63_cfg7_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || !z63_cfg7_is_word(addr)) {
        z63_stop_read("CFG7", Z63_CFG7_BASE + addr, size);
        return 0;
    }
    return s->cfg7_words[addr / 4];
}

static void z63_cfg7_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || !z63_cfg7_is_word(addr)) {
        z63_stop_write("CFG7", Z63_CFG7_BASE + addr, size, value);
        return;
    }
    s->cfg7_words[addr / 4] = value;
}

static const MemoryRegionOps z63_cfg7_ops = {
    .read = z63_cfg7_read,
    .write = z63_cfg7_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};




/* ------------------------------------------------------------------ */
/* Three flat register windows in 0x0e000000..0x0e210000                */
/*                                                                    */
/* These are reached by the driver-init chain 40c13f98 calls:           */
/*   40c140cc, 40c143f8, 40c157d4, 40c164e8, 40c16c24, 40c173b8,        */
/*   40c18ce8, 40c19a7c, 40c1b454                                       */
/* The two already handled from that chain are the auxiliary pair at    */
/* 0x0c86e000 (40c157d4) and the SPI block at 0x0e700000 (40c14408).    */
/*                                                                    */
/* A constant sweep of the image bounds where these banks are but NOT   */
/* which offsets live in them, and the run proved that: the first       */
/* version of this model accepted only movz/movk constants and stopped  */
/* at 0x0e005028, which 40c16ca8 forms as `add x23, x21, #0x28` on a    */
/* base that walks 0x400 at a time. An offset reached as base+offset    */
/* never appears as a constant, so no whitelist can be complete, and    */
/* the same reason is why the video array at 0x14000000 has none.       */
/*                                                                    */
/* So each bank is a plain register file over the span its own bases     */
/* bound, and no individual register is claimed to mean anything:       */
/*                                                                    */
/*   E0  0x0e000000..0x0e005fff   6 bases at +n*0x1000, from 40c19638,   */
/*                                0x40c14100, 40c19328, 40c1af98;       */
/*                                40c16c24 walks a further 4 sub-blocks  */
/*                                of 0x400 inside the last one.          */
/*   E1  0x0e100000..0x0e12ffff   3 bases at +n*0x10000, built by        */
/*                                40c16534/40c16538/40c16554, which set  */
/*                                bit 28 then bit 31 of each.            */
/*   E2  0x0e200000..0x0e2005ff   6 bases at +n*0x100, built by          */
/*                                40c173bc/40c17574/40c175f8.           */
/*                                                                    */
/* What the banks ARE is not established. They are not in the Linux      */
/* device tree and neither local analysis nor the Z8 notes name them.    */
/* Reads return the last write, unwritten words read 0, and every        */
/* address outside the three spans still stops with its PC.              */
/* ------------------------------------------------------------------ */

static uint64_t z63_win_read(void *opaque, hwaddr addr, unsigned size)
{
    Z63Win *w = opaque;
    uint64_t value;

    /* The multi-IO timer block inside E0 is ../z9-qemu's model when
     * z9-capture=on (nikon-z63-z9lab.h); everything else here stays a
     * plain register file. */
    if (z63_front_read(w->base + addr, size, &value)) {
        return value;
    }
    if (size != 4 || (addr & 3) || addr >= w->size) {
        z63_stop_read(w->name, w->base + addr, size);
        return 0;
    }
    return w->words[addr / 4];
}

static void z63_win_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    Z63Win *w = opaque;

    if (z63_front_write(w->base + addr, value, size)) {
        return;
    }
    if (size != 4 || (addr & 3) || addr >= w->size) {
        z63_stop_write(w->name, w->base + addr, size, value);
        return;
    }
    w->words[addr / 4] = value;
}

static const MemoryRegionOps z63_win_ops = {
    .read = z63_win_read,
    .write = z63_win_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
/* ------------------------------------------------------------------ */
/* Video pipeline register array at 0x14000000                         */
/*                                                                    */
/* sub_41187344 (a driver entry reached only through a registration     */
/* table -- it has no direct caller) sweeps this block writing 0x880000 */
/* to channel registers, then small mode words. Instances are 0x4000    */
/* apart and all of its addresses land on +0x3080, +0x3f60 or +0x3f68.  */
/*                                                                    */
/* The offsets are NOT enumerable from code, which is why there is no   */
/* whitelist here. sub_41187994 picks its target out of a runtime table */
/* (`mul x14, x8, x10; ldr w14, [x9, x14]; add w15, w14, #0x10`), and  */
/* the observed access at 0x14154050 is instance 85 offset 0x50 -- not  */
/* any offset any immediate in the image mentions. The instance count   */
/* is likewise table-driven, so the bound comes from the firmware's own */
/* 40-entry channel table at ROM 0x44b15614: every entry is            */
/* `0x14000000 + n*0x4000 + 0x3080` and the highest n is 109.          */
/*                                                                    */
/* The neighbouring interrupt-source table calls this stretch of the    */
/* SoC the video pipeline (SIF_0..7, PPC_0..4, PERSUIT_0..31, MIPU, ...);*/
/* the block is not in the Linux device tree, i.e. Linux never touches  */
/* it. Which instance is which channel is not established, and no       */
/* individual register is claimed to mean anything: this is a plain     */
/* register file over one 0x4000-byte window per instance, and every    */
/* offset outside those windows still stops.                            */
/*                                                                    */
/* The 0x3f60/0x3f68 registers are written eight bytes wide; the impl   */
/* access size is 4 so QEMU splits those into the two halves.           */
/* ------------------------------------------------------------------ */

static uint64_t z63_vpipe_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || addr >= Z63_VPIPE_SIZE) {
        z63_stop_read("VPIPE", Z63_VPIPE_BASE + addr, size);
        return 0;
    }
    return s->vpipe[addr / 4];
}

static void z63_vpipe_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || addr >= Z63_VPIPE_SIZE) {
        z63_stop_write("VPIPE", Z63_VPIPE_BASE + addr, size, value);
        return;
    }
    s->vpipe[addr / 4] = value;
}

static const MemoryRegionOps z63_vpipe_ops = {
    .read = z63_vpipe_read,
    .write = z63_vpipe_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Five sibling blocks at 0x11800000 + n * 0x800000                     */
/*                                                                    */
/* This family was found by reading the immediates of the video         */
/* driver rather than by a trap, and it is the reason a single          */
/* 0x1180002c gate guards two separate functions:                       */
/*                                                                    */
/*   401951b8  polls +0x8c8 on 0x11800000, 0x12000000, 0x12800000,      */
/*             0x13000000 and 0x13800000 in turn -- instantiating the   */
/*             same register offset on five consecutive bases           */
/*   40c312dc  builds a base 0x1180f018 and then                     */
/*   40c312e0  `add w0, w8, w19, lsl #23` -- +n*0x800000, the stride    */
/*   40c30258  loads 0x11800000 alongside the constant 0x800004         */
/*   411870c8  `cmp w0, #4` selects the instance for the upload path    */
/*                                                                    */
/* The same 64 KB window covers every 0x138xxxxx address the firmware   */
/* is known to touch (0x0000, 0x5000, 0x5100, 0x5400, 0x5500, 0x5800,   */
/* 0x7458, 0x7d00, 0x7d20) because 0x13800000 is instance 4. That the   */
/* 40-entry channel table in the upload path also lands entirely inside */
/* instance 4 is a consistency check the family model passes.           */
/*                                                                    */
/* What the blocks ARE is not established: they are not in the Linux    */
/* device tree and neither the Z8 notes nor the local analysis names    */
/* 0x11800000. So this is a plain register file, and offset 0x02c is    */
/* the one exception the firmware forces: 411870d8 and 41187458 both    */
/* `cbz` on it, and 41187458's zero branch silently skips the whole     */
/* video upload. It is therefore modelled as a read-only capability     */
/* word that reads back 1, i.e. "present" -- the only thing either      */
/* consumer distinguishes. The value is a declared board choice, like   */
/* soc-revision, not a measured register; nothing claims bit meanings,  */
/* and a write there stops rather than being absorbed.                  */
/* ------------------------------------------------------------------ */

/*
 * Instances 0..3 are the four DDR channels and instance 4 writes to all of
 * them: the two routines that take DRAM through self-refresh program
 * 0x138xxxxx only and wait on the same offsets of 0x11800000, 0x12000000,
 * 0x12800000 and 0x13000000. They are the frequency change sub_418422B8
 * and the standby power-down sub_4019502C (from 0x401951c0):
 *
 *   +0x7800 = 0x100  enter      channels' +0x7940 reads 0x10
 *                               (sub_418422B8 waits == 0x10, the
 *                               power-down for bit 4)
 *   +0x7800 = 0x200  leave      +0x7940 reads 0 (0x41842560..80)
 *   +0x7458 bit 16   request    the frequency change waits for the
 *                               channels' +0x7458 == 0x80010001 after
 *                               writing 0x80010000; the power-down
 *                               writes (old | 0x80010000), waits for
 *                               bit 0 clear, drops bit 16 and waits for
 *                               bit 0 set
 *
 * So bit 0 of a channel's +0x7458 is an acknowledge that goes down while
 * a request is taken and up when it is done. DECLARED: every handshake
 * completes at once -- +0x7940 answers the 0x7800 command on the write,
 * and +0x7458 reads the written word with bit 0 = bit 31 (enable), except
 * that the first read of each channel after a write with bit 16 set shows
 * bit 0 clear. +0x7d20 and +0x7900, which the same routines wait to read
 * 0, read what they hold, 0. Nothing else is copied to the channels.
 */
static void z63_ddr_broadcast(NikonZ63State *s, hwaddr off, uint32_t value)
{
    for (unsigned ch = 0; ch < Z63_DDR_CHANNELS; ch++) {
        if (off == 0x7800 && value == 0x100) {
            s->sib[ch][0x7940 / 4] = 0x10;
        } else if (off == 0x7800 && value == 0x200) {
            s->sib[ch][0x7940 / 4] = 0;
        } else if (off == 0x7458) {
            s->sib[ch][0x7458 / 4] = (value & ~1u) | (value >> 31);
            s->ddr_req_busy[ch] = value & 0x10000;
        }
    }
}

static uint64_t z63_sib_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;
    unsigned inst;
    hwaddr off;

    if (size != 4 || (addr & 3) ||
        addr >= (hwaddr)Z63_BLK_INST * Z63_BLK_STRIDE) {
        z63_stop_read("SIBLING", Z63_BLK_BASE + addr, size);
        return 0;
    }
    inst = addr / Z63_BLK_STRIDE;
    off = addr % Z63_BLK_STRIDE;
    if (off == Z63_BLK_CAP) {
        return 1;
    }
    if (off >= Z63_BLK_WINDOW) {
        z63_stop_read("SIBLING", Z63_BLK_BASE + addr, size);
        return 0;
    }
    if (inst < Z63_DDR_CHANNELS && off == 0x7458 && s->ddr_req_busy[inst]) {
        s->ddr_req_busy[inst] = false;
        return s->sib[inst][off / 4] & ~1u;
    }
    return s->sib[inst][off / 4];
}

static void z63_sib_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    NikonZ63State *s = opaque;
    unsigned inst;
    hwaddr off;

    if (size != 4 || (addr & 3) ||
        addr >= (hwaddr)Z63_BLK_INST * Z63_BLK_STRIDE) {
        z63_stop_write("SIBLING", Z63_BLK_BASE + addr, size, value);
        return;
    }
    inst = addr / Z63_BLK_STRIDE;
    off = addr % Z63_BLK_STRIDE;
    if (off >= Z63_BLK_WINDOW) {
        z63_stop_write("SIBLING", Z63_BLK_BASE + addr, size, value);
        return;
    }
    if (off == Z63_BLK_CAP) {
        z63_stop_write("SIBLING", Z63_BLK_BASE + addr, size, value);
        return;
    }
    s->sib[inst][off / 4] = value;
    if (inst == Z63_DDR_CHANNELS) {
        z63_ddr_broadcast(s, off, value);
    }
}

static const MemoryRegionOps z63_sib_ops = {
    .read = z63_sib_read,
    .write = z63_sib_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Driver upload window at 0x14c00000                                   */
/*                                                                    */
/* sub_411870c8 fills exactly two 0x1ac-byte tables here, from ROM      */
/* 0x44b13d78 + n*0x1ac into +0x0000 and 0x44b145d4 + n*0x1ac into      */
/* +0x1000, where n is the same 0..4 instance selector. Both targets    */
/* are fixed, so this is a flat window and not an indexed array. The    */
/* region is sized to 0x2000, which covers both tables with slack; a    */
/* read of what was written returns it, and anything else stops.        */
/* ------------------------------------------------------------------ */

static uint64_t z63_upk_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || addr >= Z63_UPK_SIZE) {
        z63_stop_read("UPLOAD", Z63_UPK_BASE + addr, size);
        return 0;
    }
    return s->upk[addr / 4];
}

static void z63_upk_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    NikonZ63State *s = opaque;

    if (size != 4 || (addr & 3) || addr >= Z63_UPK_SIZE) {
        z63_stop_write("UPLOAD", Z63_UPK_BASE + addr, size, value);
        return;
    }
    s->upk[addr / 4] = value;
}

static const MemoryRegionOps z63_upk_ops = {
    .read = z63_upk_read,
    .write = z63_upk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* System reset controller, 0x0D154000 block                          */
/*                                                                    */
/* Two write-only windows over the same three 32-bit words:            */
/*                                                                    */
/*   0x0D154240 + 4*i   clear -- writing 1 clears bit i                */
/*   0x0D154340 + 4*i   set   -- writing 1 sets bit i                  */
/*                                                                    */
/* The set of bits the firmware can reach is not a guess. The image    */
/* carries an 83-entry table of (offset, bit) byte pairs at 0x4184BD5C,*/
/* consumed by the routine pair at 0x40c2dbb0 and 0x40c2dbf8:          */
/*                                                                    */
/*   cmp  w1, #0x52                 index > 82 returns                 */
/*   x8 = 0x4184BD5C + index*2      ldrb offset, ldrb bit              */
/*   x9 = 0x0D154000 + offset                                          */
/*   lsl  w8, #1, bit               str w8, [x9, #0x340]  (or #0x240)  */
/*                                                                    */
/* Unioning the 83 routes gives three masks -- 0x01FFFFFF, 0xFFFF7FFF, */
/* 0xFFFFF0FE -- and they are bit for bit the same three words the Z8  */
/* project derives from its own image (n-research z8-qemu,             */
/* src/reset-controls.h, from a table at 0x4164C494 with the same 83   */
/* routes). Two different EXPEED 7 images agreeing on the complete     */
/* route set is the strongest evidence available that these three      */
/* words are the whole window and that no other offset is addressable. */
/*                                                                    */
/* The whole boot touches this block six times, all in the offset-4    */
/* word, and the sequence is worth keeping:                            */
/*                                                                    */
/*   244 <- 0x02   clear bit 1   release CPU1                          */
/*   044 -> poll   status                                              */
/*   244 <- 0x04   clear bit 2   release CPU2                          */
/*   044 -> poll   status                                              */
/*   344 <- 0x80   set   bit 7                                         */
/*   244 <- 0x80   clear bit 7   reset pulse on the same resource      */
/*                                                                    */
/* CPU3 is never released: bit 3 of the clear word is never written.   */
/* That is not an omission in the model. The Linux device tree shipped */
/* in the same package declares a single cpu@3, so core 3 belongs to   */
/* the LI image and this stage deliberately leaves it in reset -- which*/
/* is why booting eg alone can never produce a Linux console.          */
/*                                                                    */
/* Only CPU0 runs from reset. For cores 1..3 the EL3 code in the image */
/* builds a boot block in the mailbox, clears the core's reset bit and */
/* polls +0x044 until the matching status bit appears. The model       */
/* performs exactly that: it validates the mailbox, resets the target  */
/* CPU and points it at the posted entry address.                      */
/*                                                                    */
/* Modelling: the six words are latched per address and read back      */
/* verbatim; no meaning is claimed for any bit except the core         */
/* releases above. A write carrying a bit outside the derived mask is  */
/* a route this firmware does not have, so it stops rather than guess. */
/* ------------------------------------------------------------------ */

static const uint32_t z63_reset_masks[3] = {
    0x01ffffffU, 0xffff7fffU, 0xfffff0feU,
};

static void z63_start_secondary(CPUState *cs, run_on_cpu_data data)
{
    ARMCPU *cpu = ARM_CPU(cs);
    uint64_t entry = data.host_ulong;

    cpu_reset(cs);
    cpu->env.pc = entry;
    cpu->power_state = PSCI_ON;
    cs->halted = false;
    error_report("Z63: released CPU%d at pc=0x%08" PRIx64, cs->cpu_index, entry);
}

static uint64_t z63_release_read(void *opaque, hwaddr offset, unsigned size)
{
    NikonZ63State *s = opaque;
    uint64_t front;

    /* ../zr-qemu's video models own six offsets of this page -- the
     * seven-bit hold/release bundle, video reset 15, the NR mask, the
     * peripheral selection and its acknowledgement -- and a movie
     * recording cannot start without them. They are asked first, as every
     * register window in this board asks them, and they decline anything
     * else, so the controller below still answers all of its own. */
    if (z63_front_read(Z63_RELEASE_BASE + offset, size, &front)) {
        return front;
    }

    if (offset == 0x44) {
        return s->release_status;
    }
    if (offset >= 0x240 && offset < 0x24c && !(offset & 3)) {
        return s->reset_word[(offset - 0x240) / 4];
    }
    if (offset >= 0x340 && offset < 0x34c && !(offset & 3)) {
        return s->reset_word[3 + (offset - 0x340) / 4];
    }
    z63_stop_read("RESET", Z63_RELEASE_BASE + offset, size);
    return 0;
}

static void z63_release_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    NikonZ63State *s = opaque;

    unsigned win, idx, i;

    if (z63_front_write(Z63_RELEASE_BASE + offset, value, size)) {
        return;             /* a video reset or peripheral select, above */
    }

    if (offset >= 0x240 && offset < 0x24c && !(offset & 3)) {
        win = 0;
    } else if (offset >= 0x340 && offset < 0x34c && !(offset & 3)) {
        win = 1;
    } else {
        z63_stop_write("RESET", Z63_RELEASE_BASE + offset, size, value);
        return;
    }
    idx = (offset - (win ? 0x340 : 0x240)) / 4;

    if (value & ~(uint64_t)z63_reset_masks[idx]) {
        z63_stop_write("RESET-ROUTE", Z63_RELEASE_BASE + offset, size, value);
        return;
    }
    s->reset_word[win * 3 + idx] = value;

    if (win == 1) {
        /* Writing a core bit in the set window holds that core in reset.
         * Two paths do, each after 0x0c86a010 shows the core in WFI (see
         * z63_cpustat_read): the power-down sub_4019502C holds cores 1 and
         * 2 (0x40195170, 0xf0007fb6) and the CPU3 off routine at
         * 0x40179f50 holds core 3 (0x40179f80, 8). The core is switched
         * off as PSCI CPU_OFF does, and its release bit drops, so a later
         * clear of the same bit starts it afresh from the mailbox. The
         * core bits are bits 1..3 of the offset-4 word only: the same bits
         * of words 0 and 2 are other routes of the table at 0x4184BD5C
         * (word 0 bit 1 is route 1), which are latched like every other
         * route. */
        for (i = 1; idx == 1 && i < Z63_NUM_CPU; i++) {
            if (!(value & (1U << i)) || !s->cpu[i] ||
                s->cpu[i]->power_state == PSCI_OFF) {
                continue;
            }
            error_report("Z63_RESET: CPU%u held in reset (+0x%" PRIx64
                         " = 0x%" PRIx64 ", pc=0x%" PRIx64 ")%s", i,
                         (uint64_t)offset, value, z63_current_pc(),
                         CPU(s->cpu[i])->halted ? ", from WFI" :
                         " while running");
            arm_set_cpu_off(i);
            s->release_status &= ~(1U << i);
        }
        return;
    }
    if (idx != 1) {
        return; /* only the offset-4 clear word carries core releases */
    }

    for (i = 1; i < 4; i++) {
        uint32_t magic = 0;
        uint64_t entry = 0;

        if (!(value & (1U << i))) {
            continue;
        }
        cpu_physical_memory_read(Z63_MAILBOX_MAGIC, &magic, sizeof(magic));
        cpu_physical_memory_read(Z63_MAILBOX_ENTRY + (i - 1) * 8, &entry,
                                 sizeof(entry));
        magic = le32_to_cpu(magic);
        entry = le64_to_cpu(entry);

        if (magic != Z63_BOOT_MAGIC || !entry) {
            error_report("Z63_RELEASE CPU%d: bad mailbox magic=0x%08x entry=0x%"
                         PRIx64 " -- stopping", i, magic, entry);
            qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,
                                                   3);
            return;
        }
        if (s->release_status & (1U << i)) {
            /*
             * The firmware now gets far enough to release CPU3 itself.
             *
             * That release is the real RTOS->Linux hand-off; the li-boot
             * path above is a stand-in for it, and it ran earlier because
             * the firmware used to die long before this point. Reaching
             * here is therefore progress, not a fault -- provided the
             * firmware wants CPU3 where the stand-in already put it.
             *
             * So compare, and only stop if they disagree. Restarting CPU3
             * is not an option either way: it is running Linux by now, and
             * resetting its PC would destroy a booted kernel to re-do a
             * hand-off that has already happened.
             */
            if (i == 3 && s->cpu3_entry != 0) {
                static bool reported;

                if (entry == s->cpu3_entry) {
                    if (!reported) {
                        error_report("Z63_RELEASE CPU3: the firmware released "
                                     "it at 0x%" PRIx64 ", which is where "
                                     "-machine cpu3-entry put it. The "
                                     "stand-in was right and CPU3 is left "
                                     "running; the hand-off is now the "
                                     "firmware's own.", entry);
                        reported = true;
                    }
                    continue;
                }
                /*
                 * They disagree, and the firmware's answer is the
                 * interesting one: it asks for CPU3 at the RTOS image
                 * entry, i.e. it is claiming CPU3 as a fourth RTOS core,
                 * not handing it to Linux. So -machine cpu3-entry is
                 * early, and whatever the body really does to start Linux
                 * happens later and by some other means -- that is an open
                 * question, written up in docs/plan.md.
                 *
                 * The stand-in is kept rather than obeyed, because it is
                 * the only thing that gets Linux booted at all today and a
                 * booted Linux is worth more than a tidier hand-off. This
                 * is a DECLARED CHOICE and the one place this board lets a
                 * stand-in outrank the firmware, so it says so every time
                 * and names both addresses.
                 */
                if (!reported) {
                    error_report("Z63_RELEASE CPU3: the firmware asked for "
                                 "0x%" PRIx64 " -- the RTOS image entry, so "
                                 "it wants CPU3 as a fourth RTOS core -- "
                                 "while -machine cpu3-entry already started "
                                 "it at 0x%" PRIx64 " for Linux. DECLARED "
                                 "CHOICE: the stand-in is kept and CPU3 "
                                 "stays in Linux. What the body really does "
                                 "to start Linux is not known; see "
                                 "docs/plan.md. Drop cpu3-entry to watch "
                                 "the firmware's own release instead.",
                                 entry, s->cpu3_entry);
                    reported = true;
                }
                continue;
            }
            error_report("Z63_RELEASE CPU%d released twice -- stopping", i);
            qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,
                                                   3);
            return;
        }
        async_run_on_cpu(CPU(s->cpu[i]), z63_start_secondary,
                         RUN_ON_CPU_HOST_ULONG(entry));
        s->release_status |= 1U << i;
    }
}

static const MemoryRegionOps z63_release_ops = {
    .read = z63_release_read,
    .write = z63_release_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* The cores' WFI status at 0x0c86a010                                  */
/*                                                                    */
/* The image reads one word of this page, from two places, and both    */
/* are about to cut a core's power:                                    */
/*                                                                    */
/*   40179f60  the CPU3 off routine (0x40179f50, w0 = core): ldr, tbnz */
/*             bit 3 -- holds core 3 in reset (0x0d154344 = 8) only     */
/*             when the bit is up, and returns -1 otherwise             */
/*   401950a0  the power-down that standby copies to WorkSRAM           */
/*             (sub_4019502C): spins until bits 1 and 2 are both up,    */
/*             then gates the clocks and holds cores 1 and 2            */
/*                                                                    */
/* Bit n is core n standing by in WFI -- what a cluster's STANDBYWFI    */
/* lines give -- the one reading under which both are right to pull     */
/* the core's reset as soon as it is set. ../z9-qemu answers the same   */
/* word with a constant 6.                                             */
/*                                                                    */
/* Modelled from the vCPUs: bit n is set while core n is halted (in     */
/* WFI with nothing pending) or switched off, which includes a core     */
/* never released. Read-only; no other offset of the page is named by   */
/* the image, so any other access stops.                               */
/* ------------------------------------------------------------------ */

#define Z63_CPUSTAT_WFI     0x010
#define Z63_CPUSTAT_LOGGED  16 /* changes reported before going quiet */

static uint64_t z63_cpustat_read(void *opaque, hwaddr addr, unsigned size)
{
    NikonZ63State *s = opaque;
    uint32_t value = 0;

    if (addr != Z63_CPUSTAT_WFI || size != 4) {
        z63_stop_read("CPUSTAT", Z63_CPUSTAT_BASE + addr, size);
        return 0;
    }
    for (unsigned i = 0; i < Z63_NUM_CPU; i++) {
        if (!s->cpu[i] || qatomic_read(&CPU(s->cpu[i])->halted) ||
            s->cpu[i]->power_state == PSCI_OFF) {
            value |= 1U << i;
        }
    }
    if (value != s->cpustat_last && s->cpustat_logged < Z63_CPUSTAT_LOGGED) {
        s->cpustat_logged++;
        error_report("Z63_CPUSTAT: cores in WFI or off = 0x%x (pc=0x%" PRIx64
                     ")", value, z63_current_pc());
    }
    s->cpustat_last = value;
    return value;
}

static void z63_cpustat_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    z63_stop_write("CPUSTAT", Z63_CPUSTAT_BASE + addr, size, value);
}

static const MemoryRegionOps z63_cpustat_ops = {
    .read = z63_cpustat_read,
    .write = z63_cpustat_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

/* ------------------------------------------------------------------ */
/* The LI (Linux) payload                                              */
/*                                                                     */
/* `li2010_*.bi` is not a flat image, it is a container:                */
/*                                                                     */
/*   0x000  "NISI"                       magic                          */
/*   0x010  u32 count = 4                followed by "0200i1"           */
/*   0x020  count x {u32 first_block, u32 blocks, u64 bytes}            */
/*   ...    the members, 0x800-aligned, in directory order              */
/*                                                                     */
/* Each entry is self-checking: its offset is first_block << 11 and its */
/* block count must be ceil(bytes / 0x800). On the shipped 2.00         */
/* container all four agree exactly -- the kernel at block 1 (6,101,000 */
/* bytes), the initramfs at block 0xba5 (21,274,826), and the two       */
/* device trees at blocks 0x343a (3,845) and 0x343c (3,168). The        */
/* container carries no member names, so members are identified here by */
/* content and never by directory order, and an unrecognised member is  */
/* an error rather than something to skip.                              */
/*                                                                     */
/* Where they go is not a guess either. The running image's own SOLID   */
/* region table (0x455C3810) declares its Linux window, and the device  */
/* tree in this same container declares a memory@ node that lands        */
/* exactly inside it, ending precisely where its share_mem begins. The  */
/* kernel goes at the base of that window: its Image header says        */
/* text_offset = 0, so it must start a 2 MiB-aligned region and may not */
/* be placed anywhere else. The device tree sits 32 MiB above it and    */
/* the initramfs above that, both 2 MiB aligned and both far below      */
/* share_mem.                                                           */
/*                                                                     */
/* The device tree is patched twice, and both patches are required:     */
/*                                                                     */
/*   - /chosen gets linux,initrd-start and linux,initrd-end. That is    */
/*     the only way arm64 Linux learns about an initramfs; there is no  */
/*     command-line alternative.                                        */
/*   - the console property is removed from the nkj-uart node. There it */
/*     reads "disabled", which makes nkj_uart_probe set is_real_serial  */
/*     false, skip request_irq and turn start_tx into a discard --       */
/*     transmit would still work, because nkj_console_putchar ignores   */
/*     the flag and writes the data register directly, but receive      */
/*     would never do anything. Z8's verified Linux reports non-zero rx */
/*     interrupt counts on ttyS0, so its device tree cannot have        */
/*     carried that property either.                                    */
/*                                                                     */
/* Two device trees ship in the container and they are NOT              */
/* interchangeable: one declares a memory window that lands exactly on  */
/* share_mem, the other declares a smaller one that does not. The one   */
/* consistent with the running image's region table is selected by      */
/* comparing reg against that window, and if neither matches -- or both */
/* do -- the load fails outright rather than silently choosing one.     */
/*                                                                     */
/* Scope: this function only places bytes and returns. It starts no CPU */
/* and describes no hardware. Booting this payload is a separate,       */
/* later step, and whatever stands in for the boot ROM there must be    */
/* labelled as the stand-in it is.                                      */
/* ------------------------------------------------------------------ */

/* The Linux window, as the running image's region table declares it. */
#define Z63_LI_MEM_BASE     0x0A07800000ULL
#define Z63_LI_MEM_SIZE     0x0D400000ULL
#define Z63_LI_MEMBER_MAX   8

static bool z63_li_dtb_matches_window(const void *dtb)
{
    int node = -1;

    while ((node = fdt_next_node(dtb, node, NULL)) >= 0) {
        const char *name = fdt_get_name(dtb, node, NULL);
        const fdt32_t *reg;
        int len;

        if (name == NULL || strncmp(name, "memory@", 7) != 0) {
            continue;
        }
        reg = fdt_getprop(dtb, node, "reg", &len);
        if (reg == NULL || len < 16) {
            continue;
        }
        /* Two 64-bit cells, big endian: base then size. */
        if (((uint64_t)fdt32_to_cpu(reg[0]) << 32 | fdt32_to_cpu(reg[1]))
              == Z63_LI_MEM_BASE &&
            ((uint64_t)fdt32_to_cpu(reg[2]) << 32 | fdt32_to_cpu(reg[3]))
              == Z63_LI_MEM_SIZE) {
            return true;
        }
    }
    return false;
}

static void z63_load_li(NikonZ63State *s)
{
    g_autofree char *file = NULL;
    GError *err = NULL;
    gsize len = 0;
    const uint8_t *img;
    const uint8_t *kernel = NULL, *initrd = NULL;
    const uint8_t *dtb[Z63_LI_MEMBER_MAX];
    size_t kernel_len = 0, initrd_len = 0;
    size_t dtb_len[Z63_LI_MEMBER_MAX];
    unsigned ndtb = 0, pick = Z63_LI_MEMBER_MAX;
    uint32_t count;

    if (s->li_image == NULL) {
        return;
    }
    if (!g_file_get_contents(s->li_image, &file, &len, &err)) {
        error_report("Z63_LI: cannot read '%s': %s", s->li_image, err->message);
        exit(1);
    }
    img = (const uint8_t *)file;
    if (len < 0x30 || memcmp(img, "NISI", 4) != 0) {
        error_report("Z63_LI: '%s' does not start with a NISI header",
                     s->li_image);
        exit(1);
    }
    count = ldl_le_p(img + 0x10);
    if (count == 0 || count > Z63_LI_MEMBER_MAX ||
        0x20 + (uint64_t)count * 16 > len) {
        error_report("Z63_LI: '%s' declares %u members, which does not fit a "
                     "%zu-byte file", s->li_image, count, (size_t)len);
        exit(1);
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = img + 0x20 + (size_t)i * 16;
        uint32_t first = ldl_le_p(e);
        uint32_t blocks = ldl_le_p(e + 4);
        uint64_t bytes = ldq_le_p(e + 8);
        uint64_t off = (uint64_t)first << Z63_LI_BLOCK_SHIFT;
        const uint8_t *p;

        if (bytes == 0 || off > len || bytes > len - off ||
            blocks != DIV_ROUND_UP(bytes, 1U << Z63_LI_BLOCK_SHIFT)) {
            error_report("Z63_LI: member %u contradicts its own directory "
                         "entry (block %#x, %u blocks, %" PRIu64 " bytes) in a "
                         "%zu-byte file", i, first, blocks, bytes, (size_t)len);
            exit(1);
        }
        p = img + off;

        if (bytes >= 2 && p[0] == 0x1f && p[1] == 0x8b) {
            initrd = p;
            initrd_len = bytes;
        } else if (bytes >= 8 && ldl_be_p(p) == 0xd00dfeed) {
            dtb[ndtb] = p;
            dtb_len[ndtb] = bytes;
            ndtb++;
        } else if (bytes >= 0x3c && memcmp(p + 0x38, "ARM\x64", 4) == 0) {
            kernel = p;
            kernel_len = bytes;
        } else {
            error_report("Z63_LI: member %u (%" PRIu64 " bytes, starting %02x "
                         "%02x %02x %02x) is neither an arm64 Image, a gzip "
                         "initramfs nor a device tree",
                         i, bytes, p[0], p[1], p[2], p[3]);
            exit(1);
        }
    }

    if (kernel == NULL || initrd == NULL || ndtb == 0) {
        error_report("Z63_LI: '%s' has kernel=%s initramfs=%s dtb=%u, and all "
                     "three are needed", s->li_image,
                     kernel ? "yes" : "no", initrd ? "yes" : "no", ndtb);
        exit(1);
    }

    for (unsigned i = 0; i < ndtb; i++) {
        if (z63_li_dtb_matches_window(dtb[i])) {
            if (pick != Z63_LI_MEMBER_MAX) {
                error_report("Z63_LI: two device trees both declare the "
                             "0x%" PRIx64 " window; refusing to choose",
                             (uint64_t)Z63_LI_MEM_BASE);
                exit(1);
            }
            pick = i;
        }
    }
    if (pick == Z63_LI_MEMBER_MAX) {
        error_report("Z63_LI: none of the %u device trees declares a memory@ "
                     "window of 0x%" PRIx64 " bytes at 0x%" PRIx64
                     " -- this container does not match this firmware",
                     ndtb, (uint64_t)Z63_LI_MEM_SIZE,
                     (uint64_t)Z63_LI_MEM_BASE);
        exit(1);
    }

    /* Patch the chosen tree in a scratch buffer, then hand both the buffer
     * and the payload members to the ROM loader, which copies them into RAM
     * as the machine resets. */
    {
        g_autofree uint8_t *buf = g_malloc0(Z63_LI_DTB_RESERVE);
        const uint64_t ir_end = Z63_LI_INITRD_ADDR + initrd_len;
        int rc, node;
        bool had_console;

        rc = fdt_open_into(dtb[pick], buf, Z63_LI_DTB_RESERVE);
        if (rc != 0) {
            error_report("Z63_LI: fdt_open_into on the %zu-byte device tree: "
                         "%s", dtb_len[pick], fdt_strerror(rc));
            exit(1);
        }
        node = fdt_path_offset(buf, "/chosen");
        if (node < 0) {
            error_report("Z63_LI: device tree has no /chosen node to put the "
                         "initramfs range in");
            exit(1);
        }
        rc = fdt_setprop_u64(buf, node, "linux,initrd-start",
                             Z63_LI_INITRD_ADDR);
        if (rc == 0) {
            rc = fdt_setprop_u64(buf, node, "linux,initrd-end", ir_end);
        }
        if (rc != 0) {
            error_report("Z63_LI: cannot set the initramfs range on /chosen: "
                         "%s", fdt_strerror(rc));
            exit(1);
        }

        node = fdt_node_offset_by_compatible(buf, -1, "nikon,nkj-uart");
        if (node < 0) {
            error_report("Z63_LI: device tree has no nikon,nkj-uart node, so "
                         "the serial driver's console flag cannot be cleared");
            exit(1);
        }
        had_console = fdt_getprop(buf, node, "console", NULL) != NULL;
        if (had_console) {
            rc = fdt_delprop(buf, node, "console");
            if (rc != 0) {
                error_report("Z63_LI: cannot remove the console property "
                             "from nikon,nkj-uart: %s", fdt_strerror(rc));
                exit(1);
            }
        }
        fdt_pack(buf);

        if (s->li_ready && s->li_restage) {
            uint8_t before[16];
            cpu_physical_memory_read(Z63_LI_KERNEL_ADDR, before, sizeof(before));
            error_report("ZF_LI_RESTAGE: kernel before reload %016" PRIx64
                         "%016" PRIx64 "; explicit direct-loader diagnostic, "
                         "NAND loading remains unverified",
                         ldq_le_p(before), ldq_le_p(before + 8));
            cpu_physical_memory_write(Z63_LI_KERNEL_ADDR, kernel, kernel_len);
            cpu_physical_memory_write(Z63_LI_INITRD_ADDR, initrd, initrd_len);
            cpu_physical_memory_write(Z63_LI_DTB_ADDR, buf, fdt_totalsize(buf));
        } else {
        rom_add_blob_fixed("z63-li-kernel", kernel, kernel_len,
                           Z63_LI_KERNEL_ADDR);
        rom_add_blob_fixed("z63-li-initrd", initrd, initrd_len,
                           Z63_LI_INITRD_ADDR);
        rom_add_blob_fixed("z63-li-dtb", buf, fdt_totalsize(buf),
                           Z63_LI_DTB_ADDR);
        }

        s->li_kernel_addr = Z63_LI_KERNEL_ADDR;
        s->li_dtb_addr = Z63_LI_DTB_ADDR;
        s->li_ready = true;

        error_report("Z63_LI: from '%s': kernel %zu bytes at 0x%" PRIx64
                     ", initramfs %zu bytes at 0x%" PRIx64
                     ", device tree %u bytes at 0x%" PRIx64
                     " (console property %s, initrd range %#" PRIx64
                     "-%#" PRIx64 ")",
                     s->li_image, kernel_len, (uint64_t)Z63_LI_KERNEL_ADDR,
                     initrd_len, (uint64_t)Z63_LI_INITRD_ADDR,
                     fdt_totalsize(buf), (uint64_t)Z63_LI_DTB_ADDR,
                     had_console ? "removed" : "was already absent",
                     (uint64_t)Z63_LI_INITRD_ADDR, ir_end);
    }
}

/* ------------------------------------------------------------------ */
/* CPU3 -> LI payload hand-off                                          */
/*                                                                     */
/* The RTOS never releases CPU3 on its own (the DTB declares the core, */
/* but the image has no path that starts it), so when the LI payload   */
/* is loaded and -machine cpu3-entry=0xADDR names the kernel entry,    */
/* this hands CPU3 to the LI payload. It is a labelled stand-in for    */
/* the boot ROM's role at the RTOS->Linux hand-off, not a model of the */
/* ROM: the arm64 boot-protocol state (x0 = the patched DTB's physical */
/* address, x1..x3 = 0) is written into CPU3's registers, and it is    */
/* started. The GIC, timer and memory mapping are already configured   */
/* by the RTOS before it ever reaches this point, which is why no GIC  */
/* shim is needed -- that is exactly what Nikon's BSP leaves empty in  */
/* gic_dist_init() for this SoC.                                        */
/*                                                                     */
/* The kernel cannot simply be dropped in at EL1h: its boot path       */
/* (`__enable_mmu`) assumes a bootloader has already invalidated the   */
/* branch predictors, the TLB and the instruction cache, and QEMU's    */
/* cold vCPU has none of that done, so the `isb` after the MMU-on      */
/* `msr sctlr_el1` never returns (the CPU re-executes from a stale TB  */
/* forever). On the real SoC the mask ROM's release path (service      */
/* 0x082013e0, called from the RTOS's Linux loader at 0x40c9ce30) does */
/* this maintenance and then `eret`s the kernel in at EL1h. That ROM   */
/* is not in any firmware component, so the short sequence below is    */
/* written into secure SRAM (which neither the eg nor the li image     */
/* ever touches) as a LABELLED STAND-IN for it -- helper code for the  */
/* The actual "run a stage like ATF's BL31, then drop to EL1" work is    */
/* done by QEMU's own arm_emulate_firmware_reset(cs, 1): it sets the     */
/* EL3 control state a real firmware would (SCR_EL3.RW/NS/HCE, clears    */
/* CPTR_EL3, sets HCR_EL2.RW) and puts the vCPU at EL1h with DAIF        */
/* masked, which is exactly the arm64 Linux boot protocol's requirement. */
/* Using it keeps every EL-mode control bit under QEMU's maintenance     */
/* instead of hand-assembling a stand-in for the ROM in guest memory.    */
/* This still is a labelled stand-in for the boot ROM's role at the      */
/* RTOS->Linux hand-off: the ROM itself is not in any firmware component,*/
/* so no code here models what it does -- only its externally visible    */
/* effect (CPU3 runs the kernel at EL1h) is reproduced.                 */
/*                                                                     */
/* This runs from the machine's reset callback, AFTER the device-level  */
/* cold reset, so the CPU state it writes is not wiped by the reset     */
/* that follows z63_init.                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Secure-side GIC hand-over for the non-secure core                   */
/*                                                                     */
/* The GIC has the security extensions (GICD_TYPER.SecurityExtn is 1), */
/* the RTOS runs at Secure EL1, and the LI payload runs at Non-secure  */
/* EL1. Every interrupt therefore starts out owned by the secure side, */
/* and a non-secure write to GICR_IGROUPR0 is ignored (RAZ/WI) for an  */
/* interrupt that is still secure. Linux's gic_cpu_init() does exactly */
/* that write to claim its SGIs and PPIs as Group 1 Non-secure, so     */
/* without a secure-side hand-over its timer PPI is never non-secure,  */
/* never enabled, and the core takes no interrupt at all: the kernel   */
/* reaches rest_init(), parks in cpu_do_idle() and is never woken, so  */
/* the driver initcalls that would bind a console never run.           */
/*                                                                     */
/* That hand-over is part of what the mask ROM's release path (service */
/* 0x082013e0) does on the real SoC, and that ROM is in no firmware    */
/* component, so this is a LABELLED STAND-IN for its externally        */
/* visible effect, in the same spirit as the rest of this hand-off:    */
/* CPU3's own SGI/PPI frame is moved to Group 1 Non-secure, written    */
/* through the device's normal MMIO path with secure attributes. The   */
/* other cores' frames and every SPI are left exactly as the RTOS      */
/* programmed them, so nothing the secure side owns changes.           */
/* ------------------------------------------------------------------ */

#define Z63_GICR_SGI_IGROUPR0  0x0080
#define Z63_GICR_SGI_IGRPMODR0 0x0D00

static void z63_gic_release_ppis_to_nonsecure(unsigned cpu)
{
    const hwaddr sgi = Z63_GIC_REDIST_BASE + (hwaddr)cpu * 0x20000 + 0x10000;
    const uint32_t group1 = 0xffffffffU;   /* IGROUPR0  = 1 for all 32 */
    const uint32_t grpmod = 0x00000000U;   /* IGRPMODR0 = 0 -> Group 1 NS */
    const MemTxAttrs secure = { .secure = 1 };
    MemTxResult r;

    r = address_space_write(&address_space_memory,
                            sgi + Z63_GICR_SGI_IGROUPR0,
                            secure, &group1, sizeof(group1));
    if (r != MEMTX_OK) {
        error_report("Z63_LI_BOOT: GICR_IGROUPR0 for CPU%u did not accept the "
                     "secure hand-over (MemTxResult %d)", cpu, r);
        return;
    }
    r = address_space_write(&address_space_memory,
                            sgi + Z63_GICR_SGI_IGRPMODR0,
                            secure, &grpmod, sizeof(grpmod));
    if (r != MEMTX_OK) {
        error_report("Z63_LI_BOOT: GICR_IGRPMODR0 for CPU%u did not accept the "
                     "secure hand-over (MemTxResult %d)", cpu, r);
        return;
    }
    error_report("Z63_LI_BOOT: CPU%u's 32 SGIs/PPIs moved to Group 1 "
                 "Non-secure from the secure side -- a labelled stand-in for "
                 "the part of mask ROM service 0x082013e0 that hands the "
                 "non-secure core its own interrupts", cpu);
}

/*
 * The hand-off touches another vCPU's register state, so it has to run in
 * that vCPU's own context. At machine reset that was free -- nothing was
 * executing. Now that it can fire from a timer while the RTOS is running
 * (see z63_li_wait_tick) it must go through async_run_on_cpu, exactly as
 * the mailbox release path does. Writing env.pc from the timer thread
 * instead produced a core fetching from 0x200: the state landed while the
 * vCPU was elsewhere and was partly overwritten.
 */
static void z63_cpu3_handoff_on_cpu(CPUState *cs, run_on_cpu_data data)
{
    NikonZ63State *s = data.host_ptr;
    ARMCPU *cpu = ARM_CPU(cs);

    error_report("Z63_LI_BOOT: handing CPU3 to the LI payload at 0x%"
                 PRIx64 " (dtb at 0x%" PRIx64 ") -- a labelled stand-in "
                 "for the boot ROM's RTOS->Linux hand-off",
                 s->cpu3_entry, s->li_dtb_addr);

    if (s->li_restage) {
        z63_load_li(s);
    }

    if (object_dynamic_cast(OBJECT(s), MACHINE_TYPE_NAME("nikon-zf"))) {
        cpu_reset(cs);
        error_report("ZF_LI_BOOT: reset CPU3 before Linux hand-off; "
                     "discard prior RTOS translation/control state");
    }

    /* arm64 Linux boot protocol: x0 = DTB physical address, x1..x3 = 0. */
    cpu->env.xregs[0] = s->li_dtb_addr;
    cpu->env.xregs[1] = 0;
    cpu->env.xregs[2] = 0;
    cpu->env.xregs[3] = 0;

    /* Configure the EL3 control state as a firmware stage would and drop
     * the vCPU to EL1h with DAIF masked (the arm64 boot protocol). This
     * replaces the mask ROM's release path (service 0x082013e0, called
     * from the RTOS's Linux loader at 0x40c9ce30) which is unavailable. */
    arm_emulate_firmware_reset(cs, 1);

    /* The same ROM service also hands the non-secure core its interrupts. */
    z63_gic_release_ppis_to_nonsecure(3);

    cpu->env.pc = s->cpu3_entry;
    cpu->power_state = PSCI_ON;
    cs->halted = false;
    s->release_status |= 1U << 3;
}

static void z63_cpu3_handoff(NikonZ63State *s, int num_cpu)
{
    async_run_on_cpu(CPU(s->cpu[3]), z63_cpu3_handoff_on_cpu,
                     RUN_ON_CPU_HOST_PTR(s));
    (void)num_cpu;
}

/* ------------------------------------------------------------------ */
/* The image window is NOR, and NOR is read-only at run time.          */
/*                                                                    */
/* The eg module is executed in place from this window -- its own      */
/* patcher calls 0x40160000 XIP_BASE -- and the RTOS's initial         */
/* translation table agrees: the 1 GiB block covering 0x40000000 is    */
/* the only one in the whole table with AP=2, EL1 read-only, while     */
/* every DRAM block above 0x800000000 is AP=0.                         */
/*                                                                    */
/* Mapping it as plain RAM let a guest store to 0x4016xxxx succeed     */
/* here and fail on the body, which is the worst kind of difference:   */
/* it only shows up after flashing. A ROM device keeps reads at RAM    */
/* speed, still allows the machine's own memcpy into the backing, and  */
/* stops on a guest write with the offending PC.                       */
/* ------------------------------------------------------------------ */

static void z63_image_window_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    z63_stop_write("IMAGE-WINDOW (NOR is read-only)",
                   Z63_IMAGE_WINDOW_BASE + addr, size, value);
}

static const MemoryRegionOps z63_image_window_ops = {
    .write = z63_image_window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * Wait for the RTOS to publish the share_mem directory, then hand CPU3 to
 * Linux. See the note by Z63_SHARE_MEM_BASE for why this is not done at
 * reset any more. Polling is the whole mechanism: it reads eight bytes of
 * guest DRAM every 10 ms of virtual time and changes nothing until the
 * firmware has written them.
 */
static void z63_li_wait_tick(void *opaque)
{
    NikonZ63State *s = opaque;
    MachineState *machine = MACHINE(s);
    uint8_t hdr[8] = { 0 };

    cpu_physical_memory_read(Z63_SHARE_MEM_BASE + 8, hdr, sizeof(hdr));
    if (memcmp(hdr, "MASTER", 7) != 0) {
        timer_mod(s->li_wait_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  Z63_SHARE_MEM_POLL_NS);
        return;
    }
    error_report("Z63_LI_BOOT: the RTOS has published share_mem's directory "
                 "at 0x%09" PRIx64 " (\"MASTER\" header present) -- handing "
                 "CPU3 to Linux now. Waiting for this is what keeps "
                 "LaMainApp's CIpcPorting_Initialize from failing; see the "
                 "note by Z63_SHARE_MEM_BASE.",
                 (uint64_t)Z63_SHARE_MEM_BASE);
    z63_cpu3_handoff(s, machine->smp.cpus);
}

static void z63_machine_reset(MachineState *machine, ResetType type)
{
    NikonZ63State *s = NIKON_Z63(machine);

    qemu_devices_reset(type);
    if (!(s->li_ready && s->cpu3_entry != 0 && machine->smp.cpus > 3)) {
        return;
    }
    if (!s->li_wait_ipc) {
        z63_cpu3_handoff(s, machine->smp.cpus);
        return;
    }
    if (!s->li_wait_timer) {
        s->li_wait_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        z63_li_wait_tick, s);
    }
    error_report("Z63_LI_BOOT: holding CPU3 until the RTOS publishes "
                 "share_mem's directory at 0x%09" PRIx64 ". Linux reaches "
                 "userspace in about 3 s and the emulated RTOS needs far "
                 "longer, so starting it at reset loses a race the body "
                 "never has. -machine li-wait-ipc=off starts it at once.",
                 (uint64_t)Z63_SHARE_MEM_BASE);
    timer_mod(s->li_wait_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + Z63_SHARE_MEM_POLL_NS);
}

/* ------------------------------------------------------------------ */
/* Machine init                                                        */
/* ------------------------------------------------------------------ */

/* The display window's keyboard, as camera buttons (nikon-z63-mcu.h). */
static void z63_kbd_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    NikonZ63State *s = NIKON_Z63(qdev_get_machine());
    InputKeyEvent *key;

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    key = evt->u.key.data;
    if (s->real_mcu_built) {
        z63_mcukeys_kbd(&s->real_mcu, qemu_input_key_value_to_qcode(key->key),
                        key->down);
        z63_release_contacts(s);
        return;
    }
    if (!s->mcu_peer || !s->mcu.queue) {
        return;
    }
    z63_mcu_kbd(&s->mcu, qemu_input_key_value_to_qcode(key->key), key->down);
}

static const QemuInputHandler z63_kbd_handler = {
    .name  = "Nikon Z 6III buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = z63_kbd_event,
};

/* ------------------------------------------------------------------ */
/* The real MCU's end of the serial link                               */
/* ------------------------------------------------------------------ */
/*
 * nikon-z63-sio.h: the MCU is wired to serial instances 0 and 1, the
 * firmware runs both in PIO mode, and the MCU -- not the EXPEED --
 * supplies the clock. Running the MCU's own image (nikon-z63-stm32.h)
 * puts a real SPI master on the far side of that: a word written to
 * SPI<n>.DR is one clocking step, and the word the instance shifts out in
 * exchange is what the MCU's next DR read returns. Both directions are
 * one call to z63_sio_clock() -- full duplex, exactly as SPI is.
 *
 * mcu-spi-peer= names which SPI is on which instance. The default,
 * "1:0+4:1" (SPI1 on serial 0, SPI4 on serial 1), started as a declared
 * choice and is now borne out by both firmwares: every frame the EXPEED
 * clocks out on serial 0 passes the MCU's own frame check (count, index,
 * checksum; sub_8038F08 in the MCU image) when it arrives on SPI1, and the
 * frames the MCU queues on SPI4 (DMA2 stream 1) pass the EXPEED's check on
 * serial 1 and get answered. Each frame both ways is named on stderr as
 * Z63_MCU_FRAME (z63_mcu_link_frame).
 */
typedef struct Z63McuSpiLink {
    Z63Stm32 *mcu;                  /* the real MCU, nikon-z63-stm32.h */
    Z63Sio *sio;
    unsigned spi_inst;              /* 0 = SPI1 */
    unsigned sio_inst;              /* which serial instance it is on */
    uint64_t words;
    bool pumping;                   /* re-entrancy guard */
    uint32_t why;                   /* which guard reported */
    /* The current 128-word frame each way, for z63_mcu_link_frame(). */
    uint8_t frame[2][256];
    unsigned at;                    /* words of the current frame so far */
    GHashTable *kinds;              /* (dir, family, route, kind) -> count */
    ZfBatteryStandin battery; /* opt-in wire report replacement */
} Z63McuSpiLink;

/* One exchange's two words. `tx` is what the MCU puts on the wire and `rx`
 * is what comes back; z63_sio_clock() hands the SIO's word to the callback
 * and takes the return as the word shifted in. */
typedef struct Z63McuSpiXfer {
    uint16_t tx, rx;
} Z63McuSpiXfer;

static uint16_t z63_mcu_spi_shift(void *opaque, unsigned inst, uint16_t out)
{
    Z63McuSpiXfer *x = opaque;

    x->rx = out;
    return x->tx;
}

/* One direction's frame is complete: name it by the transport's header
 * (nikon-z63-mcu.h: [0] family, [1] route, [2..3] body length, body from
 * [8] with its kind at [8], [0xff] = ~sum of the rest). An all-zero frame
 * is the idle side of a full-duplex exchange and is not named. Each
 * (direction, family, route, kind) is shown in full the first 4 times and
 * then counted, every 256th, so a retry loop reads as a count and not as
 * a flood. */
static void z63_mcu_link_frame(Z63McuSpiLink *l, unsigned dir)
{
    const uint8_t *f = l->frame[dir];
    unsigned len = f[2] | f[3] << 8, sum = 0;
    uintptr_t key;
    unsigned n;
    char body[3 * 48 + 1];

    if (!f[0]) {
        return;
    }
    for (unsigned i = 0; i < 0xff; i++) {
        sum += f[i];
    }
    if (!l->kinds) {
        l->kinds = g_hash_table_new(NULL, NULL);
    }
    key = (uintptr_t)dir << 24 | f[0] << 16 | f[1] << 8 | f[8];
    n = GPOINTER_TO_UINT(g_hash_table_lookup(l->kinds, (void *)key)) + 1;
    g_hash_table_insert(l->kinds, (void *)key, GUINT_TO_POINTER(n));
    if (n > 4 && n % 256) {
        return;
    }
    for (unsigned i = 0; i < MIN(len, 48u); i++) {
        snprintf(body + 3 * i, 4, "%02x ", f[8 + i]);
    }
    body[3 * MIN(len, 48u)] = 0;
    error_report("Z63_MCU_FRAME: %s #%u family %u route %u kind %u len %u "
                 "count %u index %u sum %s (SPI%u/serial %u): %s%s",
                 dir ? "MCU->EXPEED" : "EXPEED->MCU", n, f[0], f[1], f[8],
                 len, f[0xfc], f[0xfd],
                 (uint8_t)~sum == f[0xff] ? "ok" : "BAD",
                 l->spi_inst + 1, l->sio_inst, body, len > 48 ? "..." : "");
}

/* `out` is the MCU's word, `in` the EXPEED's. 128 words a frame, low
 * byte first. */
static void z63_mcu_link_note(Z63McuSpiLink *l, uint16_t out, uint16_t in)
{
    l->words++;
    l->frame[0][2 * l->at] = in;
    l->frame[0][2 * l->at + 1] = in >> 8;
    l->frame[1][2 * l->at] = out;
    l->frame[1][2 * l->at + 1] = out >> 8;
    if (++l->at == 128) {
        l->at = 0;
        z63_mcu_link_frame(l, 0);
        z63_mcu_link_frame(l, 1);
        if (zf_battery_standin_selected(&l->battery)) {
            l->battery.reports++;
            error_report("ZF_BATTERY_STANDIN: report %" PRIu64
                         " replaced112B family4/route2/kind3; explicit synthetic startup input",
                         l->battery.reports);
        }
    }
}

/* The CPU path: the firmware read or wrote SPI.DR itself. */
static uint16_t z63_real_mcu_spi_peer(void *opaque, unsigned inst,
                                      uint16_t out)
{
    Z63McuSpiLink *l = opaque;
    Z63McuSpiXfer x = { .tx = zf_battery_standin_word(&l->battery, l->at, out), .rx = 0 };

    if (z63_sio_clock(l->sio, l->sio_inst, 1, z63_mcu_spi_shift, &x)) {
        z63_mcu_link_note(l, x.tx, x.rx);
    }
    return x.rx;
}

/*
 * The DMA path, which is the one the image actually uses: the probe shows
 * DMA2 stream 0 armed on 0x4001300c (SPI1_DR) for 256 items and DMA1
 * stream 4 on 0x4000380c (SPI2_DR). The DMA controller in this model only
 * moves memory<->memory (z63_stm32_dma_item), so the peripheral is what
 * has to pull -- this is that pull, for the serial link.
 *
 * Two shapes, because the firmware uses both:
 *   - a TX stream is armed: take a word from memory and clock it out;
 *   - only an RX stream is armed: clock only while the SIO actually has a
 *     word waiting, or the transfer would fill SRAM with the zeroes an
 *     idle bus returns.
 * A word comes back into memory only when an RX stream is armed to take
 * it, which is what an unread DR does on the part.
 */
static void z63_mcu_link_pump(Z63McuSpiLink *l)
{
    uint32_t par = z63_stm32_spi_base[l->spi_inst] + 0x0c;      /* DR */
    Z63SioPio *pio = &l->sio->pio[l->sio_inst];

    if (l->pumping) {
        return;
    }
    /* Say once which condition kept the link quiet: without this, a silent
     * bridge looks the same whether the SPI is off, the instance is off, or
     * nobody called. Named per reason, so a run that starts working later
     * does not read as if it always had. */
    if (!(l->mcu->spi[l->spi_inst].r[0] & 0x40)) {              /* SPE */
        if (!(l->why & 1)) {
            l->why |= 1;
            error_report("Z63_MCU_LINK: SPI%u is not enabled (CR1.SPE "
                         "clear) -- the link waits", l->spi_inst + 1);
        }
        return;
    }
    if (!pio->active) {
        if (!(l->why & 2)) {
            l->why |= 2;
            error_report("Z63_MCU_LINK: serial instance %u is not enabled "
                         "-- the link waits", l->sio_inst);
        }
        return;
    }
    if (!(l->why & 4)) {
        l->why |= 4;
        error_report("Z63_MCU_LINK: SPI%u and serial %u are both up -- "
                     "clocking", l->spi_inst + 1, l->sio_inst);
    }
    l->pumping = true;
    for (;;) {
        Z63McuSpiXfer x = { .tx = 0, .rx = 0 };
        uint32_t v = 0;

        unsigned items = z63_stm32_dma_item_bytes(l->mcu, par, true);

        /* The EXPEED's instance is the side whose queued words clock the
         * wire (z63_sio_clock moves nothing without one), so ask it first.
         * Taking the MCU's bytes from its stream before that dropped them
         * whenever the instance's 64-word FIFO ran dry mid-frame -- the
         * EXPEED then read the MCU's 128-word frame from wherever the
         * stream had got to, misaligned, and threw it away. */
        if (!pio->active || !pio->tx_queued ||
            pio->rx_queued >= Z63_SIO_FIFO) {
            break;
        }
        if (items) {
            if (!z63_stm32_dma_tx(l->mcu, par, &v)) {
                break;
            }
            if (items == 1) {
                /* An 8-bit stream: this side's word is two frames, low
                 * byte first (SPI1 runs LSBFIRST). */
                uint32_t hi = 0;

                if (!z63_stm32_dma_tx(l->mcu, par, &hi)) {
                    break;
                }
                v |= (hi & 0xff) << 8;
            }
            x.tx = v;
        }
        x.tx = zf_battery_standin_word(&l->battery, l->at, x.tx);
        if (!z63_sio_clock(l->sio, l->sio_inst, 1, z63_mcu_spi_shift, &x)) {
            break;                  /* checked above; cannot happen */
        }
        z63_mcu_link_note(l, x.tx, x.rx);
        items = z63_stm32_dma_item_bytes(l->mcu, par, false);
        if (items == 1) {
            /* One 16-bit word is two byte-sized frames on the way in; the
             * firmware armed 256 items for a 128-word frame, which is how
             * that shows. Feeding it one item per word halved every frame
             * and left the MCU unable to validate anything. */
            z63_stm32_dma_rx(l->mcu, par, x.rx & 0xff);
            z63_stm32_dma_rx(l->mcu, par, (x.rx >> 8) & 0xff);
        } else if (items) {
            z63_stm32_dma_rx(l->mcu, par, x.rx);
        }
    }
    l->pumping = false;
}

/* The SIO calls this when an instance has words and nobody has clocked
 * them -- the same hook nikon-z63-mcu.h's synthetic peer uses. */
static void z63_mcu_link_kick(void *opaque, unsigned inst)
{
    z63_mcu_link_pump(opaque);
}

static void z63_mcu_links_pump(void *opaque)
{
    NikonZ63State *s = opaque;

    for (unsigned i = 0; i < Z63_STM32_SPI_N; i++) {
        if (s->mcu_spi_link[i].sio) {
            z63_mcu_link_pump(&s->mcu_spi_link[i]);
        }
    }
}

/* "1:0+4:1" -> SPI1 on serial 0, SPI4 on serial 1. Pairs are separated by
 * "+" and not "," because -machine splits its own value on commas. */
static void z63_mcu_spi_peer_setup(NikonZ63State *s, const char *spec)
{
    g_auto(GStrv) pairs = g_strsplit(spec ? spec : "", "+", -1);

    for (char **it = pairs; *it && **it; it++) {
        unsigned spi, sio;

        if (sscanf(*it, "%u:%u", &spi, &sio) != 2 || spi < 1 ||
            spi > 6 || sio >= Z63_SPI_INST) {
            error_report("mcu-spi-peer: '%s' is not <spi>:<serial> with spi "
                         "1..6 and serial 0..%u -- ignored", *it,
                         Z63_SPI_INST - 1);
            continue;
        }
        s->mcu_spi_link[spi - 1].battery.enabled = s->battery_standin;
        s->mcu_spi_link[spi - 1].mcu = &s->real_mcu;
        s->mcu_spi_link[spi - 1].sio = &s->sio;
        s->mcu_spi_link[spi - 1].spi_inst = spi - 1;
        s->mcu_spi_link[spi - 1].sio_inst = sio;
        z63_stm32_spi_peer(&s->real_mcu, spi - 1, z63_real_mcu_spi_peer,
                           &s->mcu_spi_link[spi - 1]);
        /* Clock it from whichever side speaks first: the MCU arming a
         * stream, or the SIO finding words with no one to clock them. */
        s->sio.kick[sio] = z63_mcu_link_kick;
        s->sio.kick_opaque[sio] = &s->mcu_spi_link[spi - 1];
        s->real_mcu.link_kick = z63_mcu_links_pump;
        s->real_mcu.link_kick_opaque = s;
        error_report("Z63_MCU_LINK: SPI%u <-> serial instance %u "
                     "(declared, see mcu-spi-peer=)", spi, sio);
    }
}

static void z63_init(MachineState *machine)
{
    NikonZ63State *s = (NikonZ63State *)machine;
    MemoryRegion *sysmem = get_system_memory();
    int num_cpu = machine->smp.cpus;

    /* CPUs: Cortex-A53, TCG only. Only CPU0 executes from reset -- the RTOS
     * releases cores 1..3 through the mailbox/release handshake, which is
     * how it gets its own entry address per core. All of them share the
     * image entry as rvbar, and the counter runs at 24 MHz (the image writes
     * exactly that to CNTFRQ_EL0 early on). */
    /* They are CPU cluster 0, as the body MCU's M4 is cluster 1
     * (nikon-z63-stm32.h): once any cluster exists QEMU's gdbstub makes one
     * inferior per cluster and puts unclustered CPUs in a last, unattached
     * one -- so without this a debugger attaching to the board would get the
     * M4 and not the A53s. The cluster is realized before the CPUs, as
     * zynqmp does for its APUs, so their translations are tagged with it. */
    object_initialize_child(OBJECT(machine), "a53-cluster", &s->a53_cluster,
                            TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->a53_cluster), "cluster-id", 0);
    for (int i = 0; i < num_cpu; i++) {
        Object *cpuobj = object_new(machine->cpu_type);
        CPUState *cs = CPU(cpuobj);

        object_property_add_child(OBJECT(&s->a53_cluster), "cpu[*]", cpuobj);
        object_unref(cpuobj);
        cs->start_powered_off = (i != 0);
        object_property_set_uint(cpuobj, "rvbar", Z63_IMAGE_ENTRY, &error_fatal);
        object_property_set_int(cpuobj, "mp-affinity", i, &error_fatal);
        object_property_set_int(cpuobj, "cntfrq", 24000000, &error_fatal);
        s->cpu[i] = ARM_CPU(cpuobj);
    }
    qdev_realize(DEVICE(&s->a53_cluster), NULL, &error_fatal);
    for (int i = 0; i < num_cpu; i++) {
        qdev_realize(DEVICE(s->cpu[i]), NULL, &error_fatal);
    }
    /* -machine service-core-events=on: ../zr-qemu's target/arm patch
     * (arm-service-events.patch), wired as its board wires it -- every A53
     * signals with SEV, and CPU3, the core the LI payload runs on, halts in
     * WFE until one arrives. Upstream QEMU's WFE only yields; under -icount
     * a core polling its mailbox around WFE then spends the instruction
     * clock while the others wait, and this board never reached a live
     * view (docs/zr-port.md). */
    for (int i = 0; s->service_core_events && i < num_cpu; i++) {
        s->cpu[i]->wl_event_peer = true;
        s->cpu[i]->wl_event_block = i == 3;
    }

    z63_create_gic(s, num_cpu);

    /* Catch-all trap at the lowest priority */
    memory_region_init_io(&s->unmapped, OBJECT(machine), &z63_unmapped_ops, s,
                          "z63-unmapped", UINT64_C(0x1000000000));
    memory_region_add_subregion_overlap(sysmem, 0, &s->unmapped, -1000);

    if (object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("nikon-zf"))) {
        zf_dbi_init(s, sysmem);
    }

    /* Real RAM */
    memory_region_init_ram(&s->dram, NULL, "z63-dram", Z63_DRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, Z63_DRAM_BASE, &s->dram);

    /* Place the LI payload, if one was named. This only fills the Linux
     * window; the cores that will run it are still held. See z63_load_li. */
    z63_load_li(s);

    /* When the LI payload is in place and -machine cpu3-entry=0xADDR names
     * where CPU3 should start, that core is handed to the LI payload. The
     * actual register writes and the stub load happen in z63_cpu3_handoff,
     * which runs from the machine's reset callback: the machine-level cold
     * reset follows z63_init and would otherwise wipe both the stub's RAM
     * page and the CPU state (cpu_reset would put CPU3's PC back at the
     * image's rvbar and re-halt it). */

    /* The boot ROM's service page, only when explicitly asked for. Without
     * it, reaching the page stops the VM and says where from. */
    /*
     * WorkSRAM, 0x08200000-0x08400000 in the SoC's own region table.
     *
     * This used to be unmapped except for one refusing page at 0x08201000,
     * on the premise that the boot ROM's service page is mask ROM that no
     * firmware component contains. That premise is wrong, and the firmware
     * says so: sub_40D710DC copies 0x1000 bytes from **0x40195000 in the eg
     * image** to 0x08201000 and then uses the page. The page's code ships in
     * the package; what was missing was somewhere to put it.
     *
     * So WorkSRAM is RAM here and the firmware installs its own service
     * page. The rom-page-stub option still pre-fills the six known entries
     * with `ret`, because they are called before that copy runs, but it is
     * now a pre-fill of RAM the firmware is free to overwrite rather than a
     * region that refuses to be written.
     *
     * The boot mailbox at 0x0821f000 is a page of this same WorkSRAM, so
     * it keeps its own region and priority and this one goes underneath.
     */
    memory_region_init_ram(&s->buffer_ext, NULL, "z63-buffer-ext",
                           Z63_BUFFER_EXT_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, Z63_BUFFER_EXT_BASE, &s->buffer_ext);

    memory_region_init_ram(&s->work_sram, NULL, "z63-work-sram",
                           Z63_WORK_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion_overlap(sysmem, Z63_WORK_SRAM_BASE,
                                       &s->work_sram, -1);

    if (s->rom_page_stub) {
        /*
         * Pre-fill the six known service entries with `ret` in WorkSRAM.
         *
         * These are called before sub_40D710DC installs the real page from
         * 0x40195000, so something has to be there first; the real boot ROM
         * presumably leaves its own copy. This writes four bytes per entry
         * into ordinary RAM and nothing else, so the firmware's own copy
         * overwrites it in full. It is a pre-fill, not a device: a service
         * whose return value matters will get 0 from x0 and that is not a
         * model of anything.
         */
        void *sram = memory_region_get_ram_ptr(&s->work_sram);
        hwaddr page = Z63_ROMPAGE_BASE - Z63_WORK_SRAM_BASE;

        for (size_t i = 0; i < ARRAY_SIZE(z63_rom_entries); i++) {
            uint32_t ret = cpu_to_le32(0xd65f03c0);

            memcpy((char *)sram + page + z63_rom_entries[i].offset,
                   &ret, sizeof(ret));
        }
        memory_region_init_io(&s->rom_page, OBJECT(machine),
                              &z63_rompage_keep_ops, s, "z63-boot-rom-page",
                              Z63_ROMPAGE_SIZE);
        memory_region_add_subregion_overlap(sysmem, Z63_ROMPAGE_BASE,
                                            &s->rom_page, 1);
        error_report("Z63: the %zu boot-ROM service entries at 0x%08" PRIx64
                     " answer `ret`; the rest of the page is whatever the "
                     "firmware copies there. This stands in for the loader "
                     "at 0x40000000, which is in no input -- see "
                     "docs/serial-nor-resources.md. A service whose return "
                     "value matters is not modelled.",
                     ARRAY_SIZE(z63_rom_entries), (uint64_t)Z63_ROMPAGE_BASE);
    }

    /* Boot mailbox: 4 KiB of RAM the CPU-release handshake reads back. The
     * RTOS posts a 0x0400a151 block plus each secondary's entry address
     * here before ringing the release controller. */
    memory_region_init_ram(&s->mailbox, NULL, "z63-boot-mailbox",
                           Z63_MAILBOX_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, Z63_MAILBOX_BASE, &s->mailbox);

    /* The RTOS image window is RAM on the real camera (and the header
     * declares it 128 MiB). Model it as RAM: self-modifying code, the
     * runtime log area and the boot ROM's patched tables all live here. */
    memory_region_init_rom_device(&s->rom, NULL, &z63_image_window_ops, s,
                                  "z63-image-window", Z63_ROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, Z63_IMAGE_WINDOW_BASE, &s->rom);

    /* SNOR_USER on top of the image window: same zeros, but now the board
     * reports who reads it. See the comment above z63_snor_user_read. */
    if (s->snor_user_file) {
        gsize len = 0;
        g_autofree char *buf = NULL;
        GError *gerr = NULL;

        if (!g_file_get_contents(s->snor_user_file, &buf, &len, &gerr)) {
            error_report("Z63_SNOR_USER: %s", gerr->message);
            exit(1);
        }
        if (len != Z63_SNOR_USER_SIZE) {
            error_report("Z63_SNOR_USER: %s is %" G_GSIZE_FORMAT " bytes; "
                         "this region is 0x%x. Nothing is padded or "
                         "truncated here -- build the file with "
                         "z63-tools/snor_user_input.py.",
                         s->snor_user_file, len, Z63_SNOR_USER_SIZE);
            exit(1);
        }
        s->snor_data = g_memdup2(buf, len);
        /* The board cannot tell a real body dump from synthetic framing, so
         * it states the file and says nothing about which it is. The tool
         * that produced it records that, and docs/serial-nor-resources.md
         * says why it matters. */
        error_report("Z63_SNOR_USER: loaded %s (%#x bytes) -- see "
                     "docs/serial-nor-resources.md for what this region is "
                     "and how the file was produced.",
                     s->snor_user_file, Z63_SNOR_USER_SIZE);
    }
    memory_region_init_io(&s->snor_user, OBJECT(machine), &z63_snor_user_ops,
                          s, "z63-snor-user", Z63_SNOR_USER_SIZE);
    memory_region_add_subregion_overlap(sysmem, Z63_SNOR_USER_BASE,
                                        &s->snor_user, 1);
    /* The controller that erases and programs it (nikon-z63-nor.h). */
    z63_nor_init(OBJECT(machine), sysmem, &s->nor, &s->snor_data,
                 Z63_SNOR_USER_BASE - Z63_NOR_FLASH_VA, Z63_SNOR_USER_SIZE);

    /* A small scratch page around the boot header in case the boot ROM
     * re-reads it from an SRAM alias; harmless if never touched. */
    memory_region_init_ram(&s->boot_header_ram, NULL, "z63-boot-header",
                           0x1000, &error_fatal);

    /* UART sink: plain MemoryRegion overlay plus one wired IRQ line.
     * No SysBusDevice needed for an anonymous trap region. */
    if (!qemu_chr_fe_init(&s->serial, serial_hd(0), &error_abort)) {
        /* no serial requested; keep a null backend */
    }
    qemu_chr_fe_set_handlers(&s->serial, z63_uart_can_receive,
                             z63_uart_receive, z63_uart_event, NULL, s, NULL,
                             true);
    memory_region_init_io(&s->uart_region, OBJECT(machine), &z63_uart_ops, s,
                          "z63-uart", Z63_UART_SIZE);
    memory_region_add_subregion(sysmem, Z63_UART_BASE, &s->uart_region);
    /* Both of the port's interrupts, in the driver's order: index 0 is the
     * rx irq (SPI 173) and index 1 is tx_irq (SPI 176). */
    for (unsigned port = 0; port < Z63_UART_PORTS; port++) {
        s->uart_irq[port] = qdev_get_gpio_in(
            s->gic, Z63_GIC_SPI(Z63_UART_IRQ_SPI + port));
        s->uart_tx_irq[port] = qdev_get_gpio_in(
            s->gic, Z63_GIC_SPI(Z63_UART_TX_IRQ_SPI + port));
    }

    z63_create_dma(s);

    /* CPU release controller: read +0x44 for status, write 1<<cpu to +0x244
     * to start one. Anything else stops the VM (see z63_release_write). */
    memory_region_init_io(&s->cpu_release, OBJECT(machine), &z63_release_ops, s,
                          "z63-cpu-release", Z63_RELEASE_SIZE);
    memory_region_add_subregion(sysmem, Z63_RELEASE_BASE, &s->cpu_release);
    memory_region_init_io(&s->cpustat, OBJECT(machine), &z63_cpustat_ops, s,
                          "z63-cpustat", Z63_CPUSTAT_SIZE);
    memory_region_add_subregion(sysmem, Z63_CPUSTAT_BASE, &s->cpustat);

    /* Bootstrap latches (see the note by z63_int_latch_write). */
    memory_region_init_io(&s->int_latch, OBJECT(machine), &z63_int_latch_ops,
                          &s->int_latch_value, "z63-int-latch", 4);
    memory_region_add_subregion(sysmem, Z63_INT_LATCH_BASE, &s->int_latch);
    memory_region_init_io(&s->counter, OBJECT(machine), &z63_counter_ops,
                          &s->counter_value, "z63-counter-enable", 4);
    memory_region_add_subregion(sysmem, Z63_COUNTER_BASE, &s->counter);

    /* Pin configuration ("gpio-top", see z63_gpio_write). */
    memory_region_init_io(&s->gpio_top, OBJECT(machine), &z63_gpio_ops, s,
                          "z63-gpio-top", 0x1000);
    memory_region_add_subregion(sysmem, Z63_GPIO_TOP_BASE, &s->gpio_top);

    /* TOP block: identity word plus configuration words (see z63_top_read). */
    memory_region_init_io(&s->top, OBJECT(machine), &z63_top_ops, s,
                          "z63-top", Z63_TOP_SIZE);
    memory_region_add_subregion(sysmem, Z63_TOP_BASE, &s->top);

    /* Clock/power configuration bank (see z63_cgn_write). */
    memory_region_init_io(&s->cgn, OBJECT(machine), &z63_cgn_ops, s,
                          "z63-cgn", Z63_CGN_SIZE);
    memory_region_add_subregion(sysmem, Z63_CGN_BASE, &s->cgn);

    /* One more TOP word, past the peripheral control words: selector modes
     * packed by sub_40C2EFD0 (mode 1 writes 0x8000 | n, mode 2 0xc000 |
     * fields, else 0), reached once the MCU-driven start runs. ../z9-qemu
     * retains the same word for its 40c2ebd0. Storage only. */
    {
        static const uint16_t top_sel[] = { 0x000 };
        z63_regwindow_init(OBJECT(machine), sysmem, &s->top_sel, "TOP-SEL",
                           0x0D150500ULL, 4, top_sel, ARRAY_SIZE(top_sel));
    }

    /* Peripheral control words (see z63_pctl_write). */
    memory_region_init_io(&s->pctl, OBJECT(machine), &z63_pctl_ops, s,
                          "z63-peripheral-control", Z63_PCTL_WORDS * 4);
    memory_region_add_subregion(sysmem, Z63_PCTL_BASE, &s->pctl);

    /* Configuration sequencer (see z63_seq_read). */
    memory_region_init_io(&s->seq, OBJECT(machine), &z63_seq_ops, s,
                          "z63-sequencer", Z63_SEQ_SIZE);
    memory_region_add_subregion(sysmem, Z63_SEQ_BASE, &s->seq);

    /* Identification/diagnostic block (see z63_id_read). */
    memory_region_init_io(&s->id, OBJECT(machine), &z63_id_ops, s,
                          "z63-id-block", Z63_ID_SIZE);
    memory_region_add_subregion(sysmem, Z63_ID_BASE, &s->id);

    /* Configuration bank (see z63_cfg7_read). */
    memory_region_init_io(&s->cfg7, OBJECT(machine), &z63_cfg7_ops, s,
                          "z63-cfg7", Z63_CFG7_SIZE);
    memory_region_add_subregion(sysmem, Z63_CFG7_BASE, &s->cfg7);

    /* Serial-bus controllers: twelve instances 0x100 apart, the register
     * set taken from the driver rather than from what had been seen. The
     * evidence, including the one offset that rests on the Linux device
     * tree instead of on the firmware, is in nikon-z63-spibus.h. */
    z63_sio_init(OBJECT(machine), sysmem, s->gic, &s->sio);

    /* SD host controllers (see docs/storage-registers.md). */
    /* Two blocks whose register sets were read out of the firmware; see
     * nikon-z63-regwindow.h for the contract and each header for the
     * evidence behind its offset set. */
    {
        /* The harvested set plus device 0x190's two pages (the comment
         * above z63_sensorif_dev190_offsets). */
        size_t n0 = ARRAY_SIZE(z63_sensorif_offsets);
        size_t n1 = ARRAY_SIZE(z63_sensorif_dev190_offsets);
        size_t n2 = ARRAY_SIZE(z63_sensorif_dev190_bank1_offsets);
        size_t n3 = ARRAY_SIZE(z63_sensorif_dev190_bank34_offsets);
        size_t n4 = ARRAY_SIZE(z63_sensorif_dev18e_ack_offsets);
        g_autofree uint16_t *offs = g_new(uint16_t,
                                          n0 + n1 + n2 + 2 * n3 + n4);

        memcpy(offs, z63_sensorif_offsets, sizeof(z63_sensorif_offsets));
        for (size_t i = 0; i < n1; i++) {
            offs[n0 + i] = 0x1000 + z63_sensorif_dev190_offsets[i];
        }
        for (size_t i = 0; i < n2; i++) {
            offs[n0 + n1 + i] = 0x4000 + z63_sensorif_dev190_bank1_offsets[i];
        }
        for (size_t i = 0; i < n3; i++) {
            offs[n0 + n1 + n2 + i] =
                0x5000 + z63_sensorif_dev190_bank34_offsets[i];
            offs[n0 + n1 + n2 + n3 + i] =
                0x5400 + z63_sensorif_dev190_bank34_offsets[i];
        }
        memcpy(offs + n0 + n1 + n2 + 2 * n3, z63_sensorif_dev18e_ack_offsets,
               sizeof(z63_sensorif_dev18e_ack_offsets));
        z63_regwindow_init(OBJECT(machine), sysmem, &s->sensorif,
                           "SENSOR-IF", Z63_SENSORIF_BASE, Z63_SENSORIF_SIZE,
                           offs, n0 + n1 + n2 + 2 * n3 + n4);
        if (object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("nikon-zf"))) {
            /* Own 40c2c2f4/314/344 select DC/E0 and replace bits3/1..2/0.
             * E0 retains original writes like DC; no status/IRQ side effect. */
            set_bit(0x00e0 / 4, s->sensorif.modelled);
            /* Own 40c2d2dc writes W2&3 to observed base15083000+420/424. */
            set_bit(0x3420 / 4, s->sensorif.modelled);
            set_bit(0x3424 / 4, s->sensorif.modelled);
            /* Same original function, selector2 now observed at40c2d304. */
            set_bit(0x3428 / 4, s->sensorif.modelled);
            /* Own 40c2d318 writes W1&1 to observed base15083000+42c. */
            set_bit(0x342c / 4, s->sensorif.modelled);
            /* Own 40c2c9e0 selects EC/F0 and stores W2&1; EC now observed. */
            set_bit(0x00ec / 4, s->sensorif.modelled);
            /* Own 40c2c214 selects D0/D4 and stores W2&1; only D0 observed. */
            set_bit(0x00d0 / 4, s->sensorif.modelled);
            /* Same original setter, second bank D4 now observed as well. */
            set_bit(0x00d4 / 4, s->sensorif.modelled);
            /* Own40c2a120 packs four input u32s to observed10a8..10b4. */
            for (unsigned i = 0; i < 4; i++) {
                set_bit((0x10a8 + 4 * i) / 4, s->sensorif.modelled);
            }
            /* Own32-row loop40c2a990: only the25 observed missing words. */
            for (unsigned i = 0; i < 25; i++) {
                set_bit((0x1280 + 4 * i) / 4, s->sensorif.modelled);
            }
            /* Complete the own fixed32-word table; actual base/input now bound. */
            for (unsigned i = 25; i < 32; i++) {
                set_bit((0x1280 + 4 * i) / 4, s->sensorif.modelled);
            }
            /* Own40c2aa4c selects these two words and packs two5-bit inputs. */
            set_bit(0x133c / 4, s->sensorif.modelled);
            set_bit(0x1340 / 4, s->sensorif.modelled);
        }
    }
    {
        /* The harvested family set plus device 0x1a0's acknowledge
         * words (the comment above z63_isp_apb_1a0_ack_offsets). */
        size_t n0 = ARRAY_SIZE(z63_isp_apb_offsets);
        size_t n1 = ARRAY_SIZE(z63_isp_apb_1a0_ack_offsets);
        g_autofree uint16_t *offs = g_new(uint16_t, n0 + n1);

        memcpy(offs, z63_isp_apb_offsets, sizeof(z63_isp_apb_offsets));
        memcpy(offs + n0, z63_isp_apb_1a0_ack_offsets,
               sizeof(z63_isp_apb_1a0_ack_offsets));
        z63_regwindow_init(OBJECT(machine), sysmem, &s->isp_apb_1a0,
                           "ISP-APB-DEV-0x1A0", Z63_ISP_APB_1A0_BASE,
                           Z63_ISP_APB_DEV_SIZE, offs, n0 + n1);
    }

    /* The two windows docs/storage-registers.md section 8 separates from the
     * SD HAL. They were left unmapped on purpose until the crank reached
     * them, at 0x17800118 from pc 0x40c5dfd0. The first is the NAND flash
     * controller, whose COMP bit a register file could never set; see
     * nikon-z63-nand.h. */
    z63_nand_init(OBJECT(machine), sysmem, s->gic, &s->nand, s->nand_file);
    {
        /* The generated set plus z63_exs1784_more_offsets. */
        size_t n0 = ARRAY_SIZE(z63_exs1784_offsets);
        size_t n1 = ARRAY_SIZE(z63_exs1784_more_offsets);
        g_autofree uint16_t *offs = g_new(uint16_t, n0 + n1);

        memcpy(offs, z63_exs1784_offsets, sizeof(z63_exs1784_offsets));
        memcpy(offs + n0, z63_exs1784_more_offsets,
               sizeof(z63_exs1784_more_offsets));
        z63_regwindow_init(OBJECT(machine), sysmem, &s->exs1784, "EXS1784",
                           Z63_EXS1784_BASE, Z63_EXS1784_SIZE, offs,
                           n0 + n1);
    }
    z63_regwindow_init(OBJECT(machine), sysmem, &s->blk15d8, "BLK-0x15D8",
                       Z63_BLK15D8_BASE, Z63_BLK15D8_SIZE,
                       z63_exs15d8_offsets,
                       ARRAY_SIZE(z63_exs15d8_offsets));
    memory_region_init_ram(&s->dspmem, NULL, "z63-dsp-mem",
                           Z63_DSPMEM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, Z63_DSPMEM_BASE, &s->dspmem);
    for (unsigned i = 0; i < ARRAY_SIZE(z63_dspmem_more); i++) {
        g_autofree char *name = g_strdup_printf("z63-dsp-mem-%x",
                                                (unsigned)(z63_dspmem_more[i] >> 20));

        memory_region_init_ram(&s->dspmem_more[i], NULL, name,
                               Z63_DSPMEM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, z63_dspmem_more[i],
                                    &s->dspmem_more[i]);
    }

    z63_regwindow_init(OBJECT(machine), sysmem, &s->dsp1b80, "DSP-0x1B800000",
                       Z63_DSP1B80_BASE, Z63_DSP1B80_SIZE,
                       z63_dsp1b80_offsets,
                       ARRAY_SIZE(z63_dsp1b80_offsets));
    for (size_t i = 0; i < ARRAY_SIZE(z63_dsp1c80); i++) {
        z63_regwindow_init(OBJECT(machine), sysmem, &s->dsp1c80[i],
                           z63_dsp1c80[i].name, z63_dsp1c80[i].base,
                           Z63_DSP1C80_SIZE, z63_dsp1c80_offsets,
                           ARRAY_SIZE(z63_dsp1c80_offsets));
    }
    /* Their mailboxes, and a boot message from each (nikon-z63-dspipc.h). */
    QEMU_BUILD_BUG_ON(ARRAY_SIZE(z63_dspipc) != ARRAY_SIZE(s->dsp1c80));
    z63_dspipc_init(OBJECT(machine), sysmem, s->gic, s->dsp1c80);

    s->storage.caps_stub = s->storage_caps_stub;
    z63_create_storage(&s->storage, OBJECT(machine), sysmem);
    if (s->zf_sd_drive && *s->zf_sd_drive) {
        zf_sd_init(OBJECT(machine), sysmem, s->gic, &s->noc, s->zf_sd_drive);
    }
    z63_wlsd_init(OBJECT(machine), sysmem, s->gic, &s->wlsd_vendor);
    if (s->zf_microsd_drive && *s->zf_microsd_drive) {
        zf_sd_init_second(OBJECT(machine), sysmem, s->gic, &s->noc,
                          s->zf_pcie_host, s->zf_microsd_drive);
    }
    z63_regwindow_init(OBJECT(machine), sysmem, &s->dev199,
                       "ISP-APB-DEV-0x199", Z63_DEV199_BASE, Z63_DEV199_SIZE,
                       z63_dev199_offsets, ARRAY_SIZE(z63_dev199_offsets));
    if (object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("nikon-zf"))) {
        /* Reuse the existing Z9 configuration masks only for whole Zf contracts.
         * Storage retains original values; no mask clipping or event source. */
        for (unsigned off = 0; off < Z63_DEV199_SIZE; off += 4) {
            if (zf_isp199_config_offset_verified(off)) {
                assert(z63_z9lab_isp199_config_mask(off) != 0);
                set_bit(off / 4, s->dev199.modelled);
            }
        }
    }
    QEMU_BUILD_BUG_ON(ARRAY_SIZE(z63_ispfam_devs) != ARRAY_SIZE(s->ispfam));
    z63_ispfam_init(OBJECT(machine), sysmem, s->ispfam);
    z63_regwindow_init_strided(OBJECT(machine), sysmem, &s->stream154c,
                               "STREAM154C", Z63_STRM_BASE, Z63_STRM_SIZE,
                               Z63_STRM_STRIDE, z63_strm_offsets,
                               ARRAY_SIZE(z63_strm_offsets));
    z63_regwindow_init(OBJECT(machine), sysmem, &s->tenc, "TENC",
                       Z63_TENC_BASE, Z63_TENC_SIZE, z63_tenc_offsets,
                       ARRAY_SIZE(z63_tenc_offsets));
    z63_regwindow_init(OBJECT(machine), sysmem, &s->tenc2, "TENC-2",
                       Z63_TENC2_BASE, Z63_TENC2_SIZE, z63_tenc2_offsets,
                       ARRAY_SIZE(z63_tenc2_offsets));
    z63_regwindow_init(OBJECT(machine), sysmem, &s->tdec, "TDEC",
                       Z63_TDEC_BASE, Z63_TDEC_SIZE, z63_tdec_offsets,
                       ARRAY_SIZE(z63_tdec_offsets));
    z63_regwindow_init(OBJECT(machine), sysmem, &s->tdec2, "TDEC-2",
                       Z63_TDEC2_BASE, Z63_TDEC2_SIZE, z63_tdec2_offsets,
                       ARRAY_SIZE(z63_tdec2_offsets));
    {
        /* One layout for both blocks; +0x58 is read as one 64-bit load
         * (0x40c74ef4), hence the 64-bit-capable ops. */
        uint16_t offs[Z63_YUVCOMP_NOFFS];
        size_t n = z63_yuvcomp_offsets(offs, ARRAY_SIZE(offs));

        z63_regwindow_init_ops(OBJECT(machine), sysmem, &s->yuvcomp,
                               "YUV-COMP", Z63_YUVCOMP_BASE,
                               Z63_YUVCOMP_SIZE, 0, offs, n,
                               &z63_regwindow_ops64);
        z63_regwindow_init_ops(OBJECT(machine), sysmem, &s->codecdec,
                               "CODEC-DECOMP", Z63_CODECDEC_BASE,
                               Z63_YUVCOMP_SIZE, 0, offs, n,
                               &z63_regwindow_ops64);
    }

    /* Display complex (see docs/display-registers.md). */
    z63_display_init(OBJECT(machine), sysmem, s->gic, &s->display);
    /* The consumer for that register file: see nikon-z63-scanout.h for the
     * register meanings and for what it refuses to do. */
    z63_scanout_init(&s->display, &s->scanout, s->scanout_dump,
                     s->scanout_live);
    s->scanout.zf_outputs = object_dynamic_cast(
        OBJECT(machine), MACHINE_TYPE_NAME("nikon-zf")) != NULL;
    zf_scanout_panels_init(&s->scanout, s->scanout_live);

    /* HDMI transmitter: DDC/E-DDC master, packet slots, PHY page.
     * See docs/hdmi-registers.md. */
    z63_hdmi_init(OBJECT(machine), sysmem, &s->hdmi);

    /* DWC3 USB in device mode plus its EXS and PHY glue.
     * See docs/usb-registers.md. */
    z63_usb_init(OBJECT(machine), sysmem, &s->usb);
    s->usb.zf_sd_present = s->zf_sd_drive && *s->zf_sd_drive;
    s->usb.zf_sd_present = s->zf_sd_drive && *s->zf_sd_drive;
    /* The other end of the camera's USB port: a host, with -machine
     * usbip= in front. See nikon-z63-usbhost.h and nikon-z63-dwc3.h. */
    s->usbhost = g_new0(Z63UsbHost, 1);
    z63_usbhost_init(s->usbhost, s->usb.ctl, s->usbip, &error_fatal);
    if (!s->usb_cable_set && s->usbip) {
        s->usb_cable = true;
    }
    if (s->usb_cable) {
        error_report("Z63_USB: a USB cable is in (GPIO port 14 bit 10)");
    }
    /* EXS_USB3_TYPE_C_ASSIST, the 0x17a40000 block's line. */
    s->usb.assist_irq = qdev_get_gpio_in(s->gic, Z63_GIC_SPI(109));
    s->usb.ctl->trace = getenv("Z63_DWC3_TRACE") != NULL;
    /* EXS_USB3_TYPE_C_CONTROLLER, the DWC3's event interrupt. */
    s->usb.ctl->irq = qdev_get_gpio_in(s->gic, Z63_GIC_SPI(108));
    if (s->cfexpress && !s->z9_capture) {
        error_report("cfexpress=on needs z9-capture=on: the slot's PCIe port "
                     "is ../z9-qemu's adapter (nikon-z63-z9lab.h)");
        exit(1);
    }
    s->usb.cfexpress = s->cfexpress;

    /* The auxiliary pair, register set taken from the driver rather than
     * from the accesses that happened to have been seen. See
     * nikon-z63-aux.h, including why that set is wider than this block. */
    z63_regwindow_init_strided(OBJECT(machine), sysmem, &s->aux, "AUX",
                               Z63_AUX_BASE, Z63_AUX_SIZE, Z63_AUX_STRIDE,
                               z63_aux_offsets, ARRAY_SIZE(z63_aux_offsets));
    /* Its first words in each bank are the EXT_INT edge detector, which
     * is behaviour; see nikon-z63-extint.h. Then the MCU on top of it and
     * of the serial bus. */
    z63_extint_init(OBJECT(machine), sysmem, s->gic, &s->extint);
    if (object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("nikon-zf"))) {
        s->zf_eye_ready = true;
        /* Explicit input fixture: historical own filtered count702,
         * not a current physical temperature or an invented identity. */
        if(!s->zf_temperature.input_set) { zf_temperature_input(&s->zf_temperature,702); }
        z63_extint_set(&s->extint,ZF_EYE_EXTINT,!zf_eye_output_near(&s->zf_eye_i2c,s->zf_eye_near));
    }
    if (s->zf_ccomp_feedback) {
        /* This diagnostic peer owns request and response. Keep transport
         * wires; remove native PG8/PD7 and the obsolete PF4 assignment. */
        g_auto(GStrv) wires = g_strsplit(s->mcu_wires ? s->mcu_wires : "", "+", -1);
        GString *kept = g_string_new(NULL);
        for (unsigned i = 0; wires[i]; i++) {
            if (!strcmp(wires[i], "F4=a3:12") || !strcmp(wires[i], "D7=a3:7") || !strcmp(wires[i], "G8=a3:12")) { continue; }
            if (kept->len) { g_string_append_c(kept, '+'); }
            g_string_append(kept, wires[i]);
        }
        g_free(s->mcu_wires);
        s->mcu_wires = g_string_free(kept, false);
        s->zf_ccomp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, zf_ccomp_tick, s);
        error_report("ZF_CCOMP: explicit virtual GPIO3.12/3.7 peer, 100us response; no physical EX pin mapping");
    }
    z63_touch_init(&s->touch, &s->extint);
    /* The peer on the other end of instances 0/1: either the synthetic one
     * that answers at the transport level (nikon-z63-mcu.h), or the MCU's
     * own image, run on a Cortex-M4 beside the A53s (nikon-z63-stm32.h).
     * mcu-image= names the latter and replaces the former. */
    if (s->mcu_battery_chardev && *s->mcu_battery_chardev && !s->mcu_image) {
        error_report("ZF: mcu-battery-chardev requires mcu-image");
        exit(1);
    }
    if (s->mcu_image) {
        s->real_mcu.zf_clocks = object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("nikon-zf"));
        Chardev *mcu_serial[6] = { NULL };
        if (s->mcu_battery_chardev && *s->mcu_battery_chardev) {
            mcu_serial[0] = qemu_chr_find(s->mcu_battery_chardev);
            if (!mcu_serial[0]) {
                error_report("ZF: battery chardev '%s' does not exist", s->mcu_battery_chardev);
                exit(1);
            }
        }
        z63_stm32_init(&s->real_mcu, OBJECT(machine), s->mcu_image, mcu_serial,
                       &error_fatal);
        s->real_mcu_built = true;
        if (object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("nikon-zf"))) {
            zf_rng_init(&s->real_mcu, OBJECT(machine));
        }
        /* Its clock: running and set, by default (nikon-z63-stm32.h). */
        if (!z63_stm32_rtc_option(&s->real_mcu, s->mcu_rtc, &error_fatal)) {
            return;
        }
        /* The real MCU replaces the synthetic peer, and mcu-peer=on is only
         * a default: until this line the GPIO block's peer overlay answered
         * the EXPEED's port reads from an object nothing had initialised,
         * which is why mcu-image= on its own left the handshake pins
         * reading 0 no matter what the MCU was doing. */
        s->mcu_peer = false;
        /* Its buttons, released, and any held from the command line
         * (nikon-z63-mcukeys.h). */
        z63_mcukeys_init(&s->real_mcu);
        if (s->mcu_keys && *s->mcu_keys) {
            z63_mcukeys_set(&s->real_mcu, s->mcu_keys, &error_fatal);
        }
        /* mcu-pins=G7=1,C5=0 drives the MCU's own inputs, standing in for
         * the wires the EXPEED holds. Without it the MCU's floating inputs
         * read 0 and the handshake never starts. nikon-z63-stm32.h. */
        if (s->mcu_pins) {
            z63_stm32_pins(&s->real_mcu, s->mcu_pins, &error_fatal);
        }
        /* The link, if the wiring is declared: nikon-z63-stm32.h's SPI on
         * one side, this board's serial instances on the other. */
        if (s->mcu_spi_peer) {
            s->mcu_spi_link = g_new0(struct Z63McuSpiLink, Z63_STM32_SPI_N);
            z63_mcu_spi_peer_setup(s, s->mcu_spi_peer);
        }
        /* The wires' other half: an MCU output pin changing level is an
         * edge into the EXPEED's EXT_INT block, which is what wakes that
         * side's tasks (z63_wires_pin_out). A wire that only moved the
         * readable bit would be a wire the firmware never notices. */
        s->real_mcu.pin_out = z63_wires_pin_out;
        s->real_mcu.pin_out_opaque = s;
        /* The top LCD: SPI2's data, and some of those pins
         * (nikon-z63-toplcd.h). */
        s->toplcd.zf_segment = s->real_mcu.zf_clocks;
        z63_toplcd_init(&s->toplcd, s->toplcd_live,
                        &s->real_mcu.gpio[Z63_TOPLCD_READY_PORT],
                        Z63_TOPLCD_READY_PIN);
        z63_stm32_spi_sink(&s->real_mcu, 1, z63_toplcd_byte, &s->toplcd);
    } else if (s->mcu_peer) {
        z63_mcu_init(&s->mcu, &s->sio, &s->extint);
    }
    if (s->mcu_image || s->mcu_peer) {
        qemu_input_handler_activate(
            qemu_input_handler_register(NULL, &z63_kbd_handler));
    }
    z63_audio_init(&s->audio, &s->sio, s->audio_codec);
    z63_sio_sinks(&s->sio, s->sio_sink);
    /* A synthetic lens on serial instance 3 and pins 41/58, powered and
     * detected through the real MCU when there is one (nikon-z63-lens.h). */
    if (s->lens_on) {
        if (s->sio.kick[Z63_LENS_SIO]) {
            error_report("lens=on: serial instance %u already has a peer",
                         Z63_LENS_SIO);
            exit(1);
        }
        z63_lens_init(&s->lens, &s->sio, &s->extint,
                      s->real_mcu_built ? &s->real_mcu : NULL);
        if (s->lens_replies) {
            z63_lens_set_replies(&s->lens, s->lens_replies, &error_fatal);
        }
    }

    /* The block named by the single runtime pointer at 0xa02e7b0c0. See
     * nikon-z63-vpipe-ctl.h. */
    z63_regwindow_init(OBJECT(machine), sysmem, &s->vpipe_ctl, "VPIPE-CTL",
                       Z63_VPCTL_BASE, Z63_VPCTL_SIZE, z63_vpipe_ctl_offsets,
                       ARRAY_SIZE(z63_vpipe_ctl_offsets));

    /* One five-instance family on a 0x40000 stride, bounded below by the
     * firmware's device table and above by the display controller. See
     * nikon-z63-blkfam.h. */
    z63_regwindow_init_strided(OBJECT(machine), sysmem, &s->blkfam, "BLK-FAM",
                               Z63_BLKFAM_BASE, Z63_BLKFAM_SIZE,
                               Z63_BLKFAM_STRIDE, z63_blkfam_offsets,
                               ARRAY_SIZE(z63_blkfam_offsets));

    /* The instances behind the runtime pointer array at 0xa02dc47xx. The
     * list is in nikon-z63-ispband.h and grows by one line per stop,
     * because the array is in BSS and cannot be enumerated statically. */
    for (size_t i = 0; i < ARRAY_SIZE(z63_ispband); i++) {
        z63_regwindow_init(OBJECT(machine), sysmem, &s->ispband[i],
                           z63_ispband[i].name, z63_ispband[i].base,
                           Z63_ISPBAND_SIZE, z63_ispband_offsets,
                           ARRAY_SIZE(z63_ispband_offsets));
        /* Dense: these two are register files the firmware fills with a
         * loop whose extent is an argument -- 0x40c23974 onward writes
         * base + 0x284 + 0x10k for k < W1. An enumerated set has a hole at
         * every k the immediates do not reach, and each hole is a false
         * stop. See the dense note in nikon-z63-regwindow.h. */
        s->ispband[i].dense = true;
    }

    /* The block behind the pointer slot at 0xa02dc3d88, same driver as the
     * AUX pair. See nikon-z63-blk1524.h. */
    z63_regwindow_init(OBJECT(machine), sysmem, &s->blk1524, "BLK-0x15240000",
                       Z63_BLK1524_BASE, Z63_BLK1524_SIZE,
                       z63_blk1524_offsets, ARRAY_SIZE(z63_blk1524_offsets));

    /* Devices 0x1cf and 0x1d0, a pair the firmware's device table names
     * together. See nikon-z63-dev1cf.h. */
    z63_regwindow_init_strided(OBJECT(machine), sysmem, &s->dev1cf,
                               "DEV-0x1CF", Z63_DEV1CF_BASE, Z63_DEV1CF_SIZE,
                               Z63_DEV1CF_STRIDE, z63_dev1cf_offsets,
                               ARRAY_SIZE(z63_dev1cf_offsets));

    /* The CODEC block, offsets read off the call sites of its generic
     * accessor pair. See nikon-z63-codec.h. */
    z63_regwindow_init(OBJECT(machine), sysmem, &s->codec, "CODEC",
                       Z63_CODEC_BASE, Z63_CODEC_SIZE, z63_codec_offsets,
                       ARRAY_SIZE(z63_codec_offsets));

    /* Devices 0x1cc, 0x1cd and 0x1ce, one table group with one register
     * set. See nikon-z63-dev1cc.h. */
    {
        /* The harvested set plus the eight channels' registers (the
         * comment above z63_dev1cc_chan_offsets). */
        size_t n0 = ARRAY_SIZE(z63_dev1cc_offsets);
        size_t n1 = ARRAY_SIZE(z63_dev1cc_chan_offsets);
        g_autofree uint16_t *offs = g_new(uint16_t, n0 + 8 * n1);

        memcpy(offs, z63_dev1cc_offsets, sizeof(z63_dev1cc_offsets));
        for (size_t ch = 0; ch < 8; ch++) {
            for (size_t k = 0; k < n1; k++) {
                offs[n0 + ch * n1 + k] = 0x1000 * (ch / 4) +
                                         0x400 * (ch % 4) +
                                         z63_dev1cc_chan_offsets[k];
            }
        }
        for (size_t i = 0; i < ARRAY_SIZE(z63_dev1cc); i++) {
            z63_regwindow_init(OBJECT(machine), sysmem, &s->dev1cc[i],
                               z63_dev1cc[i].name, z63_dev1cc[i].base,
                               Z63_DEV1CC_SIZE, offs, n0 + 8 * n1);
        }
    }

    /* Device 0x184's four channels. See nikon-z63-dev184.h. */
    {
        GArray *offs = g_array_new(false, false, sizeof(uint16_t));

        g_array_append_vals(offs, z63_dev184_offsets,
                            ARRAY_SIZE(z63_dev184_offsets));
        for (unsigned ch = 0; ch < Z63_DEV184_CHANNELS; ch++) {
            for (size_t r = 0; r < ARRAY_SIZE(z63_dev184_chan_ranges); r++) {
                for (unsigned o = z63_dev184_chan_ranges[r][0];
                     o <= z63_dev184_chan_ranges[r][1]; o += 4) {
                    uint16_t off = ch * Z63_DEV184_CHAN_STRIDE + o;

                    g_array_append_val(offs, off);
                }
            }
        }
        z63_regwindow_init(OBJECT(machine), sysmem, &s->dev184,
                           "DEV-0x184", Z63_DEV184_BASE, Z63_DEV184_SIZE,
                           (const uint16_t *)offs->data, offs->len);
        g_array_free(offs, true);
    }

    /*
     * The histogram RAM of device 0x1f9 (the resizer): sub_40C25C7C puts
     * 0x15420000 at descriptor +8 and 0x15c00000 at +16 (0xa02dc4888), and
     * sub_40C2662C copies the bins out of +16 -- 256 words, or three runs
     * of 256 at +0x000/+0x400/+0x800, each masked to 24 bits -- after the
     * RSZ E unit's frame end (nikon-z63-z9lab.h, z63-lv-frame-done). The
     * firmware only reads it. A RAM the unit fills, so the whole 0xc00 is
     * storage (dense, see nikon-z63-regwindow.h). DECLARED: nothing
     * computes the bins, so they read 0, an empty histogram.
     */
    z63_regwindow_init(OBJECT(machine), sysmem, &s->rszhist, "RSZ-HIST",
                       0x15c00000ULL, 0xc00, NULL, 0);
    s->rszhist.dense = true;

    /* ../z9-qemu's capture models, asked before the windows above where
     * they cover the same addresses. Opt-in; see nikon-z63-z9lab.h. */
    if (s->zf_statistics && (!s->zf_multi_io_counter || !s->zf_isp1a3_config ||
                             !s->z9_capture_raw || !*s->z9_capture_raw)) {
        error_report("zf-statistics requires shared counter, ISP1A3 and z9-capture-raw");
        exit(1);
    }
    if (s->zf_isp_small_config && !s->zf_multi_io_counter) {
        error_report("zf-isp-small-config requires the shared zf-multi-io-counter object");
        exit(1);
    }
    if (s->zf_isp192_config && !s->zf_multi_io_counter) {
        error_report("zf-isp192-config requires the shared zf-multi-io-counter object");
        exit(1);
    }
    if (s->zf_stream154c_empty && !s->zf_multi_io_counter) {
        error_report("zf-stream154c-empty requires the shared zf-multi-io-counter object");
        exit(1);
    }
    if (s->zf_sensorif_source_stop && !s->zf_sensorif_source_period_ns) {
        error_report("zf-sensorif-source-stop requires an explicit virtual SIF period");
        exit(1);
    }
    if (s->zf_isp199_config && !s->zf_multi_io_counter) {
        error_report("zf-isp199-config requires the shared zf-multi-io-counter object");
        exit(1);
    }
    if (s->zf_isp1a3_config && !s->zf_multi_io_counter) {
        error_report("zf-isp1a3-config requires the shared zf-multi-io-counter object");
        exit(1);
    }
    if (s->zf_sensorif_source_period_ns &&
        (!s->zf_multi_io_counter || !s->zf_sensorif_primary || !s->zf_sensorif_aux)) {
        error_report("zf-sensorif-source-period-ns requires shared MIO, primary and AUX");
        exit(1);
    }
    if (s->zf_sensorif_primary && !s->zf_multi_io_counter) {
        error_report("zf-sensorif-primary requires the shared zf-multi-io-counter object");
        exit(1);
    }
    if (s->zf_sensorif_group && !s->zf_multi_io_counter) {
        error_report("zf-sensorif-group requires the shared zf-multi-io-counter object");
        exit(1);
    }
    if (s->zf_sensorif_output_config && !s->zf_multi_io_counter) {
        error_report("zf-sensorif-output-config requires the shared zf-multi-io-counter object");
        exit(1);
    }
    if (s->zf_sensorif_aux && !s->zf_multi_io_counter) {
        error_report("zf-sensorif-aux requires the shared zf-multi-io-counter object");
        exit(1);
    }
    if (s->zf_multi_io_counter) {
        if (s->z9_capture) {
            error_report("zf-multi-io-counter is isolated; cannot combine with z9-capture");
            exit(1);
        }
        if (!zf_mio_only_init(OBJECT(machine), s->gic, Z63_DRAM_SIZE,
                              &s->dram, &s->buffer_ext, s->zf_sensorif_aux,
                              s->zf_sensorif_output_config, s->zf_sensorif_group,
                              s->zf_sensorif_primary, s->zf_sensorif_source_period_ns,
                              s->zf_isp1a3_config, s->zf_isp199_config,
                              s->zf_sensorif_source_stop, s->zf_stream154c_empty,
                              s->zf_isp192_config, s->zf_isp_small_config,
                              s->zf_statistics, s->z9_capture_raw, s->zf_still, s->zf_video_retained_start, s->zf_video_primary_source, s->zf_video_codec, s->zf_video_host_codec, s->zf_video_primary_exclusive,
                              &error_fatal)) {
            return;
        }
    }
    if (s->z9_capture &&
        !z63_z9lab_init(OBJECT(machine), s->gic, Z63_DRAM_SIZE, &s->dram,
                        &s->buffer_ext, s->cfexpress,
                        s->z9_capture_props, s->z9_capture_raw,
                        &error_fatal)) {
        return;
    }

    /* Register windows in 0x0e000000..0x0e210000 (see z63_win_read). The
     * gaps between them are deliberately left unmapped so an address in a
     * gap stops as unmapped rather than being absorbed into a bank. */
    {
        Z63Win *w = s->emio;
        unsigned i;

        w[0].name = "EMIO-E0"; w[0].base = Z63_EMIO_E0_BASE;
        w[0].size = Z63_EMIO_E0_SIZE; w[0].words = s->emio_e0_words;
        w[1].name = "EMIO-E1"; w[1].base = Z63_EMIO_E1_BASE;
        w[1].size = Z63_EMIO_E1_SIZE; w[1].words = s->emio_e1_words;
        w[2].name = "EMIO-E2"; w[2].base = Z63_EMIO_E2_BASE;
        w[2].size = Z63_EMIO_E2_SIZE; w[2].words = s->emio_e2_words;
        for (i = 0; i < 3; i++) {
            if (i == 1) {
                /* E1 is the HDMAC now: nikon-z63-hdmac.h, same storage. */
                continue;
            }
            memory_region_init_io(&w[i].region, OBJECT(machine), &z63_win_ops,
                                  &w[i], w[i].name, w[i].size);
            memory_region_add_subregion(sysmem, w[i].base, &w[i].region);
        }
    }
    z63_hdmac_init(OBJECT(machine), sysmem, s->gic, &s->hdmac,
                   s->emio_e1_words, &s->noc);
    z63_imu_init(&s->imu);
    z63_i2c_init(OBJECT(machine), sysmem, s->gic, &s->i2c, &z63_bus_endpoint,
                 s);
    s->sio.hdmac = &s->hdmac;

    /*
     * The 0x0c850000 and 0x16807000 banks are the two XDMAC controllers.
     * They used to be plain storage called PAIR-A and PAIR-B; the firmware's
     * own descriptors at 0xa0280d388 name their channels, register bases and
     * interrupts, and nikon-z63-xdmac.h has the evidence and the limits.
     */
    z63_xdmac_init(OBJECT(machine), sysmem, s->gic, &s->xdmac[0],
                   "XDMAC-PERI", Z63_PAIR_A_BASE, Z63_PAIR_A_SIZE, 0x10,
                   Z63_GIC_SPI(Z63_SPI_PERI_XDMAC_0), s->pair_a_words,
                   &s->noc, 1);
    z63_xdmac_init(OBJECT(machine), sysmem, s->gic, &s->xdmac[1],
                   "XDMAC-EXS", Z63_PAIR_B_BASE, Z63_PAIR_B_SIZE, 0x1010,
                   Z63_GIC_SPI(Z63_SPI_EXS_XDMAC_0), s->pair_b_words,
                   &s->noc, 2);
    /* rom-page-stub=on puts the keep overlay (writes through to WorkSRAM)
     * over the service page, which otherwise fails the DMA model's backed
     * check and stops sub_40D710DC's documented service-page copy. See the
     * DECLARED CHOICE by Z63_XDMAC_KEEP_BASE in nikon-z63-xdmac.h. */
    s->xdmac[0].rompage_keep = s->rom_page_stub;
    s->xdmac[1].rompage_keep = s->rom_page_stub;

    /* Video pipeline register array (see z63_vpipe_read). */
    memory_region_init_io(&s->vpipe_region, OBJECT(machine), &z63_vpipe_ops, s,
                          "z63-video-pipeline", Z63_VPIPE_SIZE);
    memory_region_add_subregion(sysmem, Z63_VPIPE_BASE, &s->vpipe_region);
    /* Its NoC windows, and the graphics engine that reaches DRAM through
     * them; nikon-z63-noc.h has why the windows are provisioned here. */
    s->noc.words = s->vpipe;
    s->noc.base = Z63_VPIPE_BASE;
    s->noc.size = Z63_VPIPE_SIZE;
    if (s->noc_windows) {
        z63_noc_provision(&s->noc);
        if ((s->zf_sd_drive && *s->zf_sd_drive) ||
            (s->zf_microsd_drive && *s->zf_microsd_drive)) {
            zf_sd_provision_noc(&s->noc);
        }
        error_report("Z63: NoC bus windows provisioned (../z9-qemu's "
                     "hypothesis: 128 MiB, 512 MiB for groups 1/2/15, 1 GiB "
                     "for 16) -- a declared choice standing in for the "
                     "loader's unmeasured setup; -machine noc-windows=off "
                     "disables it");
    }
    z63_gfx_init(OBJECT(machine), sysmem, &s->gfx, &s->noc);
    s->gfx.skip_unmodelled = s->gfx_skip_unmodelled;

    /* Five sibling blocks, one subregion per instance (see z63_sib_read).
     * The region spans the whole family so an access between instances
     * reaches the model and stops with a PC, rather than landing in the
     * unassigned pool where the address would still be reported but the
     * block would not. */
    memory_region_init_io(&s->sib_region, OBJECT(machine), &z63_sib_ops, s,
                          "z63-sibling-blocks",
                          (uint64_t)Z63_BLK_INST * Z63_BLK_STRIDE);
    memory_region_add_subregion(sysmem, Z63_BLK_BASE, &s->sib_region);

    /* Driver upload window (see z63_upk_read). */
    memory_region_init_io(&s->upk_region, OBJECT(machine), &z63_upk_ops, s,
                          "z63-upload-window", Z63_UPK_SIZE);
    memory_region_add_subregion(sysmem, Z63_UPK_BASE, &s->upk_region);

    if (object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("nikon-zf")) &&
        !s->loader_file) {
        error_report("ZF: loader=<Zf's own loader band> is required");
        exit(1);
    }

    /* Load the decrypted factory eg image. The file starts with the EL3
     * reset code; on the real camera it lands at 0x40160000 (the boot ROM
     * writes the 0x40000000 header itself). The copy is done by hand into
     * the RAM window -- see the note by z63_boot_header -- so that the
     * header and the per-CPU redistributor table the boot ROM patches in
     * afterwards are not undone by the ROM loader's reset. */
    if (machine->firmware) {
        gchar *buf = NULL;
        gsize len = 0;
        GError *err = NULL;
        uint8_t *ram = memory_region_get_ram_ptr(&s->rom);

        if (!g_file_get_contents(machine->firmware, &buf, &len, &err)) {
            error_report("Z63: cannot read RTOS image '%s': %s",
                         machine->firmware, err->message);
            exit(1);
        }
        if (len > Z63_ROM_SIZE - (Z63_IMAGE_LOAD_BASE - Z63_IMAGE_WINDOW_BASE)) {
            error_report("Z63: image '%s' (%zu bytes) does not fit the window",
                         machine->firmware, (size_t)len);
            exit(1);
        }
        error_report("Z63: virtual SoC identity at 0x%08" PRIx64 ": "
                     "revision=%u chip-id=%u (declared board choice, "
                     "not measured -- override with -machine soc-revision=N)",
                     (uint64_t)Z63_TOP_BASE, s->soc_revision, s->soc_chip_id);

        memcpy(ram + (Z63_IMAGE_LOAD_BASE - Z63_IMAGE_WINDOW_BASE), buf, len);
        error_report("Z63: loaded %zu bytes at 0x%08" PRIx64
                     ", entry rvbar=0x%08" PRIx64,
                     (size_t)len, (uint64_t)Z63_IMAGE_LOAD_BASE,
                     (uint64_t)Z63_IMAGE_ENTRY);

        /*
         * The loader band, 0x40000000-0x40160000.
         *
         * With -machine loader=<file> this is the body's own content and the
         * boot header comes with it. Without it the band is zeros and only
         * the synthesised 0x40-byte header stands in -- which is enough to
         * boot the image but not enough to call into the band, because the
         * image's boot-ROM service page does exactly that.
         */
        if (s->loader_file) {
            gchar *lbuf = NULL;
            gsize llen = 0;
            GError *lerr = NULL;
            const size_t want = Z63_IMAGE_LOAD_BASE - Z63_IMAGE_WINDOW_BASE;

            if (!g_file_get_contents(s->loader_file, &lbuf, &llen, &lerr)) {
                error_report("Z63: cannot read loader '%s': %s",
                             s->loader_file, lerr->message);
                exit(1);
            }
            if (llen != want && llen < want) {
                error_report("Z63: loader '%s' is %zu bytes; needs either "
                             "exactly 0x%zx or a dump of the window from "
                             "0x40000000 that is at least that long",
                             s->loader_file, (size_t)llen, want);
                exit(1);
            }
            if (memcmp(lbuf, z63_boot_header, sizeof(z63_boot_header)) != 0) {
                error_report("Z63: loader '%s' disagrees with the boot header "
                             "this board synthesises; the file is from a "
                             "different build or is not a dump of "
                             "0x40000000. Refusing to guess which is right.",
                             s->loader_file);
                exit(1);
            }
            memcpy(ram, lbuf, want);
            g_free(lbuf);
            error_report("Z63: loaded the loader band 0x%08" PRIx64
                         "-0x%08" PRIx64 " from '%s' (header matches)",
                         (uint64_t)Z63_IMAGE_WINDOW_BASE,
                         (uint64_t)Z63_IMAGE_LOAD_BASE, s->loader_file);
        } else {
            /* Boot ROM patches, applied after the copy (see above). */
            memcpy(ram, z63_boot_header, sizeof(z63_boot_header));
            error_report("Z63: no -machine loader=<file>: 0x%08" PRIx64
                         "-0x%08" PRIx64 " reads as zeros and only the "
                         "0x%zx-byte boot header stands in",
                         (uint64_t)Z63_IMAGE_WINDOW_BASE,
                         (uint64_t)Z63_IMAGE_LOAD_BASE,
                         sizeof(z63_boot_header));
        }

        /* The loader's "Program Load" step -- see z63_image_in_dram. */
        if (s->image_in_dram) {
            uint8_t *dram = memory_region_get_ram_ptr(&s->dram);

            bool zf = object_dynamic_cast(OBJECT(machine),
                                           MACHINE_TYPE_NAME("nikon-zf"));
            const Z63ImagePlacement *placements = zf ? zf_image_in_dram : z63_image_in_dram;
            size_t count = zf ? ARRAY_SIZE(zf_image_in_dram) : ARRAY_SIZE(z63_image_in_dram);
            for (size_t i = 0; i < count; i++) {
                const Z63ImagePlacement *w = &placements[i];
                uint64_t img_off = w->va - Z63_IMAGE_LOAD_BASE;
                uint64_t n = w->len;

                if (w->va < Z63_IMAGE_LOAD_BASE || img_off >= len) {
                    error_report("Z63: image window 0x%08" PRIx64 " is not "
                                 "inside this image -- not placed",
                                 w->va);
                    continue;
                }
                if (img_off + n > len) {
                    n = len - img_off;
                }
                if (w->pa < Z63_DRAM_BASE ||
                    w->pa + n > Z63_DRAM_BASE + Z63_DRAM_SIZE) {
                    error_report("Z63: DRAM destination 0x%09" PRIx64
                                 " is outside the modelled window -- "
                                 "not placed", w->pa);
                    continue;
                }
                memcpy(dram + (w->pa - Z63_DRAM_BASE), buf + img_off, n);
                error_report("Z63: placed image VA 0x%08" PRIx64 "+0x%" PRIx64
                             " at PA 0x%09" PRIx64 " (the loader's job; the "
                             "firmware's initial page tables map it there)",
                             w->va, n, w->pa);
            }
        }
        g_free(buf);
        /* Written into the backing, not through the address space: the
         * window is a ROM device now, so an address-space write would be a
         * guest write to NOR and would stop the machine. It is also the
         * truthful shape -- on the body this table is part of the flashed
         * image or comes from RAM; nothing writes NOR at run time. */
        for (int i = 0; i < num_cpu; i++) {
            uint64_t typer_addr = Z63_GIC_REDIST_BASE + (uint64_t)i * 0x20000
                                  + 0x14;
            uint64_t typer_le = cpu_to_le64(typer_addr);

            uint8_t *slot = ram + (Z63_CPU_TABLE_ADDR - Z63_IMAGE_WINDOW_BASE) + i * 8;
            if (object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("nikon-zf"))) {
                if (memcmp(slot, &typer_le, sizeof(typer_le))) {
                    error_report("ZF: original CPU table disagrees with GIC mapping; refusing ROM patch");
                    exit(1);
                }
            } else {
                memcpy(slot, &typer_le, sizeof(typer_le));
            }
        }
    } else {
        error_report("Z63: -bios <eg2010_0200i1.bi> is required");
        exit(1);
    }
}

static void z63_machine_instance_init(Object *obj)
{
    NikonZ63State *s = NIKON_Z63(obj);

    s->soc_revision = Z63_SOC_REVISION_DEFAULT;
    s->image_in_dram = true;   /* what the loader does on the body */
    s->li_wait_ipc = true;     /* the RTOS boots first on the body */
    s->noc_windows = true;     /* the loader's windows; nikon-z63-noc.h */
    s->mcu_peer = true;        /* nikon-z63-mcu.h */
    /* The handshake wiring the two firmwares' own tables name (see
     * Z63McuWire, and analysis/emulation/00-what-must-be-modeled.md 13):
     * MCU PB12 (pin id 28) -> EXPEED port 3 bit 2 (TX_READY), PE3 (67) ->
     * port 3 bit 3 (RX_READY), PD12 (60) -> port 4 bit 3 (ACK), and
     * EXPEED pin 54 = port 4 bit 7 -> MCU PG6. The EXPEED raises pin 54 in
     * the MCU module's init and in its "port open" sequence, and the MCU
     * takes it as EXTI line 6 (SYSCFG_EXTICR2 sends line 6 to port G and the
     * live EXTI_IMR has bit 6 set), where the link module's callback posts
     * the "link established" flag bit that the transmit loop waits for --
     * without this wire that loop retries its 128-word frame on a timeout
     * for ever. Then CAPCOM's capture-sync pair (nikon-z63-mcu.h): EXPEED
     * pin 43 = port 3 bit 12, the request, -> MCU PF4, which the mechanism
     * manager arms as EXTI line 4, rising (its family-9 mode-command path,
     * sub_803B404 -> sub_803A614; pin descriptor at 0x2001A178, id 0x54);
     * and MCU PD7 (descriptor at 0x2001A174, id 0x37), which the same
     * module raises when its operation is done (sub_803A820), -> EXPEED
     * port 3 bit 7, pin 38, EXT_INT_7 rising, the completion. Only
     * meaningful with mcu-image=; the synthetic peer answers those ports
     * itself. Overridable with mcu-wires=. */
    s->mcu_wires = g_strdup("B12=a3:2+E3=a3:3+D12=a4:3+G6=a4:7"
                            "+F4=a3:12+D7=a3:7");
    s->mcu_spi_peer = g_strdup("1:0+4:1");    /* see Z63McuSpiLink */
    /* The battery's data line idles high (a pulled-up single-wire bus).
     * The MCU's fuel-gauge task (sub_80266A0, event 0x2000 -> sub_80258E8)
     * polls PD13 (pin descriptor at 0x2001A878) every ~10 ms and talks to
     * the battery only after it has read 1 several times in a row; reading
     * the floating 0 instead, it gives up after 300 polls, ~2.8 s, and only
     * then sends the power report (family 4 kind 3) that the EXPEED's start
     * sequence waits for before it asks for live view. That is long past the
     * 1 s CaptureSequence gives the front end after powering the sensor
     * (error 0x1EA7). Nothing answers on the line itself; what the battery
     * would say is a separate matter. Overridable with mcu-pins=. */
    s->mcu_pins = g_strdup("D13=1");
    s->mcu_rtc = g_strdup("host");
    s->power_off = g_strdup("shutdown");
    /* Write-only serial devices (nikon-z63-sio.h): 9 is the rear panel;
     * 8 is the image sensor's control port -- logical serial channel 6
     * (sub_4115B8FC: sub_40C0040C(6)), chip select on the GPIO behind
     * resource 69. Every register write goes through sub_4115BBB8 with no
     * receive buffer: command 0x81, a 16-bit register address, then data
     * bytes, one per 16-bit word. Live-view start (sub_41167A48) sends such
     * packets and waits on each transfer, so unclocked it stopped at its
     * first one. The port also has a read command, 0x82 (sub_4115BD14,
     * from sub_4116D150), used by the sensor module's adjustment messages
     * (sub_4115E420) and not on any path reached so far; a sink would answer
     * it with zeros, which is the declared limit of this choice. */
    s->sio_sink = g_strdup("9:8");
    s->toplcd.named_cap = Z63_TOPLCD_NAMED;
    s->storage_caps_stub = true; /* nikon-z63-storage.h, declared values */
    s->audio_codec = true;       /* nikon-z63-audio.h, declared register file */
    s->soc_chip_id = Z63_SOC_CHIP_ID_DEFAULT;
}

static void z63_prop_get_soc_revision(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    uint8_t value = s->soc_revision;

    visit_type_uint8(v, name, &value, errp);
}

static void z63_prop_set_soc_revision(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value > 15) {
        error_setg(errp, "soc-revision is bits 28..31 of the identity word, "
                         "so it must be 0..15");
        return;
    }
    s->soc_revision = value;
}

static void z63_prop_get_soc_chip_id(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    uint8_t value = s->soc_chip_id;

    visit_type_uint8(v, name, &value, errp);
}

static void z63_prop_set_soc_chip_id(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value > 31) {
        error_setg(errp, "soc-chip-id is bits 0..4 of the identity word, "
                         "so it must be 0..31");
        return;
    }
    s->soc_chip_id = value;
}

static char *z63_prop_get_li_image(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->li_image);
}

static void z63_prop_set_li_image(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->li_image);
    s->li_image = g_strdup(value);
}

static void z63_prop_get_gpio_input(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    uint64_t value = s->gpio_input_level;

    visit_type_uint64(v, name, &value, errp);
}

static void z63_prop_set_gpio_input(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (value > UINT32_MAX) {
        error_setg(errp, "gpio-input-level must fit in 32 bits");
        return;
    }
    s->gpio_input_level = value;
    s->gpio_input_level_set = true;
}

static char *z63_prop_get_nand(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->nand_file);
}

static void z63_prop_set_nand(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->nand_file);
    s->nand_file = g_strdup(value);
}

static char *z63_prop_get_snor_user(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->snor_user_file);
}

static void z63_prop_set_snor_user(Object *obj, const char *value,
                                   Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->snor_user_file);
    s->snor_user_file = g_strdup(value);
}

static bool z63_prop_get_noc_windows(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->noc_windows;
}

static void z63_prop_set_noc_windows(Object *obj, bool value, Error **errp)
{
    NIKON_Z63(obj)->noc_windows = value;
}

static char *z63_prop_get_scanout_dump(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->scanout_dump);
}

static void z63_prop_set_scanout_dump(Object *obj, const char *value,
                                      Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->scanout_dump);
    s->scanout_dump = g_strdup(value);
}

static char *z63_prop_get_scanout_live(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->scanout_live);
}

static void z63_prop_set_scanout_live(Object *obj, const char *value,
                                      Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->scanout_live);
    s->scanout_live = g_strdup(value);
}

static char *z63_prop_get_toplcd_live(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->toplcd_live);
}

static void z63_prop_set_toplcd_live(Object *obj, const char *value,
                                     Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->toplcd_live);
    s->toplcd_live = g_strdup(value);
}

static bool z63_prop_get_camctl(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->mcu.camctl_reply;
}

static void z63_prop_set_camctl(Object *obj, bool value, Error **errp)
{
    NIKON_Z63(obj)->mcu.camctl_reply = value;
}

static bool zf_prop_get_ramdump_zeros(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_ramdump_zeros;
}

static void zf_prop_set_ramdump_zeros(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-ramdump-zeros requires Zf before native MCU initialization");
        return;
    }
    s->zf_ramdump_zeros = value;
}

static bool zf_prop_get_gpio22_bit8_high(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_gpio22_bit8_high;
}

static void zf_prop_set_gpio22_bit8_high(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-gpio22-bit8-high requires Zf before native MCU initialization");
        return;
    }
    s->zf_gpio22_bit8_high = value;
    if (value) {
        error_report("ZF_GPIO_INPUT: group22 bit8 driven high; explicit one-pin input, not a card backend");
    }
}

static bool zf_prop_get_multi_io_counter(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_multi_io_counter;
}

static void zf_prop_set_multi_io_counter(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-multi-io-counter requires Zf before native MCU initialization");
        return;
    }
    s->zf_multi_io_counter = value;
}

static bool zf_prop_get_sensorif_aux(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_sensorif_aux;
}

static void zf_prop_set_sensorif_aux(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-sensorif-aux requires Zf before native MCU initialization");
        return;
    }
    s->zf_sensorif_aux = value;
}

static bool zf_prop_get_output_config(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_sensorif_output_config;
}

static void zf_prop_set_output_config(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-sensorif-output-config requires Zf before native MCU initialization");
        return;
    }
    s->zf_sensorif_output_config = value;
}

static bool zf_prop_get_sensorif_group(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_sensorif_group;
}

static void zf_prop_set_sensorif_group(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-sensorif-group requires Zf before native MCU initialization");
        return;
    }
    s->zf_sensorif_group = value;
}

static bool zf_prop_get_sensorif_primary(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_sensorif_primary;
}

static void zf_prop_set_sensorif_primary(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-sensorif-primary requires Zf before native MCU initialization");
        return;
    }
    s->zf_sensorif_primary = value;
}

static char *zf_prop_get_sensorif_source_period(Object *obj, Error **errp)
{
    return g_strdup_printf("%" PRIu64, NIKON_Z63(obj)->zf_sensorif_source_period_ns);
}

static void zf_prop_set_sensorif_source_period(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    uint64_t period;
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-sensorif-source-period-ns requires Zf before native MCU initialization");
        return;
    }
    if (qemu_strtou64(value, NULL, 10, &period) || period > 1000000000 ||
        (period && period < 1000000)) {
        error_setg(errp, "zf-sensorif-source-period-ns requires decimal0 or1000000..1000000000");
        return;
    }
    s->zf_sensorif_source_period_ns = period;
}

static char *zf_prop_get_toplcd_named_cap(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", NIKON_Z63(obj)->toplcd.named_cap);
}

static void zf_prop_set_toplcd_named_cap(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    unsigned cap;
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-toplcd-named-cap requires Zf before native MCU initialization");
        return;
    }
    if (qemu_strtoui(value, NULL, 10, &cap) || cap > 1000000 || cap == 0) {
        error_setg(errp, "zf-toplcd-named-cap requires decimal 1..1000000");
        return;
    }
    s->toplcd.named_cap = cap;
}

static bool zf_prop_get_isp1a3_config(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_isp1a3_config;
}

static void zf_prop_set_isp1a3_config(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-isp1a3-config requires Zf before native MCU initialization");
        return;
    }
    s->zf_isp1a3_config = value;
}

static bool zf_prop_get_isp199_config(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_isp199_config;
}

static void zf_prop_set_isp199_config(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-isp199-config requires Zf before native MCU initialization");
        return;
    }
    s->zf_isp199_config = value;
}

static bool zf_prop_get_sensorif_source_stop(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_sensorif_source_stop;
}

static void zf_prop_set_sensorif_source_stop(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-sensorif-source-stop requires Zf before native MCU initialization");
        return;
    }
    s->zf_sensorif_source_stop = value;
}

static bool zf_prop_get_stream154c_empty(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_stream154c_empty;
}

static void zf_prop_set_stream154c_empty(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-stream154c-empty requires Zf before native MCU initialization");
        return;
    }
    s->zf_stream154c_empty = value;
}

static bool zf_prop_get_isp192_config(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_isp192_config;
}

static void zf_prop_set_isp192_config(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-isp192-config requires Zf before native MCU initialization");
        return;
    }
    s->zf_isp192_config = value;
}

static bool zf_prop_get_isp_small_config(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_isp_small_config;
}

static void zf_prop_set_isp_small_config(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-isp-small-config requires Zf before native MCU initialization");
        return;
    }
    s->zf_isp_small_config = value;
}

static bool zf_prop_get_statistics(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_statistics;
}

static void zf_prop_set_statistics(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-statistics requires Zf before initialization");
        return;
    }
    s->zf_statistics = value;
}

static bool zf_prop_get_ccomp_feedback(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_ccomp_feedback;
}

static void zf_prop_set_ccomp_feedback(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-ccomp-feedback requires Zf before initialization");
        return;
    }
    s->zf_ccomp_feedback = value;
}

static char *zf_prop_get_ccomp_state(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    return g_strdup_printf("{\"enabled\":%s,\"request\":%s,\"level\":%s,"
        "\"requests\":%" PRIu64 ",\"responses\":%" PRIu64
        ",\"pending\":%u,\"mask\":%u,\"polarity\":%u,\"edge\":%u}",
        s->zf_ccomp_feedback ? "true" : "false", s->zf_ccomp_request ? "true" : "false",
        s->zf_ccomp_level ? "true" : "false", s->zf_ccomp_requests, s->zf_ccomp_responses,
        s->extint.pending[0] & 0x80, s->extint.mask[0] & 0x80,
        s->extint.polarity[0] & 0x80, s->extint.edge[0] & 0x80);
}

static char *zf_prop_get_sd_drive(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->zf_sd_drive ?: "");
}

static void zf_prop_set_sd_drive(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-sd-drive requires Zf before initialization");
        return;
    }
    g_free(s->zf_sd_drive);
    s->zf_sd_drive = g_strdup(value);
}

static bool zf_prop_get_video_retained_start(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_video_retained_start;
}

static void zf_prop_set_video_retained_start(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-video-retained-start requires Zf before initialization");
        return;
    }
    s->zf_video_retained_start = value;
}

static bool zf_prop_get_video_host_codec(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_video_host_codec;
}
static void zf_prop_set_video_host_codec(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-video-host-codec requires Zf before initialization");
        return;
    }
    s->zf_video_host_codec = value;
}

static bool zf_prop_get_video_codec(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_video_codec;
}
static void zf_prop_set_video_codec(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-video-codec requires Zf before initialization");
        return;
    }
    s->zf_video_codec = value;
}

static bool zf_prop_get_video_primary_exclusive(Object *obj, Error **errp)
{ return NIKON_Z63(obj)->zf_video_primary_exclusive; }
static void zf_prop_set_video_primary_exclusive(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s=NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-video-primary-exclusive requires Zf before initialization"); return;
    }
    s->zf_video_primary_exclusive=value;
}

static bool zf_prop_get_video_primary_source(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_video_primary_source;
}
static void zf_prop_set_video_primary_source(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-video-primary-source requires Zf before initialization");
        return;
    }
    s->zf_video_primary_source = value;
}

static bool zf_prop_get_still(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_still;
}

static void zf_prop_set_still(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-still requires Zf before initialization");
        return;
    }
    s->zf_still = value;
}

static char *zf_prop_get_microsd_drive(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->zf_microsd_drive ?: "");
}

static char *zf_prop_get_sd_capability(Object *obj, Error **errp)
{
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp, "zf-sd-capability requires Zf"); return NULL;
    }
    return g_strdup("{\"host\":\"SDH30\",\"base\":\"0x17820000\","
                    "\"intid\":132,\"uhs_max\":\"UHS-II\","
                    "\"compatible\":\"UHS-I-and-lower\"}");
}

static char *zf_prop_get_microsd_capability(Object *obj, Error **errp)
{
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp, "zf-microsd-capability requires Zf"); return NULL;
    }
    return g_strdup("{\"host\":\"SDK-PCI-mode3\",\"base\":\"0x600010000\","
                    "\"intid\":149,\"msi_intid\":150,"
                    "\"physical_slot_verified\":false,\"uhs2_implemented\":false}");
}

static void zf_prop_set_microsd_drive(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-microsd-drive requires Zf before initialization");
        return;
    }
    g_free(s->zf_microsd_drive);
    s->zf_microsd_drive = g_strdup(value);
}

static bool zf_prop_get_gpio22_bit8_low(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_gpio22_bit8_low;
}

static void zf_prop_set_gpio22_bit8_low(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "zf-gpio22-bit8-low requires Zf before initialization");
        return;
    }
    s->zf_gpio22_bit8_low = value;
}

static bool z63_prop_get_service_core_events(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->service_core_events;
}

static void z63_prop_set_service_core_events(Object *obj, bool value,
                                             Error **errp)
{
    NIKON_Z63(obj)->service_core_events = value;
}

static bool z63_prop_get_cfexpress(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->cfexpress;
}

static void z63_prop_set_cfexpress(Object *obj, bool value, Error **errp)
{
    NIKON_Z63(obj)->cfexpress = value;
}

/* The slot's detect line, which a run may change: taking the card out and
 * putting it back is the one thing about this slot the firmware watches
 * for by itself. */
static bool z63_prop_get_cfexpress_card(Object *obj, Error **errp)
{
    return !NIKON_Z63(obj)->usb.cfexpress_card_out;
}

static void z63_prop_set_cfexpress_card(Object *obj, bool value, Error **errp)
{
    NIKON_Z63(obj)->usb.cfexpress_card_out = !value;
}

static bool z63_prop_get_z9_capture(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->z9_capture;
}

static void z63_prop_set_z9_capture(Object *obj, bool value, Error **errp)
{
    NIKON_Z63(obj)->z9_capture = value;
}

static char *z63_prop_get_z9_capture_props(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->z9_capture_props ? s->z9_capture_props :
                    z63_z9lab_default_props);
}

static void z63_prop_set_z9_capture_props(Object *obj, const char *value,
                                          Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->z9_capture_props);
    s->z9_capture_props = g_strdup(value);
}

static char *z63_prop_get_z9_capture_raw(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->z9_capture_raw ?
                    NIKON_Z63(obj)->z9_capture_raw : "");
}

static void z63_prop_set_z9_capture_raw(Object *obj, const char *value,
                                        Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->z9_capture_raw);
    s->z9_capture_raw = g_strdup(value);
}

static bool z63_prop_get_li_restage(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->li_restage;
}

static void z63_prop_set_li_restage(Object *obj, bool value, Error **errp)
{
    if (value && !object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp, "li-restage is restricted to the Zf diagnostic board");
        return;
    }
    NIKON_Z63(obj)->li_restage = value;
}

static bool z63_prop_get_capsync(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->mcu.capsync;
}

static void z63_prop_set_capsync(Object *obj, bool value, Error **errp)
{
    NIKON_Z63(obj)->mcu.capsync = value;
}

static char *z63_prop_get_imu_mg(Object *obj, Error **errp)
{
    Z63Imu *m = &NIKON_Z63(obj)->imu;

    return g_strdup_printf("%d:%d:%d", m->mg[0], m->mg[1], m->mg[2]);
}

static void z63_prop_set_imu_mg(Object *obj, const char *value, Error **errp)
{
    z63_imu_set_mg(&NIKON_Z63(obj)->imu, value, errp);
}

static char *z63_prop_get_imu_dps(Object *obj, Error **errp)
{
    Z63Imu *m = &NIKON_Z63(obj)->imu;

    if (!m->mdps_set) {
        return g_strdup("sine");
    }
    return g_strdup_printf("%d:%d:%d", m->mdps[0], m->mdps[1], m->mdps[2]);
}

static void z63_prop_set_imu_dps(Object *obj, const char *value, Error **errp)
{
    z63_imu_set_dps(&NIKON_Z63(obj)->imu, value, errp);
}

static void z63_prop_get_i2c_ns(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint32_t value = NIKON_Z63(obj)->i2c.step_ns;

    if (!value) {
        value = Z63_I2C_STEP_NS;
    }
    visit_type_uint32(v, name, &value, errp);
}

static void z63_prop_set_i2c_ns(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value < 1000 || value > 1000000) {
        error_setg(errp, "i2c-byte-ns: %u is not in 1000..1000000", value);
        return;
    }
    NIKON_Z63(obj)->i2c.step_ns = value;
}

static void z63_prop_get_imu_ppm(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    int32_t value = NIKON_Z63(obj)->imu.clock_ppm;

    visit_type_int32(v, name, &value, errp);
}

static void z63_prop_set_imu_ppm(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    if (value < -100000 || value > 100000) {
        error_setg(errp, "imu-clock-ppm: %d is not in -100000..100000", value);
        return;
    }
    NIKON_Z63(obj)->imu.clock_ppm = value;
}

static char *z63_prop_get_mcu_keys(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->mcu_keys ? NIKON_Z63(obj)->mcu_keys : "");
}

static bool z63_prop_get_lens(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->lens_on;
}

static void z63_prop_set_lens(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    if (s->lens.sio) {
        error_setg(errp, "lens= is fixed once the machine is built");
        return;
    }
    s->lens_on = value;
}

static char *z63_prop_get_lens_replies(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->lens_replies ?: "");
}

static void z63_prop_set_lens_replies(Object *obj, const char *value,
                                      Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->lens_replies);
    s->lens_replies = g_strdup(value);
    if (s->lens.sio) {                  /* qom-set at run time */
        z63_lens_set_replies(&s->lens, value, errp);
    }
}

static bool z63_prop_get_usb_cable(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->usb_cable;
}

static void z63_prop_set_usb_cable(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    if (s->usbhost && value != s->usb_cable) {
        error_report("Z63_USB: USB cable %s", value ? "plugged in"
                                                     : "pulled out");
    }
    s->usb_cable = value;
    s->usb_cable_set = true;
}

static char *z63_prop_get_usbip(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->usbip);
}

static void z63_prop_set_usbip(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->usbip);
    s->usbip = g_strdup(value);
}

static char *z63_prop_get_power_off(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->power_off);
}

static void z63_prop_set_power_off(Object *obj, const char *value,
                                   Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    if (strcmp(value, "shutdown") && strcmp(value, "continue")) {
        error_setg(errp, "power-off: '%s' is not shutdown or continue",
                   value);
        return;
    }
    g_free(s->power_off);
    s->power_off = g_strdup(value);
}

static char *z63_prop_get_mcu_rtc(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->mcu_rtc);
}

static void z63_prop_set_mcu_rtc(Object *obj, const char *value,
                                 Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->mcu_rtc);
    s->mcu_rtc = g_strdup(value);
}

static char *z63_prop_get_touch(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->touch_points ? s->touch_points : "");
}

static void z63_prop_set_touch(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    if (!s->touch.timer) {
        error_setg(errp, "touch: settable only while the machine runs "
                   "(qom-set /machine touch x:y)");
        return;
    }
    if (!z63_touch_set(&s->touch, value, errp)) {
        return;
    }
    g_free(s->touch_points);
    s->touch_points = g_strdup(value);
}

static void z63_prop_set_mcu_keys(Object *obj, const char *value,
                                  Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    if (object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp, "Zf uses mcu-button/mcu-menu/mcu-navigation/mcu-mode-lever/mcu-dial; the Z63 mcu-keys map is unbound");
        return;
    }

    if (object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp, "Zf uses mcu-button/mcu-menu/mcu-navigation/mcu-mode-lever/mcu-dial; the Z63 mcu-keys map is unbound");
        return;
    }

    /* Before the machine is built this is the report sent after the boot
     * sequence; afterwards (qom-set) it is sent at once. With the real MCU
     * it is the buttons' levels, applied at once or at machine init. */
    if (s->real_mcu_built) {
        if (!z63_mcukeys_set(&s->real_mcu, value, errp)) {
            return;
        }
        z63_release_contacts(s);
    } else if (s->mcu_peer && s->mcu.queue) {
        if (!z63_mcu_keys(&s->mcu, value, errp)) {
            return;
        }
    } else {
        g_free(s->mcu.keys_at_boot);
        s->mcu.keys_at_boot = g_strdup(value);
    }
    g_free(s->mcu_keys);
    s->mcu_keys = g_strdup(value);
}

static char *z63_prop_get_sio_sink(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->sio_sink);
}

static void z63_prop_set_sio_sink(Object *obj, const char *value,
                                  Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->sio_sink);
    s->sio_sink = g_strdup(value);
}

static bool z63_prop_get_audio_codec(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->audio_codec;
}

static void z63_prop_set_audio_codec(Object *obj, bool value, Error **errp)
{
    NIKON_Z63(obj)->audio_codec = value;
}

static bool z63_prop_get_storage_caps_stub(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->storage_caps_stub;
}

static void z63_prop_set_storage_caps_stub(Object *obj, bool value,
                                           Error **errp)
{
    NIKON_Z63(obj)->storage_caps_stub = value;
}

static bool z63_prop_get_mcu_peer(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->mcu_peer;
}

static void z63_prop_set_mcu_peer(Object *obj, bool value, Error **errp)
{
    NIKON_Z63(obj)->mcu_peer = value;
}

static bool z63_prop_get_li_wait_ipc(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return s->li_wait_ipc;
}

static void z63_prop_set_li_wait_ipc(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    s->li_wait_ipc = value;
}

static bool z63_prop_get_gfx_skip(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->gfx_skip_unmodelled;
}

static void z63_prop_set_gfx_skip(Object *obj, bool value, Error **errp)
{
    NIKON_Z63(obj)->gfx_skip_unmodelled = value;
}

static bool z63_prop_get_image_in_dram(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return s->image_in_dram;
}

static void z63_prop_set_image_in_dram(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    s->image_in_dram = value;
}

static char *z63_prop_get_loader(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->loader_file);
}

static void z63_prop_set_loader(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->loader_file);
    s->loader_file = g_strdup(value);
}

static char *z63_prop_get_mcu_image(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->mcu_image);
}

static void z63_prop_set_mcu_image(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->mcu_image);
    s->mcu_image = g_strdup(value);
}

static char *z63_prop_get_mcu_spi_peer(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->mcu_spi_peer);
}

static void z63_prop_set_mcu_spi_peer(Object *obj, const char *value,
                                      Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->mcu_spi_peer);
    s->mcu_spi_peer = g_strdup(value);
}

static bool zf_prop_get_battery_standin(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->battery_standin;
}

static void zf_prop_set_battery_standin(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "battery-standin requires Zf before native MCU initialization");
        return;
    }
    s->battery_standin = value;
}

static char *zf_prop_get_battery_chardev(Object *obj, Error **errp)
{
    return g_strdup(NIKON_Z63(obj)->mcu_battery_chardev);
}

static void zf_prop_set_battery_chardev(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || s->real_mcu_built) {
        error_setg(errp, "mcu-battery-chardev requires Zf before machine initialization");
        return;
    }
    g_free(s->mcu_battery_chardev);
    s->mcu_battery_chardev = g_strdup(value);
}

static bool zf_prop_get_menu(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->real_mcu.zf_menu_pressed;
}

static void zf_prop_set_menu(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) ||
        !s->real_mcu_built) {
        error_setg(errp, "mcu-menu requires a running native Zf MCU");
        return;
    }
    if (s->real_mcu.gpio[5].driven & (1u << 10)) {
        error_setg(errp, "mcu-menu conflicts with an external PF10 drive");
        return;
    }
    s->real_mcu.zf_menu_pressed = value;
    error_report("ZF_MENU_CONTACT: PF1/PF10 %s", value ? "closed" : "open");
}

static const char *const zf_navigation_names[] = {
    "none", "up", "down", "left", "right"
};

static char *zf_prop_get_navigation(Object *obj, Error **errp)
{
    return g_strdup(zf_navigation_names[NIKON_Z63(obj)->real_mcu.zf_navigation]);
}

static void zf_prop_set_navigation(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    unsigned key;
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || !s->real_mcu_built) {
        error_setg(errp, "mcu-navigation requires a running native Zf MCU");
        return;
    }
    for (key = 0; key < ARRAY_SIZE(zf_navigation_names); key++) {
        if (!strcmp(value, zf_navigation_names[key])) { break; }
    }
    if (key == ARRAY_SIZE(zf_navigation_names)) {
        error_setg(errp, "mcu-navigation expects none/up/down/left/right");
        return;
    }
    if (key && (s->real_mcu.gpio[5].driven & (1u << (key + 6)))) {
        error_setg(errp, "mcu-navigation conflicts with an external column drive");
        return;
    }
    s->real_mcu.zf_navigation = key;
    error_report("ZF_NAVIGATION_CONTACT: %s", value);
}

static const char *const zf_mode_lever_names[] = {
    "idle", "pos1", "pos2"
};

static char *zf_prop_get_mode_lever(Object *obj, Error **errp)
{
    return g_strdup(zf_mode_lever_names[NIKON_Z63(obj)->real_mcu.zf_mode_lever]);
}

static void zf_prop_set_mode_lever(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    unsigned pos;
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || !s->real_mcu_built) {
        error_setg(errp, "mcu-mode-lever requires a running native Zf MCU");
        return;
    }
    for (pos = 0; pos < ARRAY_SIZE(zf_mode_lever_names); pos++) {
        if (!strcmp(value, zf_mode_lever_names[pos])) { break; }
    }
    if (pos == ARRAY_SIZE(zf_mode_lever_names)) {
        error_setg(errp, "mcu-mode-lever expects idle/pos1/pos2");
        return;
    }
    unsigned col = pos == 1 ? 7 : pos == 2 ? 11 : 0;
    if (pos && (s->real_mcu.gpio[5].driven & (1u << col))) {
        error_setg(errp, "mcu-mode-lever conflicts with an external column drive");
        return;
    }
    s->real_mcu.zf_mode_lever = pos;
    error_report("ZF_MODE_LEVER: %s", value);
}

static char *zf_prop_get_display3e_state(Object *obj, Error **errp)
{
    ZfDisplay3e *p = &NIKON_Z63(obj)->zf_display3e;
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp, "zf-i2c3e-state requires Zf"); return NULL;
    }
    GString *state = g_string_new("{\"registers\":[");
    bool first = true;
    for (unsigned reg = 0; reg < 4; reg++) {
        if (reg == 2) continue;
        g_string_append_printf(state, "%s{\"address\":%u,\"written\":%s,\"value\":%u}",
            first ? "" : ",", reg, p->written & (1u << reg) ? "true" : "false", p->value[reg]);
        first = false;
    }
    g_string_append_printf(state, "],\"starts\":%" PRIu64 ",\"writes\":%" PRIu64
        ",\"rejects\":%" PRIu64 "}", p->starts, p->writes, p->rejects);
    return g_string_free(state, false);
}

static char *zf_prop_get_temperature_raw(Object *obj, Error **errp)
{
    return g_strdup_printf("%d",NIKON_Z63(obj)->zf_temperature.input);
}
static void zf_prop_set_temperature_raw(Object *obj,const char *value,Error **errp)
{
    NikonZ63State *s=NIKON_Z63(obj);long raw;
    if(!object_dynamic_cast(obj,MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp,"i2c48-raw requires Zf");return;
    }
    if(qemu_strtol(value,NULL,10,&raw) || raw < -2048 || raw > 2047) {
        error_setg(errp,"i2c48-raw expects signed12-bit count -2048..2047");return;
    }
    zf_temperature_input(&s->zf_temperature,raw);
    error_report("ZF_TEMPERATURE_INPUT: raw=%ld",raw);
}
static char *zf_prop_get_temperature_state(Object *obj,Error **errp)
{
    NikonZ63State *s=NIKON_Z63(obj);ZfTemperature *p=&s->zf_temperature;
    if(!object_dynamic_cast(obj,MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp,"zf-i2c48-state requires Zf");return NULL;
    }
    return g_strdup_printf("{\"raw\":%d,\"held\":%d,\"configured\":%s,\"running\":%s,\"have_sample\":%s,\"config\":%u,\"starts\":%" PRIu64
        ",\"config_writes\":%" PRIu64 ",\"read_bytes\":%" PRIu64 ",\"errors\":%" PRIu64 "}",
        p->input,p->held,p->configured?"true":"false",zf_temperature_running(p)?"true":"false",
        p->have_sample?"true":"false",p->config,p->starts,p->config_writes,p->reads,p->errors);
}

static bool zf_prop_get_eye_near(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->zf_eye_near;
}

static void zf_prop_set_eye_near(Object *obj, bool near, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp,"eye-near requires the Zf board"); return;
    }
    if (s->zf_eye_near != near) { s->zf_eye_changes++; }
    s->zf_eye_near = near;
    if (s->zf_eye_ready) { z63_extint_set(&s->extint,ZF_EYE_EXTINT,!zf_eye_output_near(&s->zf_eye_i2c,near)); }
    error_report("ZF_EYE_INPUT: near=%u GPIO3.13=%u",near,!zf_eye_output_near(&s->zf_eye_i2c,near));
}

static char *zf_prop_get_eye_state(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp,"zf-eye-state requires Zf"); return NULL;
    }
    unsigned mask = 1u << 4; /* own EXT_INT13 = bank1/bit4 */
    return g_strdup_printf("{\"enabled\":%s,\"near\":%s,\"level\":%u,\"changes\":%" PRIu64
        ",\"line\":13,\"intid\":68,\"configured\":%s,\"masked\":%s,\"pending\":%s,\"polarity\":%u,\"measurement_enabled\":%s,\"mode_written\":%s,\"mode\":%u,\"i2c_starts\":%" PRIu64 ",\"i2c_writes\":%" PRIu64 ",\"resets\":%" PRIu64 ",\"unsupported_reads\":%" PRIu64 ",\"unsupported_writes\":%" PRIu64 "}",
        s->zf_eye_ready?"true":"false",s->zf_eye_near?"true":"false",!zf_eye_output_near(&s->zf_eye_i2c,s->zf_eye_near),s->zf_eye_changes,
        s->extint.configured[1]&mask?"true":"false",s->extint.mask[1]&mask?"true":"false",
        s->extint.pending[1]&mask?"true":"false",!!(s->extint.polarity[1]&mask),zf_eye_measurement(&s->zf_eye_i2c)?"true":"false",
        s->zf_eye_i2c.written[0x41]?"true":"false",s->zf_eye_i2c.registers[0x41],
        s->zf_eye_i2c.starts,s->zf_eye_i2c.writes,s->zf_eye_i2c.resets,
        s->zf_eye_i2c.unsupported_reads,s->zf_eye_i2c.unsupported_writes);
}

static char *zf_prop_get_panel_state(Object *obj, Error **errp)
{
    Z63Scanout *s = &NIKON_Z63(obj)->scanout;
    if (!s->zf_outputs || !s->panel_con[1]) {
        error_setg(errp, "zf-panel-state requires initialized Zf panels");
        return NULL;
    }
    return zf_panel_state(s);
}

static void zf_prop_set_panel_snapshot(Object *obj, const char *prefix, Error **errp)
{
    Z63Scanout *s = &NIKON_Z63(obj)->scanout;
    if (!s->zf_outputs || !s->panel_con[1] || !prefix || !*prefix) {
        error_setg(errp, "zf-panel-snapshot requires initialized Zf panels and a new output prefix");
        return;
    }
    zf_panel_snapshot(s, prefix, errp);
}

/* Own access-light handle139: GPIO_TOP group9/bit14, high=on.
 * Read the existing output latch, not DMA traffic or cached host state. */
static char *zf_prop_get_access_led_state(Object *obj, Error **errp)
{
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp, "zf-access-led-state requires Zf"); return NULL;
    }
    unsigned level = (NIKON_Z63(obj)->gpio_config[0][9] >> 14) & 1;
    return g_strdup_printf("{\"on\":%s,\"color\":\"green\","
                           "\"gpio\":139,\"group\":9,\"bit\":14,\"level\":%u}",
                           level ? "true" : "false", level);
}

/* Own USB-C charge indicator: read the EX MCU's own latches, not a host
 * policy.  PB4 is BatCgSvc's gated charge-enable output; PC9 its companion,
 * high while charge control is disabled. */
static char *zf_prop_get_charge_lamp_state(Object *obj, Error **errp)
{
    unsigned pb4, pc9;
    const char *state, *color, *pair;

    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp, "zf-charge-lamp-state requires Zf"); return NULL;
    }
    if (!NIKON_Z63(obj)->real_mcu_built) {
        error_setg(errp, "zf-charge-lamp-state requires mcu-image="); return NULL;
    }
    pb4 = (NIKON_Z63(obj)->real_mcu.gpio[1].odr >> 4) & 1;
    pc9 = (NIKON_Z63(obj)->real_mcu.gpio[2].odr >> 9) & 1;
    if (pb4) {
        /* Charge output asserted, which is what the colour convention maps to
         * orange.  PC9 high at the same time is not a combination the
         * decompiled enable/disable paths produce, so say so. */
        state = "charging"; color = "orange";
        pair = pc9 ? "not-seen-in-firmware" : "enable";
    } else if (pc9) {
        state = "idle"; color = "off"; pair = "disabled";
    } else {
        state = "off"; color = "off"; pair = "neither";
    }
    return g_strdup_printf("{\"state\":\"%s\",\"color\":\"%s\","
                           "\"pb4\":%u,\"pc9\":%u,\"pair\":\"%s\","
                           "\"complete\":\"unavailable\","
                           "\"confirmed\":false,"
                           "\"mapping\":\"charging=orange (operator convention); "
                           "green/complete needs a charge-status source the model "
                           "lacks\"}",
                           state, color, pb4, pc9, pair);
}

/* Experimental Zf lens profile.  The transport peer is reused from the
 * reference lens implementation, but Zf's native lens wiring/protocol is not
 * hardware-qualified yet.  Keep the declared optical values explicit. */
static char *zf_prop_get_lens_state(Object *obj, Error **errp)
{
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf"))) {
        error_setg(errp, "zf-lens-state requires Zf"); return NULL;
    }
    if (!NIKON_Z63(obj)->lens_on) {
        return g_strdup("{\"enabled\":false,\"profile\":\"50mm f/1\","
                       "\"focal_length_mm\":50,\"max_aperture\":1,"
                       "\"status\":\"off\",\"experimental\":true}");
    }
    return g_strdup("{\"enabled\":true,\"profile\":\"50mm f/1\","
                    "\"focal_length_mm\":50,\"max_aperture\":1,"
                    "\"status\":\"synthetic-peer\",\"experimental\":true,"
                    "\"native_zf_transport\":\"unverified\"}");
}

static char *zf_prop_get_toplcd_state(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || !s->real_mcu_built) {
        error_setg(errp, "zf-toplcd-state requires native Zf MCU");
        return NULL;
    }
    ZfSegmentPanel *p = &s->toplcd.segment;
    char text[5]; uint8_t digits[2]; bool flags[2];
    zf_segment_text(p,text); zf_segment_masks(p,digits,flags);
    return g_strdup_printf("{\"ram\":[%u,%u],\"digits\":[%u,%u],\"decimal\":%s,\"auxiliary_bit\":%s,"
        "\"text\":\"%s\",\"disp\":%s,\"cs_high\":%s,\"bytes\":%" PRIu64
        ",\"frames\":%" PRIu64 ",\"writes\":%" PRIu64 ",\"dropped\":%" PRIu64
        ",\"errors\":%" PRIu64 ",\"truncated\":%" PRIu64 ",\"registers\":[%u,%u,%u,%u],\"power_command\":%u}",
        p->ram[0],p->ram[1],digits[0],digits[1],flags[0]?"true":"false",flags[1]?"true":"false",text,
        s->toplcd.disp?"true":"false",p->cs_high?"true":"false",p->bytes,p->frames,p->writes,
        p->dropped,p->errors,p->truncated,p->registers[0],p->registers[1],p->registers[2],p->registers[3],p->power_command);
}

static char *zf_prop_get_mode_adc(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", NIKON_Z63(obj)->real_mcu.adc.input[0][1]);
}

static void zf_prop_set_mode_adc(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    unsigned level;
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || !s->real_mcu_built) {
        error_setg(errp, "mcu-mode-adc requires a running native Zf MCU");
        return;
    }
    if (qemu_strtoui(value, NULL, 0, &level) || level > 4095) {
        error_setg(errp, "mcu-mode-adc expects ADC1 channel1 sample 0..4095");
        return;
    }
    s->real_mcu.adc.input[0][1] = level;
    error_report("ZF_MODE_ADC: %u", level);
}

static char *zf_prop_get_rotary(Object *obj, Error **errp)
{
    Z63Stm32 *m = &NIKON_Z63(obj)->real_mcu;
    return g_strdup_printf("enabled=%u;row0=%u;row2=%u;row5=%u",
        m->zf_rotary_enabled, m->zf_rotary_mask[0], m->zf_rotary_mask[1], m->zf_rotary_mask[2]);
}

static void zf_prop_set_rotary(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    const unsigned rows[] = { 0, 2, 5 };
    unsigned slot, mask;
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || !s->real_mcu_built) {
        error_setg(errp, "mcu-rotary requires a running native Zf MCU");
        return;
    }
    if (strncmp(value, "row", 3) || strlen(value) < 6 || value[4] != '=') {
        error_setg(errp, "mcu-rotary expects row0/row2/row5=MASK or =open");
        return;
    }
    for (slot = 0; slot < ARRAY_SIZE(rows); slot++) {
        if (value[3] == '0' + rows[slot]) { break; }
    }
    if (slot == ARRAY_SIZE(rows)) {
        error_setg(errp, "mcu-rotary supports only own aggregate rows0/2/5");
        return;
    }
    if (!strcmp(value + 5, "open")) {
        s->real_mcu.zf_rotary_enabled &= ~(1u << slot);
        return;
    }
    if (qemu_strtoui(value + 5, NULL, 0, &mask) || mask > (slot == 1 ? 127 : 63)) {
        error_setg(errp, "mcu-rotary expects a6-bit mask (row2:7-bit)");
        return;
    }
    unsigned pins = slot == 1 ? 0x3f80 : 0x1f80;
    if (s->real_mcu.gpio[5].driven & pins) {
        error_setg(errp, "mcu-rotary conflicts with an external column drive");
        return;
    }
    s->real_mcu.zf_rotary_mask[slot] = mask;
    s->real_mcu.zf_rotary_enabled |= 1u << slot;
    error_report("ZF_ROTARY_CONTACTS: row%u=%u", rows[slot], mask);
}

static char *zf_prop_get_button(Object *obj, Error **errp)
{
    return g_strdup(zf_buttons[NIKON_Z63(obj)->real_mcu.zf_button].name);
}

static void zf_prop_set_button(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    unsigned key;
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || !s->real_mcu_built) {
        error_setg(errp, "mcu-button requires a running native Zf MCU");
        return;
    }
    for (key = 0; key < ARRAY_SIZE(zf_buttons); key++) {
        if (!strcmp(value, zf_buttons[key].name)) { break; }
    }
    if (key == ARRAY_SIZE(zf_buttons)) {
        error_setg(errp, "unknown Zf mcu-button name: %s", value);
        return;
    }
    const ZfButton *b = &zf_buttons[key];
    /* Direct contacts can close against the inherited released-high input
     * defaults (notably PG2). Matrix columns must have no whole-pin drive. */
    if (key && (s->real_mcu.gpio[b->port].driven & (1u << b->pin)) &&
        (b->row != 16 || !(s->real_mcu.gpio[b->port].level & (1u << b->pin)))) {
        error_setg(errp, "mcu-button conflicts with an external pin drive");
        return;
    }
    s->real_mcu.zf_button = key;
    error_report("ZF_BUTTON_CONTACT: %s", value);
}

static void zf_prop_set_dial(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    long n;
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || !s->real_mcu_built) {
        error_setg(errp, "mcu-dial requires a running native Zf MCU");
        return;
    }
    /* Reuse the donor's virtual-time Gray cycle and bounded detent parser.
     * Call just this token path: do not apply the Z63 button/ADC tables. */
    if (!g_str_has_prefix(value, "dial-main=") &&
        !g_str_has_prefix(value, "dial-sub=")) {
        error_setg(errp, "mcu-dial expects dial-main=+N/-N or dial-sub=+N/-N");
        return;
    }
    unsigned i = g_str_has_prefix(value, "dial-main=") ? 0 : 1;
    if (qemu_strtol(strchr(value, '=') + 1, NULL, 10, &n) || !n ||
        n < -127 || n > 127 || abs(z63_mcuin.dial[i].pending) > 127 - labs(n)) {
        error_setg(errp, "mcu-dial allows at most 127 queued detents per encoder");
        return;
    }
    z63_mcuin_token(&s->real_mcu, value, true, errp);
}

static bool zf_prop_get_fn(Object *obj, Error **errp)
{
    return NIKON_Z63(obj)->real_mcu.zf_fn_pressed;
}

static void zf_prop_set_fn(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    if (!object_dynamic_cast(obj, MACHINE_TYPE_NAME("nikon-zf")) || !s->real_mcu_built) {
        error_setg(errp, "mcu-fn requires a running native Zf MCU");
        return;
    }
    s->real_mcu.zf_fn_pressed = value;
    error_report("ZF_FN_MODIFIER: %s", value ? "pressed" : "released");
}

static char *z63_prop_get_mcu_pins(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->mcu_pins);
}

static void z63_prop_set_mcu_pins(Object *obj, const char *value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->mcu_pins);
    s->mcu_pins = g_strdup(value);
    /* qom-set at run time drives the pins at once, as the standalone MCU
     * machine's pins= does -- e.g. G0=1 / G0=0, the lens-detect line
     * (lenshw_detect_read 0x08035600: PG0 low is a lens on the mount),
     * to take a lens off and put it back. */
    if (s->real_mcu_built) {
        z63_stm32_pins(&s->real_mcu, value, errp);
        /* GPIO reads already expose PG1/PG13 on MAIN port3 bits4/5.
         * Notify their existing EXT_INT lines just as mcu-keys does. */
        z63_release_contacts(s);
        z63_lens_power(&s->lens);       /* G0: the lens on or off the mount */
    }
}

static char *z63_prop_get_mcu_wires(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return g_strdup(s->mcu_wires);
}

static void z63_prop_set_mcu_wires(Object *obj, const char *value,
                                   Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    g_free(s->mcu_wires);
    s->mcu_wires = g_strdup(value);
}

static bool z63_prop_get_rom_page_stub(Object *obj, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    return s->rom_page_stub;
}

static void z63_prop_set_rom_page_stub(Object *obj, bool value, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);

    s->rom_page_stub = value;
}

static void z63_prop_get_cpu3_entry(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    uint64_t value = s->cpu3_entry;

    visit_type_uint64(v, name, &value, errp);
}

static void z63_prop_set_cpu3_entry(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    NikonZ63State *s = NIKON_Z63(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    s->cpu3_entry = value;
}

static void z63_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a53"),
        NULL,
    };

    mc->desc = "Nikon Z 6III RTOS research machine (TCG, GICv3, experimental)";
    mc->init = z63_init;
    mc->reset = z63_machine_reset;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 8;
    mc->default_cpus = Z63_NUM_CPU; /* EXPEED 7: 4-core cluster (DTB cpu@3) */
    mc->ignore_memory_transaction_failures = false;
    mc->no_cdrom = 1;
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->default_ram_size = Z63_DRAM_SIZE;
    /* 4 KiB pages, unless mcu-image= puts the body MCU beside the A53s: its
     * Cortex-M4 asks for 1 KiB (target/arm/cpu.c, the MPU branch) and the
     * page size cannot be lowered once cpu_exec_init_all() has fixed it --
     * which happens while the machine is created, before any -machine
     * property is applied. So the choice has to be made here, and 1 KiB is
     * the size an arm board that states nothing gets anyway. */
    mc->minimum_page_bits = 10;

    object_class_property_add(oc, "soc-revision", "uint8",
                              z63_prop_get_soc_revision,
                              z63_prop_set_soc_revision, NULL, NULL);
    object_class_property_set_description(oc, "soc-revision",
        "Virtual silicon revision nibble (bits 28..31) reported by the TOP "
        "identity word at 0x0d150000. Not measured from hardware.");
    object_class_property_add(oc, "soc-chip-id", "uint8",
                              z63_prop_get_soc_chip_id,
                              z63_prop_set_soc_chip_id, NULL, NULL);
    object_class_property_set_description(oc, "soc-chip-id",
        "Virtual chip id (bits 0..4) reported by the TOP identity word at "
        "0x0d150000. Not measured from hardware.");
    object_class_property_add_str(oc, "li-image",
                                  z63_prop_get_li_image,
                                  z63_prop_set_li_image);
    object_class_property_set_description(oc, "li-image",
        "The li2010_*.bi container to place in the Linux window at reset. "
        "Members are identified by content and the device tree is patched "
        "with the initramfs range before it is written; no CPU is started. "
        "Nothing is fabricated: an unrecognised container is a fatal error.");
    object_class_property_add(oc, "gpio-input-level", "uint64",
                              z63_prop_get_gpio_input,
                              z63_prop_set_gpio_input, NULL, NULL);
    object_class_property_set_description(oc, "gpio-input-level",
        "Answer every GPIO bank's input register (+0x010) with this word "
        "instead of what the guest last wrote. A measurement knob, not a "
        "model: the real values are per pin and are not known here. Use it "
        "to find out whether an input this board holds at zero is gating "
        "something, then replace it with the pin's actual value. Unset by "
        "default, which keeps the previous behaviour exactly.");
    object_class_property_add_str(oc, "snor-user", z63_prop_get_snor_user,
                                  z63_prop_set_snor_user);
    object_class_property_set_description(oc, "snor-user",
        "A 0x2680000-byte image for the serial NOR beyond the firmware "
        "(0x45980000-0x48000000, SNOR_USER). Those partitions hold "
        "factory-written per-unit bodies that are in no firmware package, so "
        "without this the region reads as zeros and the readers reject every "
        "body. z63-tools/snor_user_input.py builds the minimum the firmware's "
        "own validator checks -- four bytes of `ENDM` per partition trailer "
        "and nothing else, which is framing and not calibration. "
        "z63-tools/snor_user_extract.py instead takes the real content out "
        "of a full NOR dump, which is per-unit data from a particular body. "
        "This board cannot tell the two apart; the tools record which.");
    object_class_property_add_str(oc, "nand", z63_prop_get_nand,
                                  z63_prop_set_nand);
    object_class_property_set_description(oc, "nand",
        "A file to be the NAND chip on the controller at 0x17800000: "
        "1024 blocks of 64 pages of 2048 + 64 bytes (0x8400000 bytes). An "
        "empty or missing file is created erased. Program and erase are "
        "written back, so what the firmware stores there survives the run. "
        "Without it the chip is erased at every start -- a chip as it leaves "
        "the factory, not this body's, which the firmware's store treats as "
        "holding nothing yet. nikon-z63-nand.h has the layout and what is a "
        "declared choice.");
    object_class_property_add_bool(oc, "noc-windows",
                                   z63_prop_get_noc_windows,
                                   z63_prop_set_noc_windows);
    object_class_property_set_description(oc, "noc-windows",
        "Provision the NoC bus windows at reset (start and size of each DMA "
        "aperture), which the loader sets on the body and the RTOS only "
        "reads back. The values are ../z9-qemu's hypothesis for the same "
        "SoC, not measurements; see nikon-z63-noc.h. On by default.");
    object_class_property_add_str(oc, "scanout-dump",
                                  z63_prop_get_scanout_dump,
                                  z63_prop_set_scanout_dump);
    object_class_property_set_description(oc, "scanout-dump",
        "Write the composed scanout frame to this file as a binary PPM about "
        "once a second -- a host-side debugging aid for builds without a "
        "window or screendump. Nothing the guest observes changes.");
    object_class_property_add_str(oc, "scanout-live",
                                  z63_prop_get_scanout_live,
                                  z63_prop_set_scanout_live);
    object_class_property_set_description(oc, "scanout-live",
        "Keep the composed scanout frame, every frame, in this file for a "
        "viewer in another process to map (layout in nikon-z63-scanout.h; "
        "the z63ui repository reads it). Host-side only, like scanout-dump.");
    object_class_property_add_str(oc, "toplcd-live",
                                  z63_prop_get_toplcd_live,
                                  z63_prop_set_toplcd_live);
    object_class_property_set_description(oc, "toplcd-live",
        "Keep what the top LCD shows -- the 128x128 panel the real MCU "
        "drives over SPI2 -- in this file, in scanout-live's layout "
        "(nikon-z63-toplcd.h). Needs mcu-image=. Host-side only.");
    object_class_property_add_bool(oc, "mcu-camctl-reply",
                                   z63_prop_get_camctl, z63_prop_set_camctl);
    object_class_property_set_description(oc, "mcu-camctl-reply",
        "Answer the camera controller's start-up mode command (MCU family 9 "
        "kind 2) with a completion (kind 3). Off by default: without it the "
        "controller times out (camera error 0x34, Err on screen) but the "
        "menu works; with it there is no Err but the GUI waits for live "
        "view. See nikon-z63-mcu.h.");
    object_class_property_add_bool(oc, "mcu-capture-sync",
                                   z63_prop_get_capsync,
                                   z63_prop_set_capsync);
    object_class_property_set_description(oc, "mcu-capture-sync",
        "Answer CAPCOM's capture-sync request (GPIO port 3 bit 12) with a "
        "completion edge on EXT_INT_7 100 us later, as ../z7-qemu's test "
        "peer does; without it live view's start times out. Synthetic, off "
        "by default. See nikon-z63-mcu.h.");
    object_class_property_add_bool(oc, "z9-capture",
                                   z63_prop_get_z9_capture,
                                   z63_prop_set_z9_capture);
    object_class_property_set_description(oc, "z9-capture",
        "Put ../z9-qemu's capture models (sensor-interface events, ISP "
        "configuration, engines, statistics, RAW pipeline) in front of this "
        "board's register windows in 0x15000000..0x160fffff: where they "
        "answer, their answer is used. Needs z9-capture-raw. Off by default. "
        "See nikon-z63-z9lab.h.");
    object_class_property_add_bool(oc, "zf-ramdump-zeros",
                                   zf_prop_get_ramdump_zeros, zf_prop_set_ramdump_zeros);
    object_class_property_set_description(oc, "zf-ramdump-zeros",
        "Off by default: declared zero for unknown words only at the Zf original "
        "RAMDMP load in eleven observed descriptor spans; not hardware status");
    object_class_property_add_bool(oc, "zf-gpio22-bit8-high",
                                   zf_prop_get_gpio22_bit8_high, zf_prop_set_gpio22_bit8_high);
    object_class_property_set_description(oc, "zf-gpio22-bit8-high",
        "Off by default: drive only observed Zf GPIO group22 bit8 high; "
        "external input experiment, no card or software-ready state supplied");
    object_class_property_add_bool(oc, "zf-multi-io-counter",
                                   zf_prop_get_multi_io_counter, zf_prop_set_multi_io_counter);
    object_class_property_set_description(oc, "zf-multi-io-counter",
        "Off by default: isolated MIO counter using the cached functional "
        "100MHz/(divider+1) lab clock; physical timing unverified; no capture profile");
    object_class_property_add_bool(oc, "zf-sensorif-aux",
                                   zf_prop_get_sensorif_aux, zf_prop_set_sensorif_aux);
    object_class_property_set_description(oc, "zf-sensorif-aux",
        "Off by default: Zf-bound AUX0/1 configuration and Z9 pending/W1C/level IRQ; "
        "no sensor event source or DMA completion; requires zf-multi-io-counter");
    object_class_property_add_bool(oc, "zf-sensorif-output-config",
                                   zf_prop_get_output_config, zf_prop_set_output_config);
    object_class_property_set_description(oc, "zf-sensorif-output-config",
        "Off by default: own15-word output configuration in the shared Z9 shadow; "
        "no command/status/event path; requires zf-multi-io-counter");
    object_class_property_add_bool(oc, "zf-sensorif-group",
                                   zf_prop_get_sensorif_group, zf_prop_set_sensorif_group);
    object_class_property_set_description(oc, "zf-sensorif-group",
        "Off by default: native two-group bitmap/request snapshot with explicit "
        "idle command read0 hypothesis; no producer; requires zf-multi-io-counter");
    object_class_property_add_bool(oc, "zf-sensorif-primary",
                                   zf_prop_get_sensorif_primary, zf_prop_set_sensorif_primary);
    object_class_property_set_description(oc, "zf-sensorif-primary",
        "Off by default: native control/mask/W1C, shared FRONT configuration/latch, "
        "and command counts; no source clock; requires zf-multi-io-counter");
    object_class_property_add_str(oc, "zf-sensorif-source-period-ns",
                                  zf_prop_get_sensorif_source_period,
                                  zf_prop_set_sensorif_source_period);
    object_class_property_set_description(oc, "zf-sensorif-source-period-ns",
        "Default0/off: declared virtual SIF input period; native control gates and "
        "original IRQ consumers; physical timing unverified, no RAW/DMA source");
    object_class_property_add_bool(oc, "zf-isp1a3-config",
                                   zf_prop_get_isp1a3_config, zf_prop_set_isp1a3_config);
    object_class_property_set_description(oc, "zf-isp1a3-config",
        "Off by default: own107core+220lane+129LUT words perbank with original Z9 validbits; "
        "no control/readback/submit/IRQ/DMA; requires shared MIO object");
    object_class_property_add_bool(oc, "zf-isp199-config",
                                   zf_prop_get_isp199_config, zf_prop_set_isp199_config);
    object_class_property_set_description(oc, "zf-isp199-config",
        "Off by default: complete1596 ordinary words for each199/19a instance, "
        "same Z9 owner; only10/14/1704 native reads, no result/submit/IRQ/DMA");
    object_class_property_add_bool(oc, "zf-sensorif-source-stop",
                                   zf_prop_get_sensorif_source_stop,
                                   zf_prop_set_sensorif_source_stop);
    object_class_property_set_description(oc, "zf-sensorif-source-stop",
        "Default off: original Z9 immediate empty status-source drain on native "
        "active group0 clear; requires explicit SIF period; no pixel/DMA completion");
    object_class_property_add_bool(oc, "zf-stream154c-empty",
                                   zf_prop_get_stream154c_empty, zf_prop_set_stream154c_empty);
    object_class_property_set_description(oc, "zf-stream154c-empty",
        "Default off: three-channel ordinary configuration and existing Z9 empty-only "
        "query/reset/stop/W1C owner; no active transfer, DMA, completed buffer or IRQ producer");
    object_class_property_add_bool(oc, "zf-isp192-config",
                                   zf_prop_get_isp192_config, zf_prop_set_isp192_config);
    object_class_property_set_description(oc, "zf-isp192-config",
        "Default off: complete51 ordinary words each192/193 in original Z9 owner; "
        "only+10 native config read/raw32 RMW; no result/270/submit/IRQ/DMA");
    object_class_property_add_bool(oc, "zf-isp-small-config",
                                   zf_prop_get_isp_small_config, zf_prop_set_isp_small_config);
    object_class_property_set_description(oc, "zf-isp-small-config",
        "Default off: complete195/197/198 ordinary configuration in original Z9 "
        "owners, 33/44/35 words; raw32 RMW197+0; no results/submit/IRQ/DMA");
    object_class_property_add_bool(oc, "zf-statistics",
                                   zf_prop_get_statistics, zf_prop_set_statistics);
    object_class_property_set_description(oc, "zf-statistics",
        "Z9 primary controller, stop IRQ and synthetic RAW statistics; requires z9-capture-raw");
    object_class_property_add_bool(oc, "zf-ccomp-feedback",
                                   zf_prop_get_ccomp_feedback, zf_prop_set_ccomp_feedback);
    object_class_property_add_str(oc, "mcu-mode-lever",
                                  zf_prop_get_mode_lever,
                                  zf_prop_set_mode_lever);
    object_class_property_set_description(oc, "mcu-mode-lever",
        "Zf native MCU: the three-position mode lever's two matrix lines "
        "(row4 col0/col4, logical 46; body byte 0x57). idle/pos1/pos2");
    object_class_property_add_str(oc, "zf-ccomp-state", zf_prop_get_ccomp_state, NULL);
    object_class_property_set_description(oc, "zf-ccomp-feedback",
        "Default off: virtual controller GPIO3.12 request / GPIO3.7 response after100us");
    object_class_property_add_str(oc, "zf-toplcd-named-cap",
                                  zf_prop_get_toplcd_named_cap,
                                  zf_prop_set_toplcd_named_cap);
    object_class_property_set_description(oc, "zf-toplcd-named-cap",
        "Zf only: raise the top-LCD oddity naming cap to log every dropped byte "
        "(default 8; capture side of the Zf top-LCD protocol decode)");
    object_class_property_add_str(oc, "zf-sd-drive", zf_prop_get_sd_drive, zf_prop_set_sd_drive);
    object_class_property_set_description(oc, "zf-sd-drive",
        "Named SD block backend, SDHCI PIO/ADMA2 and native INTID132; absent by default");
    object_class_property_add_bool(oc, "zf-still", zf_prop_get_still, zf_prop_set_still);
    object_class_property_add_bool(oc, "zf-video-primary-source",
        zf_prop_get_video_primary_source, zf_prop_set_video_primary_source);
    object_class_property_add_bool(oc, "zf-video-primary-exclusive",
        zf_prop_get_video_primary_exclusive, zf_prop_set_video_primary_exclusive);
    object_class_property_add_bool(oc, "zf-video-codec",
        zf_prop_get_video_codec, zf_prop_set_video_codec);
    object_class_property_add_bool(oc, "zf-video-host-codec",
        zf_prop_get_video_host_codec, zf_prop_set_video_host_codec);
    object_class_property_set_description(oc, "zf-video-host-codec",
        "Explicit diagnostic host x264/x265 encoding from actual native DMA inputs; vendor VPU microcode is not executed");
    object_class_property_add_bool(oc, "zf-video-retained-start",
        zf_prop_get_video_retained_start, zf_prop_set_video_retained_start);
    object_class_property_set_description(oc, "zf-still",
        "Shared finite RAW/ISP/JPEG pipeline using Zf descriptors and synthetic input");
    object_class_property_add_str(oc, "zf-microsd-drive",
                                  zf_prop_get_microsd_drive,
                                  zf_prop_set_microsd_drive);
    object_class_property_set_description(oc, "zf-microsd-drive",
        "Optional SDK PCI mode3 backend at guest BAR0/600010000, INTx149/MSI150; physical identity unqualified");
    object_class_property_add_str(oc, "zf-sd-capability", zf_prop_get_sd_capability, NULL);
    object_class_property_add_str(oc, "zf-microsd-capability",
                                  zf_prop_get_microsd_capability, NULL);
    object_class_property_set_description(oc, "zf-sd-capability",
        "Read-only SDH30 host identity; physical maximum UHS-II and lower-speed compatibility are user-confirmed; negotiation timing is not modeled");
    object_class_property_set_description(oc, "zf-microsd-capability",
        "Read-only SDK mode3 model routing; physical slot assignment and UHS-II are unqualified");
    object_class_property_add_bool(oc, "zf-gpio22-bit8-low",
                                   zf_prop_get_gpio22_bit8_low, zf_prop_set_gpio22_bit8_low);
    object_class_property_set_description(oc, "zf-gpio22-bit8-low",
        "Explicit SDK GPIO22 bit8 card-present input; backend also drives this input");
    object_class_property_add_bool(oc, "service-core-events",
                                   z63_prop_get_service_core_events,
                                   z63_prop_set_service_core_events);
    object_class_property_set_description(oc, "service-core-events",
        "CPU3 (the LI payload's core) halts in WFE until another A53's SEV, "
        "../zr-qemu's service-core event model. For -accel tcg,thread=single "
        "-icount, where a WFE that only yields spends the instruction clock.");
    object_class_property_add_bool(oc, "cfexpress",
                                   z63_prop_get_cfexpress,
                                   z63_prop_set_cfexpress);
    object_class_property_set_description(oc, "cfexpress",
        "A card in the CFexpress slot: PCIe port 2 through ../z9-qemu's "
        "DesignWare adapter, the slot's detect and link reporting it. Give "
        "the card itself as an NVMe device on that port: -drive "
        "if=none,id=cf,file=<image>,format=raw -device "
        "nvme,drive=cf,serial=<s>,bus=dw-pcie,addr=0. Needs z9-capture=on.");
    object_class_property_add_bool(oc, "cfexpress-card",
                                   z63_prop_get_cfexpress_card,
                                   z63_prop_set_cfexpress_card);
    object_class_property_set_description(oc, "cfexpress-card",
        "Whether the CFexpress slot's detect line says a card is in it. On "
        "by default with cfexpress=on; set it at run time with qom-set "
        "/machine cfexpress-card false to take the card out and true to put "
        "it back, which is what the camera's card-detect service polls for. "
        "The NVMe endpoint itself is left alone.");
    object_class_property_add_str(oc, "z9-capture-props",
                                  z63_prop_get_z9_capture_props,
                                  z63_prop_set_z9_capture_props);
    object_class_property_set_description(oc, "z9-capture-props",
        "Replace the Z 9 model switches used by z9-capture, name=value items "
        "separated by ';'. Default: ../z9-qemu's profile less the blocks this "
        "board models itself.");
    object_class_property_add_str(oc, "z9-capture-raw",
                                  z63_prop_get_z9_capture_raw,
                                  z63_prop_set_z9_capture_raw);
    object_class_property_set_description(oc, "z9-capture-raw",
        "The NZ9RAW1 Bayer file the z9-capture statistics and RAW pipeline "
        "read (z63-tools/make_raw.py writes a synthetic test pattern). An "
        "input chosen for the run, not a sensor model.");
    object_class_property_add_str(oc, "imu-mg",
                                  z63_prop_get_imu_mg,
                                  z63_prop_set_imu_mg);
    object_class_property_set_description(oc, "imu-mg",
        "The posture sensor's input, x:y:z in milligravity; default 0:0:1000, "
        "a body at rest. Settable at run time with qom-set. See "
        "nikon-z63-imu.h.");
    object_class_property_add_str(oc, "imu-dps",
                                  z63_prop_get_imu_dps,
                                  z63_prop_set_imu_dps);
    object_class_property_set_description(oc, "imu-dps",
        "The posture sensor's gyroscope input, x:y:z in millidegrees per "
        "second, or 'sine' (default): a declared synthetic motion. Only seen "
        "once something powers the gyroscope, which stock firmware never "
        "does. Settable at run time with qom-set. See nikon-z63-imu.h.");
    object_class_property_add(oc, "i2c-byte-ns", "uint32",
                              z63_prop_get_i2c_ns,
                              z63_prop_set_i2c_ns, NULL, NULL);
    object_class_property_set_description(oc, "i2c-byte-ns",
        "Virtual time the I2C masters take per bus step (a byte, an address); "
        "default 100000. A test input, not a measured rate: the driver's "
        "divisors give about 24000 at an unmeasured 100 MHz pclk. See "
        "nikon-z63-i2c.h.");
    object_class_property_add(oc, "imu-clock-ppm", "int32",
                              z63_prop_get_imu_ppm,
                              z63_prop_set_imu_ppm, NULL, NULL);
    object_class_property_set_description(oc, "imu-clock-ppm",
        "How many parts per million the posture sensor's own clock (its ODR "
        "and FIFO timestamps) runs fast against the virtual clock; default "
        "0. A declared test input, not a measurement.");
    object_class_property_add_bool(oc, "lens", z63_prop_get_lens,
                                   z63_prop_set_lens);
    object_class_property_set_description(oc, "lens",
        "A synthetic lens on the mount's data link (serial instance 3, "
        "ready on GPIO port 3 bit 10 / EXT_INT_10, request on port 4 bit "
        "11): answers the EXPEED's lens transport with declared replies "
        "and logs every frame (Z63_LENS). Power and presence follow the "
        "real MCU's PB0/PA9 and PG0. Default off. See nikon-z63-lens.h.");
    object_class_property_add_str(oc, "lens-replies",
                                  z63_prop_get_lens_replies,
                                  z63_prop_set_lens_replies);
    object_class_property_set_description(oc, "lens-replies",
        "\"0x15:40+0x1f:0000\": with lens=on, answer these lens commands "
        "with these payload bytes (hex, after the 0x55) instead of the "
        "built-in replies; '+' separates entries. Settable at run time. "
        "See nikon-z63-lens.h.");
    object_class_property_add_bool(oc, "usb-cable", z63_prop_get_usb_cable,
                                   z63_prop_set_usb_cable);
    object_class_property_set_description(oc, "usb-cable",
        "A USB cable with VBUS in the camera's USB-C port (GPIO port 14 "
        "bit 10): on or off; settable at run time with qom-set. On by "
        "default when usbip= is given.");
    object_class_property_add_str(oc, "usbip", z63_prop_get_usbip,
                                  z63_prop_set_usbip);
    object_class_property_set_description(oc, "usbip",
        "A USB/IP server for the camera's USB port: <path>, unix:<path>, "
        "or [tcp:]<host>:<port>. The camera appears as bus id 1-1 once it "
        "is on the bus (usbip attach -r <host> -b 1-1; z63-tools/ptp.py). "
        "See nikon-z63-usbhost.h.");
    object_class_property_add_str(oc, "power-off", z63_prop_get_power_off,
                                  z63_prop_set_power_off);
    object_class_property_set_description(oc, "power-off",
        "What the EXPEED dropping its power-hold line (GPIO port 3 bit 11: "
        "standby after the standby timer, or a power-off) does: shutdown "
        "(the default; a guest shutdown, so -action shutdown=pause keeps "
        "the VM) or continue (log it; the EXPEED idles in WFI and the MCU "
        "runs on).");
    object_class_property_add_str(oc, "mcu-rtc", z63_prop_get_mcu_rtc,
                                  z63_prop_set_mcu_rtc);
    object_class_property_set_description(oc, "mcu-rtc",
        "The body MCU's clock at power-on, with mcu-image=: host (QEMU's "
        "RTC time, the default), YYYY-MM-DDTHH:MM:SS, or unset -- a "
        "calendar that has stopped, as with a flat clock battery, which "
        "the camera answers with its default time and the clock-not-set "
        "warning. See nikon-z63-stm32.h.");
    object_class_property_add_str(oc, "touch", z63_prop_get_touch,
                                  z63_prop_set_touch);
    object_class_property_set_description(oc, "touch",
        "Fingers on the rear touch panel, in its 1024x682 screen space from "
        "the top left: x:y, or x:y+x:y for two (a pinch); empty or 'up' "
        "lifts them. Run time only (qom-set /machine touch 512:341). See "
        "nikon-z63-touch.h.");
    object_class_property_add_str(oc, "mcu-keys",
                                  z63_prop_get_mcu_keys,
                                  z63_prop_set_mcu_keys);
    object_class_property_set_description(oc, "mcu-keys",
        "Keys the MCU peer reports held, joined with ':' (half, full, menu, "
        "ok, play, up, down, left, right, delete, disp, fn, fn2, fn3, movie, "
        "af-on); empty releases all. On the command line it is sent after the "
        "boot sequence; set at run time with qom-set /machine mcu-keys it is "
        "sent at once. See nikon-z63-mcu.h.");
    object_class_property_add_str(oc, "sio-sink",
                                  z63_prop_get_sio_sink,
                                  z63_prop_set_sio_sink);
    object_class_property_set_description(oc, "sio-sink",
        "Serial-bus instances to clock as write-only devices, separated by "
        "colons: each word sent is logged and a zero word comes back. "
        "Default \"9:8\", the rear panel's and the image sensor's control "
        "port; empty attaches none. See nikon-z63-sio.h.");
    object_class_property_add_bool(oc, "audio-codec",
                                   z63_prop_get_audio_codec,
                                   z63_prop_set_audio_codec);
    object_class_property_set_description(oc, "audio-codec",
        "Answer the audio codec on serial instance 10 (chip select GPIO "
        "port 5 bit 13) as a register file that starts at zero: writes are "
        "kept, reads return them. Default on; off leaves the instance "
        "unclocked. See nikon-z63-audio.h.");
    object_class_property_add_bool(oc, "storage-caps-stub",
                                   z63_prop_get_storage_caps_stub,
                                   z63_prop_set_storage_caps_stub);
    object_class_property_set_description(oc, "storage-caps-stub",
        "Answer the SD hosts' Capabilities and Host Controller Version "
        "registers with declared values instead of stopping. The values are "
        "derived from what the firmware tests and are logged at startup; "
        "they are not measured from the camera. On by default since the "
        "MCU-driven start reaches the card hosts; nikon-z63-storage.h.");
    object_class_property_add_bool(oc, "mcu-peer",
                                   z63_prop_get_mcu_peer,
                                   z63_prop_set_mcu_peer);
    object_class_property_set_description(oc, "mcu-peer",
        "Attach a synthetic peer for the external MCU on serial instances "
        "0/1 and EXT_INT_2/3. It implements the transport as the firmware's "
        "driver expects it and logs every frame; it is not the body's MCU "
        "and answers only what nikon-z63-mcu.h lists. On by default.");
    object_class_property_add_str(oc, "mcu-image", z63_prop_get_mcu_image,
                                  z63_prop_set_mcu_image);
    object_class_property_set_description(oc, "mcu-image",
        "Path to the body MCU's own flash image (ex2010_0200a0.bi). The MCU "
        "then runs on a Cortex-M4 beside the A53 cluster, and the synthetic "
        "peer is not attached. See nikon-z63-stm32.h.");
    object_class_property_add_str(oc, "mcu-spi-peer",
                                  z63_prop_get_mcu_spi_peer,
                                  z63_prop_set_mcu_spi_peer);
    object_class_property_set_description(oc, "mcu-spi-peer",
        "\"1:0+4:1\": bridge SPI<n> of the real MCU (mcu-image=) to this "
        "board's serial instance <m>. The default, \"1:0+4:1\", is the pairing "
        "both firmwares' frame checks agree with; every frame both ways is "
        "logged. Empty bridges nothing. See nikon-z63-stm32.h and "
        "nikon-z63-sio.h.");
    object_class_property_add_bool(oc, "battery-standin",
                                  zf_prop_get_battery_standin, zf_prop_set_battery_standin);
    object_class_property_set_description(oc, "battery-standin",
        "Diagnostic synthetic MCU battery report for startup; not physical authentication");
    object_class_property_add_str(oc, "mcu-battery-chardev",
                                 zf_prop_get_battery_chardev, zf_prop_set_battery_chardev);
    object_class_property_set_description(oc, "mcu-battery-chardev",
        "Explicit USART1 byte peer; no synthetic battery response by default");
    object_class_property_add_str(oc,"zf-i2c3e-state",zf_prop_get_display3e_state,NULL);
    object_class_property_add_str(oc,"i2c48-raw",zf_prop_get_temperature_raw,zf_prop_set_temperature_raw);
    object_class_property_set_description(oc,"i2c48-raw","Own bank3@48 signed12-bit temperature code; default historical fixture702; no unit/chip identity assertion");
    object_class_property_add_str(oc,"zf-i2c48-state",zf_prop_get_temperature_state,NULL);
    object_class_property_add_bool(oc,"eye-near",zf_prop_get_eye_near,zf_prop_set_eye_near);
    object_class_property_set_description(oc,"eye-near",
        "Zf own digital proximity output: GPIO44 port3/bit13 low=near, EXT_INT13; default far, no GUI state injection");
    object_class_property_add_str(oc,"zf-eye-state",zf_prop_get_eye_state,NULL);
    object_class_property_add_str(oc,"zf-panel-state",zf_prop_get_panel_state,NULL);
    object_class_property_add_str(oc,"zf-panel-snapshot",NULL,zf_prop_set_panel_snapshot);
    object_class_property_set_description(oc,"zf-panel-snapshot",
        "Runtime host-only prefix: exclusive .lcd.ppm/.evf.ppm/.json snapshots; no continuous file publishing");
    object_class_property_add_str(oc, "zf-access-led-state",
                                  zf_prop_get_access_led_state, NULL);
    object_class_property_set_description(oc, "zf-access-led-state",
        "Read-only Zf green access light: native GPIO139/group9 bit14 output; no host blinking policy");
    object_class_property_add_str(oc, "zf-charge-lamp-state",
                                  zf_prop_get_charge_lamp_state, NULL);
    object_class_property_set_description(oc, "zf-charge-lamp-state",
        "Read-only Zf USB-C charge indicator: native EX MCU PB4/PC9 output "
        "latches; orange follows PB4, and a finished charge cannot be reported "
        "because the model has no charger-status source");
    object_class_property_add_str(oc, "zf-lens-state",
                                  zf_prop_get_lens_state, NULL);
    object_class_property_set_description(oc, "zf-lens-state",
        "Read-only experimental Zf synthetic lens: 50mm f/1; the reused "
        "serial peer is not a hardware-qualified Zf lens protocol model");
    object_class_property_add_str(oc, "zf-toplcd-state", zf_prop_get_toplcd_state, NULL);
    object_class_property_set_description(oc, "zf-toplcd-state",
        "Read-only Zf SPI2 segment RAM, two digits/one decimal/unknown auxiliary bit and CS/DISP; no value injection");
    object_class_property_add_str(oc, "mcu-mode-adc", zf_prop_get_mode_adc, zf_prop_set_mode_adc);
    object_class_property_set_description(oc, "mcu-mode-adc",
        "Zf own ADC1ch1 electrical input 0..4095; native conversion/debounce/report, physical mode names not assumed");
    object_class_property_add_str(oc, "mcu-rotary", zf_prop_get_rotary, zf_prop_set_rotary);
    object_class_property_set_description(oc, "mcu-rotary",
        "Zf aggregate matrix row0/2/5 electrical contact mask (1=open); separate native dial position tables; =open disables");
    object_class_property_add_str(oc, "mcu-button", zf_prop_get_button, zf_prop_set_button);
    object_class_property_set_description(oc, "mcu-button",
        "Zf native single contact: none/play/delete/zoom-in/zoom-out/ae-af-lock/fn/movie/ok/i/disp/display-mode; matrix-l39/l42/l45 are logical-ID aliases");
    object_class_property_add_str(oc, "mcu-dial", NULL, zf_prop_set_dial);
    object_class_property_set_description(oc, "mcu-dial",
        "Zf own main PA10/PA11 or sub PE7/PE13 Gray cycles, signed counter detents; front/rear unverified");
    if (!object_class_property_find(oc, "mcu-fn")) {
        object_class_property_add_bool(oc, "mcu-fn", zf_prop_get_fn, zf_prop_set_fn);
        object_class_property_set_description(oc, "mcu-fn",
            "Persistent Zf Fn modifier contact; independent from mcu-button");
    }
    object_class_property_add_str(oc, "mcu-navigation", zf_prop_get_navigation, zf_prop_set_navigation);
    object_class_property_set_description(oc, "mcu-navigation",
        "Runtime Zf PF3 navigation contacts: none/up/down/left/right, native EX scanning");
    object_class_property_add_bool(oc, "mcu-menu",
                                   zf_prop_get_menu, zf_prop_set_menu);
    object_class_property_set_description(oc, "mcu-menu",
        "Runtime Zf MENU matrix contact PF1/PF10, native EX scan; no guest state injection");
    object_class_property_add_str(oc, "mcu-pins", z63_prop_get_mcu_pins,
                                  z63_prop_set_mcu_pins);
    object_class_property_set_description(oc, "mcu-pins",
        "\"G7=1,C5=0\": levels to drive on the real MCU's own input pins at "
        "reset, standing in for the wires the EXPEED holds. Only meaningful "
        "with mcu-image=. Default \"D13=1\" (the battery data line's idle "
        "level); a value replaces it. qom-set at run time drives the named "
        "pins at once (G0=1 / G0=0: lens off / on the mount). See "
        "nikon-z63-stm32.h.");
    object_class_property_add_str(oc, "mcu-wires", z63_prop_get_mcu_wires,
                                  z63_prop_set_mcu_wires);
    object_class_property_set_description(oc, "mcu-wires",
        "\"B12=a3:2+E3=a3:3+D12=a4:3\": handshake wires between the real "
        "MCU (mcu-image=) and the EXPEED's GPIO_TOP banks: <MCU port><pin> "
        "and a<EXPEED port>:<bit> name the same line. Declared, and each "
        "wire is logged; empty (the default) leaves the pins as they were, "
        "which is what the two chips' own firmware reads. See nikon-z63.c's "
        "Z63McuWire.");
    object_class_property_add_bool(oc, "gfx-skip-unmodelled",
                                   z63_prop_get_gfx_skip,
                                   z63_prop_set_gfx_skip);
    object_class_property_set_description(oc, "gfx-skip-unmodelled",
        "Drop a graphics draw the model cannot do, at its commit, instead of "
        "stopping the machine: no pixels are written, fences complete, and "
        "each distinct reason is named once on stderr. For runs that have to "
        "get past the GUI; the picture is then knowingly incomplete. Off by "
        "default. See nikon-z63-gfx.h.");
    object_class_property_add_bool(oc, "li-restage",
                                   z63_prop_get_li_restage,
                                   z63_prop_set_li_restage);
    object_class_property_set_description(oc, "li-restage",
        "Diagnostic direct loader: reload LI at the MASTER hand-off after "
        "erased NAND reads. Does not validate native NAND provisioning. "
        "Off by default; only supported for the experimental Zf board.");
    object_class_property_add_bool(oc, "li-wait-ipc",
                                   z63_prop_get_li_wait_ipc,
                                   z63_prop_set_li_wait_ipc);
    object_class_property_set_description(oc, "li-wait-ipc",
        "Hold CPU3 until the RTOS has published the share_mem directory "
        "(the \"MASTER\" header at its base), instead of starting Linux at "
        "machine reset. The camera application attaches to that directory "
        "and gives up for good if it is not there yet, which is what a "
        "3-second Linux boot racing a much slower emulated RTOS always "
        "produced. On by default; off restores the immediate hand-off.");
    object_class_property_add_bool(oc, "image-in-dram",
                                   z63_prop_get_image_in_dram,
                                   z63_prop_set_image_in_dram);
    object_class_property_set_description(oc, "image-in-dram",
        "Place the image in DRAM at the three addresses the firmware's own "
        "initial page tables map parts of its text to (0xa15c00000, "
        "0xa16600000, 0xa17200000). On the body the loader does this; this "
        "board otherwise only puts the image in the NOR window, so those "
        "windows read as zeros and a branch into them fetches 0x00000000. "
        "See z63_image_in_dram for the measured map and what is a declared "
        "choice. On by default; off reproduces the earlier behaviour.");
    object_class_property_add_str(oc, "loader", z63_prop_get_loader,
                                  z63_prop_set_loader);
    object_class_property_set_description(oc, "loader",
        "The 0x160000 bytes below the SOLID image (0x40000000-0x40160000), "
        "which the partition table calls the loader. It is not in the update "
        "package -- that holds only the image and the Linux component -- so "
        "without this the band reads as zeros and anything the image calls "
        "down there executes `udf`. Give either exactly 0x160000 bytes or a "
        "whole dump of the window from 0x40000000, of which the first "
        "0x160000 is used. The boot header the board otherwise synthesises "
        "comes from the file instead, and the board checks the two agree. "
        "A dump read out of a running body is that body's resident loader, "
        "which is not the same thing as what is in its flash at reset.");
    object_class_property_add_bool(oc, "rom-page-stub",
                                   z63_prop_get_rom_page_stub,
                                   z63_prop_set_rom_page_stub);
    object_class_property_set_description(oc, "rom-page-stub",
        "Answer the boot ROM's service page at 0x08201000 with a labelled "
        "stand-in: `ret` at the six entry points the image calls, logged on "
        "every use, and a stop anywhere else in the page. The page is mask "
        "ROM and is in no firmware package, so this substitutes for missing "
        "code rather than modelling it. Off by default.");
    object_class_property_add(oc, "cpu3-entry", "uint64",
                              z63_prop_get_cpu3_entry,
                              z63_prop_set_cpu3_entry, NULL, NULL);
    object_class_property_set_description(oc, "cpu3-entry",
        "Hand CPU3 to the LI payload at reset with this entry point, "
        "instead of leaving it for the RTOS's release controller. The RTOS "
        "never releases CPU3 on its own, so this is the only way it runs. "
        "A labelled stand-in for the boot ROM's RTOS->Linux hand-off, not "
        "a model of the ROM. Off (0) by default.");
}

static const TypeInfo z63_machine_info = {
    .name          = TYPE_NIKON_Z63,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(NikonZ63State),
    .instance_init = z63_machine_instance_init,
    .class_init    = z63_machine_class_init,
    .interfaces    = aarch64_machine_interfaces,
};

/*
 * The body MCU on its own, for bringing its image up without the A53
 * cluster: -machine nikon-z63-mcu -kernel ex2010_0200a0.bi. The same
 * Z63Stm32 is what the main machine will carry. See nikon-z63-stm32.h.
 */
#define TYPE_NIKON_Z63_MCU MACHINE_TYPE_NAME("nikon-z63-mcu")

typedef struct NikonZ63McuMachine {
    MachineState parent;
    Z63Stm32 mcu;
    char *pins;
    bool built;
} NikonZ63McuMachine;

static void z63_mcu_machine_init(MachineState *machine)
{
    NikonZ63McuMachine *s = OBJECT_CHECK(NikonZ63McuMachine, machine,
                                         TYPE_NIKON_Z63_MCU);

    if (!machine->kernel_filename) {
        error_report("nikon-z63-mcu needs -kernel <the MCU image, "
                     "ex2010_0200a0.bi>");
        exit(1);
    }
    Chardev *serial[6];

    for (unsigned i = 0; i < 6; i++) {
        serial[i] = serial_hd(i);
    }
    z63_stm32_init(&s->mcu, OBJECT(machine), machine->kernel_filename,
                   serial, &error_fatal);
    s->built = true;
    if (s->pins) {
        z63_stm32_pins(&s->mcu, s->pins, &error_fatal);
    }
}

static char *z63_mcu_machine_get_pins(Object *obj, Error **errp)
{
    return g_strdup(OBJECT_CHECK(NikonZ63McuMachine, obj,
                                 TYPE_NIKON_Z63_MCU)->pins ?: "");
}

static void z63_mcu_machine_set_pins(Object *obj, const char *value,
                                     Error **errp)
{
    NikonZ63McuMachine *s = OBJECT_CHECK(NikonZ63McuMachine, obj,
                                         TYPE_NIKON_Z63_MCU);

    g_free(s->pins);
    s->pins = g_strdup(value);
    if (s->built) {                     /* qom-set at run time */
        z63_stm32_pins(&s->mcu, value, errp);
    }
}

static void z63_mcu_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m4"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Nikon Z 6III body MCU alone (STM32F469 class, its own "
               "image via -kernel; experimental)";
    mc->init = z63_mcu_machine_init;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m4");
    object_class_property_add_str(oc, "pins", z63_mcu_machine_get_pins,
                                  z63_mcu_machine_set_pins);
    object_class_property_set_description(oc, "pins",
        "Drive MCU inputs, e.g. G7=1,C5=0 (a switch, a handshake line); at "
        "run time with qom-set /machine pins. See nikon-z63-stm32.h.");
}

static const TypeInfo z63_mcu_machine_info = {
    .name          = TYPE_NIKON_Z63_MCU,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(NikonZ63McuMachine),
    .class_init    = z63_mcu_machine_class_init,
    .interfaces    = arm_machine_interfaces,
};

static void z63_machine_init(void)
{
    type_register_static(&z63_machine_info);
    type_register_static(&z63_mcu_machine_info);
}

type_init(z63_machine_init);
