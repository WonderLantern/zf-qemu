/*
 * Nikon Z 6III -- the DWC3 at 0x17880000 as a device-mode controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The register file in nikon-z63-usb.h carried the driver through its
 * bring-up (core soft reset, command-active bits) and no further, because
 * what is left is not a register but a conversation: a SETUP packet has to
 * be put where the driver is looking, and its arrival posted to the event
 * ring the driver reads. This is that conversation, ported from
 * ../vphone-qemu's hw/usb/apple-dwc3.c (the iPhone board's device-mode
 * DWC3, GPL-2.0-or-later, same authors), with the Z 6III's own subset:
 *
 *   - one event buffer (GEVNTADRLO/HI/SIZ/COUNT at 0xc400..0xc40c, set to
 *     0xa1aa0c000 / 0x1000 by sub_40C70F14; the walker sub_40C6C454 reads
 *     events until GEVNTCOUNT is 0 and writes back what it took);
 *   - sixteen endpoint command blocks (PAR2/PAR1/PAR0/DEPCMD at 0xc800 +
 *     0x10n, composed by sub_40C70378: param << 16 | cmd, CMDIOC when
 *     RUN_STOP is set, CMDACT always);
 *   - the device generic command (DGCMDPAR/DGCMD 0xc710/0xc714), which
 *     the iPhone driver never used: answered as done, nothing else;
 *   - DSTS from the state: CONNECTSPD high speed after a reset, link in
 *     U0, DEVCTRLHLT while the run bit is clear, and the frame number
 *     moving with virtual time (a driver that waits polls it).
 *
 * The bus side is a token interface, z63_dwc3_token(), which is what a
 * host's packets are: a SETUP lands in the descriptor the driver armed on
 * physical endpoint 0, an IN or OUT is moved through the endpoint's
 * descriptor chain (TRBs in the driver's rings, DRAM 0xa1aa09410 +
 * 1040n), and each finished descriptor is handed back (HWO clear, residue
 * and status in its size word). As on the part, a finished descriptor is
 * an event only when it ends the transfer -- LST, or a short OUT packet:
 * XferComplete -- or asks for one (IOC: XferInProgress); the descriptors
 * of a chain in between are silent. (GetObject's data phase is two
 * descriptors, 512 KiB and the rest: an XferComplete for the first has
 * the driver stall both bulk endpoints when the real end comes.) An endpoint
 * with nothing armed answers NAK, and on endpoint 0 says XferNotReady
 * once. The run bit and the bus reset go through the listener and
 * z63_dwc3_bus_reset(). The iPhone model was a QEMU USBDevice through a
 * hook its tree added to the USB core; this tree's core runs control
 * transfers itself and wants them answered at once, which a device that
 * is guest firmware cannot do, so the host (nikon-z63-usbhost.h) calls
 * in directly.
 *
 * This driver numbers its hardware endpoints its own way: hardware
 * endpoint = (bEndpointAddress & 0xf) + 1 (sub_40C70950 at 0x40C70A50),
 * so the PTP interface's 0x81 / 0x02 / 0x83 are 2 / 3 / 4, and DEPCMD(n)
 * and the events carry that n. Which USB endpoint a hardware endpoint
 * serves is what SETEPCONFIG says in parameter 1 bits 29:26 (number) and
 * 25 (direction), as on the part; DALEPENA keeps the USB numbering, bit
 * 2 * number + direction (sub_40C70B14). A token is routed by that
 * configuration, not by an assumed 2n + direction. Each transfer is one
 * STARTTRANSFER over a chain ending in a descriptor with LST
 * (sub_40C70148), whose completion ends it; UPDATETRANSFER is only ever
 * written without CMDACT (isochronous rings), so nothing waits on it.
 *
 * The register discipline is the board's: an offset no call site names
 * still stops (z63_dwc3_offset_known in nikon-z63-usb.h); every other
 * offset the driver names and nothing here gives behaviour is stored.
 * The iPhone model's lessons are kept with it -- they are about the
 * DWC3's programming model, not that driver: descriptors are fetched at
 * the command, a link is followed and not written, an IN shorter than
 * its descriptor keeps the descriptor, a SETUP clears a protocol stall.
 */

#ifndef HW_ARM_NIKON_Z63_DWC3_H
#define HW_ARM_NIKON_Z63_DWC3_H

#include "qemu/log.h"
#include "system/dma.h"

#define Z63_DWC3_EPS            32      /* physical: 2n out, 2n+1 in */
#define Z63_DWC3_EVENTS         1

#define Z63_DWC3_GSNPSID        0xc120
#define Z63_DWC3_DSTS_DEVCTRLHLT (1u << 22)
#define Z63_DWC3_DSTS_SOFFN_SHIFT 3
#define Z63_DWC3_DSTS_SOFFN_MASK 0x3fff
#define Z63_DWC3_MICROFRAME_NS  125000
#define Z63_DWC3_DCTL_RUN_STOP  (1u << 31)
#define Z63_DWC3_GEVNTSIZ_INTMASK (1u << 31)
#define Z63_DWC3_GEVNTSIZ_MASK  0xffff
#define Z63_DWC3_GEVNTCOUNT_MASK 0xfffc

#define Z63_DEPCMD_CMDIOC       (1u << 8)
#define Z63_DEPCMD_RSC_SHIFT    16
#define Z63_DEPCMD_SETEPCONFIG  1
#define Z63_DEPCMD_SETTRANSFRESOURCE 2
#define Z63_DEPCMD_GETEPSTATE   3
#define Z63_DEPCMD_SETSTALL     4
#define Z63_DEPCMD_CLEARSTALL   5
#define Z63_DEPCMD_STARTTRANSFER 6
#define Z63_DEPCMD_UPDATETRANSFER 7
#define Z63_DEPCMD_ENDTRANSFER  8
#define Z63_DEPCMD_STARTCFG     9

#define Z63_DEPEVT_XFERCOMPLETE 1
#define Z63_DEPEVT_XFERINPROGRESS 2
#define Z63_DEPEVT_XFERNOTREADY 3
#define Z63_DEPEVT_EPCMDCMPLT   7
#define Z63_DEVT_DISCONNECT     0
#define Z63_DEVT_RESET          1
#define Z63_DEVT_CONNECTDONE    2
#define Z63_DEPEVT_STATUS_CONTROL_DATA   1
#define Z63_DEPEVT_STATUS_CONTROL_STATUS 2
#define Z63_DEPEVT_STATUS_SHORT (1u << 1)
#define Z63_DEPEVT_STATUS_IOC   (1u << 2)
#define Z63_DEPEVT_STATUS_LST   (1u << 3)

typedef struct Z63Trb {
    uint32_t bpl, bph, size, ctrl;
} Z63Trb;

#define Z63_TRB_HWO             (1u << 0)
#define Z63_TRB_LST             (1u << 1)
#define Z63_TRB_CHN             (1u << 2)
#define Z63_TRB_IOC             (1u << 11)
#define Z63_TRB_TYPE(x)         (((x) >> 4) & 0x3f)
#define Z63_TRB_SIZE_MASK       0x00ffffff
#define Z63_TRB_STS_MASK        (0xfu << 28)
#define Z63_TRBCTL_NORMAL       1
#define Z63_TRBCTL_LINK         8
#define Z63_TRB_PLAUSIBLE_SIZE  (1u << 20)
#define Z63_TRB_WALK_LIMIT      64

typedef struct Z63Dwc3Ep {
    uint32_t par2, par1, par0, cmd;
    uint32_t maxp, type;
    unsigned interrupter;
    unsigned usb_num;       /* SETEPCONFIG parameter 1, 29:26 */
    bool usb_in;            /* and 25 */
    bool configured;
    uint64_t trb;           /* where the walk is */
    uint32_t taken;         /* of that descriptor, already sent */
    bool armed, stalled, asked;
} Z63Dwc3Ep;

typedef struct Z63Dwc3 Z63Dwc3;

/* A token's outcome. */
enum { Z63_TOK_OK, Z63_TOK_NAK, Z63_TOK_STALL };
#define Z63_TOK_SETUP   0x2d
#define Z63_TOK_IN      0x69
#define Z63_TOK_OUT     0xe1

/* What the host beside it hears that the wire does not say. */
typedef struct Z63Dwc3Listener {
    void (*attached)(void *opaque);
    void (*detached)(void *opaque);
    void (*bus_reset)(void *opaque);
    void (*ep_armed)(void *opaque, unsigned phys);
    void *opaque;
} Z63Dwc3Listener;

struct Z63Dwc3 {
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0x10000 / 4];     /* what has no behaviour: stored */
    struct {
        uint32_t adrlo, adrhi, size, count, put;
        bool raised;
    } evt[Z63_DWC3_EVENTS];
    uint32_t dctl, dsts, dalepena;
    Z63Dwc3Ep ep[Z63_DWC3_EPS];
    bool on_bus;
    Z63Dwc3Listener listener;
    bool trace;
    uint64_t events, transfers;
};

#define Z63_DWC3_LOG(s, fmt, ...)                                    \
    do {                                                             \
        if ((s)->trace) {                                            \
            qemu_log("z63-dwc3: " fmt "\n", ##__VA_ARGS__);          \
        }                                                            \
    } while (0)

/*
 * The line stands while the ring holds events the driver has not taken
 * and its mask (GEVNTSIZ bit 31, which the ISR sub_40C6C618 sets before
 * waking EXSUSBDP) is clear.
 */
static void z63_dwc3_update_irq(Z63Dwc3 *s, unsigned n)
{
    bool level = (s->evt[n].count & Z63_DWC3_GEVNTCOUNT_MASK) &&
                 !(s->evt[n].size & Z63_DWC3_GEVNTSIZ_INTMASK);

    if (level != s->evt[n].raised) {
        Z63_DWC3_LOG(s, "line %s (%u byte(s), mask %d)", level ? "up" : "down",
                     s->evt[n].count,
                     !!(s->evt[n].size & Z63_DWC3_GEVNTSIZ_INTMASK));
    }
    s->evt[n].raised = level;
    qemu_set_irq(s->irq, level);
}

static void z63_dwc3_push(Z63Dwc3 *s, unsigned n, uint32_t word)
{
    uint64_t base = ((uint64_t)s->evt[n].adrhi << 32) | s->evt[n].adrlo;
    uint32_t size = s->evt[n].size & Z63_DWC3_GEVNTSIZ_MASK;
    uint32_t put;

    if (size < sizeof(word) || !base) {
        Z63_DWC3_LOG(s, "event 0x%08x dropped: no event buffer", word);
        return;
    }
    if (s->evt[n].count + sizeof(word) > size) {
        error_report("Z63_DWC3: event buffer full, event 0x%08x dropped",
                     word);
        return;
    }
    put = s->evt[n].put % size;
    word = cpu_to_le32(word);
    if (dma_memory_write(&address_space_memory, base + put, &word,
                         sizeof(word), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return;
    }
    s->evt[n].put = (put + sizeof(word)) % size;
    s->evt[n].count += sizeof(word);
    s->events++;
    Z63_DWC3_LOG(s, "event 0x%08x, %u byte(s) outstanding",
                 le32_to_cpu(word), s->evt[n].count);
    z63_dwc3_update_irq(s, n);
}

static void z63_dwc3_depevt(Z63Dwc3 *s, unsigned phys, unsigned type,
                            unsigned status, unsigned params)
{
    unsigned n = s->ep[phys].interrupter;

    z63_dwc3_push(s, n < Z63_DWC3_EVENTS ? n : 0,
                  ((phys & 0x1f) << 1) | ((type & 0xf) << 6) |
                  ((status & 0xf) << 12) | ((params & 0xffff) << 16));
}

static void z63_dwc3_devt(Z63Dwc3 *s, unsigned type, unsigned info)
{
    z63_dwc3_push(s, 0, 1u | ((type & 0xf) << 8) | ((info & 0x1ff) << 16));
}

static void z63_dwc3_ep_reset(Z63Dwc3 *s, unsigned phys)
{
    Z63Dwc3Ep *ep = &s->ep[phys];

    ep->armed = ep->stalled = ep->asked = false;
    ep->trb = 0;
    ep->taken = 0;
}

/* The run bit: the device may be seen. The host notices and resets it,
 * and the reset says what speed came out of it. */
static void z63_dwc3_set_running(Z63Dwc3 *s, bool running)
{
    if (running) {
        s->dsts &= ~Z63_DWC3_DSTS_DEVCTRLHLT;
        if (!s->on_bus) {
            s->on_bus = true;
            error_report("Z63_DWC3: run bit set -- the camera is on the bus");
            if (s->listener.attached) {
                s->listener.attached(s->listener.opaque);
            }
        }
    } else {
        s->dsts |= Z63_DWC3_DSTS_DEVCTRLHLT;
        if (s->on_bus) {
            s->on_bus = false;
            error_report("Z63_DWC3: run bit clear -- off the bus");
            if (s->listener.detached) {
                s->listener.detached(s->listener.opaque);
            }
        }
    }
}

static bool z63_dwc3_trb_plausible(const Z63Trb *trb)
{
    unsigned type = Z63_TRB_TYPE(trb->ctrl);
    uint32_t size = trb->size & Z63_TRB_SIZE_MASK;

    if ((trb->ctrl & 0xffff0000u) || type < Z63_TRBCTL_NORMAL ||
        type > Z63_TRBCTL_LINK) {
        return false;
    }
    return type == Z63_TRBCTL_LINK ? size == 0
                                   : size <= Z63_TRB_PLAUSIBLE_SIZE;
}

static bool z63_dwc3_read_trb(uint64_t at, Z63Trb *trb)
{
    if (dma_memory_read(&address_space_memory, at, trb, sizeof(*trb),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }
    trb->bpl = le32_to_cpu(trb->bpl);
    trb->bph = le32_to_cpu(trb->bph);
    trb->size = le32_to_cpu(trb->size);
    trb->ctrl = le32_to_cpu(trb->ctrl);
    return true;
}

static void z63_dwc3_write_trb(uint64_t at, const Z63Trb *trb)
{
    Z63Trb w = {
        cpu_to_le32(trb->bpl), cpu_to_le32(trb->bph),
        cpu_to_le32(trb->size), cpu_to_le32(trb->ctrl),
    };

    smp_wmb();
    dma_memory_write(&address_space_memory, at, &w, sizeof(w),
                     MEMTXATTRS_UNSPECIFIED);
}

/* Links are followed at the command, while they are valid (hardware
 * fetches at STARTTRANSFER/UPDATETRANSFER). */
static void z63_dwc3_follow_links(Z63Dwc3 *s, unsigned phys)
{
    Z63Dwc3Ep *ep = &s->ep[phys];

    for (unsigned steps = 0; ep->trb && !ep->taken &&
                             steps < Z63_TRB_WALK_LIMIT; steps++) {
        Z63Trb trb;

        if (!z63_dwc3_read_trb(ep->trb, &trb) || !(trb.ctrl & Z63_TRB_HWO) ||
            Z63_TRB_TYPE(trb.ctrl) != Z63_TRBCTL_LINK ||
            !z63_dwc3_trb_plausible(&trb)) {
            break;
        }
        ep->trb = ((uint64_t)trb.bph << 32) | trb.bpl;
    }
}

static void z63_dwc3_depcmd(Z63Dwc3 *s, unsigned phys, uint32_t cmd)
{
    Z63Dwc3Ep *ep = &s->ep[phys];
    unsigned type = cmd & 0xf, resource = 0;

    Z63_DWC3_LOG(s, "ep %u command %u (0x%08x) par0 0x%08x par1 0x%08x",
                 phys, type, cmd, ep->par0, ep->par1);
    switch (type) {
    case Z63_DEPCMD_STARTCFG:
        /*
         * Its transfer-resource index says where the reset starts: 0 at
         * initialisation, every endpoint; 2 at SET_CONFIGURATION
         * (0x00020509 here), only the ones past the control pair -- whose
         * status stage is still to come. (Resetting endpoint 1 there had
         * it say XferNotReady(status) twice; the driver answered the
         * second in the next session's data stage with a zero-length
         * packet, and every control stage after it was one behind.)
         */
        if (phys == 0) {
            for (unsigned i = (cmd >> 16) & 0x7f; i < Z63_DWC3_EPS; i++) {
                z63_dwc3_ep_reset(s, i);
            }
        }
        break;
    case Z63_DEPCMD_SETEPCONFIG:
        ep->type = (ep->par0 >> 1) & 3;
        ep->maxp = (ep->par0 >> 3) & 0x7ff;
        ep->interrupter = ep->par1 & 0x1f;
        ep->usb_num = (ep->par1 >> 26) & 0xf;
        ep->usb_in = (ep->par1 >> 25) & 1;
        ep->configured = true;
        Z63_DWC3_LOG(s, "hardware endpoint %u is USB endpoint %u %s, type "
                     "%u, max packet %u", phys, ep->usb_num,
                     ep->usb_in ? "in" : "out", ep->type, ep->maxp);
        break;
    case Z63_DEPCMD_SETTRANSFRESOURCE:
        break;
    case Z63_DEPCMD_SETSTALL:
        ep->stalled = true;
        break;
    case Z63_DEPCMD_CLEARSTALL:
        ep->stalled = false;
        break;
    case Z63_DEPCMD_STARTTRANSFER:
        ep->trb = ((uint64_t)ep->par0 << 32) | ep->par1;
        ep->taken = 0;
        ep->armed = true;
        ep->asked = false;
        resource = phys + 1;
        if (phys >= 2) {
            z63_dwc3_follow_links(s, phys);
        }
        break;
    case Z63_DEPCMD_UPDATETRANSFER:
        if (phys >= 2) {
            z63_dwc3_follow_links(s, phys);
        }
        break;
    case Z63_DEPCMD_ENDTRANSFER:
        ep->armed = false;
        ep->trb = 0;
        ep->taken = 0;
        break;
    case Z63_DEPCMD_GETEPSTATE:
        ep->par2 = 0;
        break;
    default:
        error_report("Z63_DWC3: endpoint %u command %u is not modelled",
                     phys, type);
        break;
    }
    ep->cmd = (cmd & ~Z63_DWC3_CMDACT & 0xffff) |
              (type == Z63_DEPCMD_STARTTRANSFER ?
               resource << Z63_DEPCMD_RSC_SHIFT : cmd & 0xffff0000u);
    if (cmd & Z63_DEPCMD_CMDIOC) {
        /* the command's type in parameter 11:8: the driver ends a
         * transfer on EPCmdCmplt of ENDTRANSFER */
        z63_dwc3_depevt(s, phys, Z63_DEPEVT_EPCMDCMPLT, 0, type << 8);
    }
    if ((type == Z63_DEPCMD_STARTTRANSFER ||
         type == Z63_DEPCMD_UPDATETRANSFER) && s->listener.ep_armed) {
        s->listener.ep_armed(s->listener.opaque, phys);
    }
}

/* One token's worth of a transfer through the endpoint's descriptors.
 * Returns false when nothing armed is the block's yet (a NAK). */
static bool z63_dwc3_run_trbs(Z63Dwc3 *s, unsigned phys, bool in,
                              uint8_t *buf, uint32_t total,
                              unsigned *moved, unsigned *status,
                              bool *complete)
{
    Z63Dwc3Ep *ep = &s->ep[phys];
    unsigned done = 0, steps = 0;
    uint64_t at = ep->trb;
    bool ours = false;

    while ((done < total || !ours) && steps++ < Z63_TRB_WALK_LIMIT) {
        Z63Trb trb;
        uint64_t buffer;
        uint32_t room, take;

        if (!z63_dwc3_read_trb(at, &trb) || !(trb.ctrl & Z63_TRB_HWO)) {
            break;
        }
        smp_rmb();
        if (!z63_dwc3_trb_plausible(&trb)) {
            Z63_DWC3_LOG(s, "ep %u: 0x%" PRIx64 " holds no descriptor",
                         phys, at);
            break;
        }
        buffer = ((uint64_t)trb.bph << 32) | trb.bpl;
        if (Z63_TRB_TYPE(trb.ctrl) == Z63_TRBCTL_LINK) {
            at = buffer;
            ep->trb = at;
            continue;
        }
        ours = true;
        room = (trb.size & Z63_TRB_SIZE_MASK) - ep->taken;
        buffer += ep->taken;
        take = MIN(room, total - done);
        if (in) {
            if (take && dma_memory_read(&address_space_memory, buffer,
                                        buf + done, take,
                                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                take = 0;
            }
        } else if (take) {
            dma_memory_write(&address_space_memory, buffer, buf + done, take,
                             MEMTXATTRS_UNSPECIFIED);
        }
        done += take;
        if (in && take < room) {
            ep->taken += take;      /* the rest of it is still owed */
            break;
        }
        ep->taken = 0;
        *status = (take < room ? Z63_DEPEVT_STATUS_SHORT : 0) |
                  (trb.ctrl & Z63_TRB_IOC ? Z63_DEPEVT_STATUS_IOC : 0) |
                  (trb.ctrl & Z63_TRB_LST ? Z63_DEPEVT_STATUS_LST : 0);
        trb.size = (trb.size & ~(Z63_TRB_SIZE_MASK | Z63_TRB_STS_MASK)) |
                   (room - take);
        trb.ctrl &= ~Z63_TRB_HWO;
        z63_dwc3_write_trb(at, &trb);
        at += sizeof(trb);
        ep->trb = at;
        if (!in && take < room) {
            trb.ctrl |= Z63_TRB_LST;    /* a short packet ends it there */
        }
        if (trb.ctrl & Z63_TRB_LST) {
            *complete = true;
        } else if (trb.ctrl & Z63_TRB_IOC) {
            Z63_DWC3_LOG(s, "ep %u: descriptor done, transfer goes on", phys);
            z63_dwc3_depevt(s, phys, Z63_DEPEVT_XFERINPROGRESS, *status, 0);
        }
        if (trb.ctrl & Z63_TRB_LST) {
            /* the transfer is over: the next needs a STARTTRANSFER */
            at = 0;
            ep->armed = false;
            break;
        }
        if (!(trb.ctrl & Z63_TRB_CHN)) {
            break;
        }
    }
    *moved = done;
    ep->trb = at;
    return ours;
}

/* A token from the host: pid Z63_TOK_SETUP/IN/OUT on endpoint `nr`,
 * `len` bytes at `buf` (SETUP: the eight-byte request). */
static int z63_dwc3_token(Z63Dwc3 *s, int pid, unsigned nr, uint8_t *buf,
                          uint32_t len, uint32_t *moved_out)
{
    bool in = pid == Z63_TOK_IN;
    unsigned phys = Z63_DWC3_EPS;
    Z63Dwc3Ep *ep;
    unsigned moved = 0, status = 0;
    bool complete = false;

    *moved_out = 0;
    if (!s->on_bus || nr > 15) {
        return Z63_TOK_STALL;
    }
    if (nr == 0) {
        phys = in ? 1 : 0;
    } else {
        for (unsigned i = 2; i < Z63_DWC3_EPS; i++) {
            if (s->ep[i].configured && s->ep[i].usb_num == nr &&
                s->ep[i].usb_in == in) {
                phys = i;
                break;
            }
        }
    }
    if (!(s->dalepena & (1u << (2 * nr + in))) || phys >= Z63_DWC3_EPS) {
        return Z63_TOK_NAK;
    }
    ep = &s->ep[phys];
    if (pid == Z63_TOK_SETUP) {
        s->ep[0].stalled = s->ep[1].stalled = false;
    }
    if (ep->stalled) {
        return Z63_TOK_STALL;
    }
    if (!ep->trb) {
        if (nr == 0 && !ep->asked) {
            ep->asked = true;
            z63_dwc3_depevt(s, phys, Z63_DEPEVT_XFERNOTREADY,
                            len ? Z63_DEPEVT_STATUS_CONTROL_DATA
                                : Z63_DEPEVT_STATUS_CONTROL_STATUS, 0);
        }
        return Z63_TOK_NAK;
    }
    if (!z63_dwc3_run_trbs(s, phys, in, buf, len, &moved, &status,
                           &complete)) {
        if (nr == 0 && !ep->asked) {
            ep->asked = true;
            z63_dwc3_depevt(s, phys, Z63_DEPEVT_XFERNOTREADY, 0, 0);
        }
        return Z63_TOK_NAK;
    }
    ep->asked = false;
    s->transfers++;
    *moved_out = moved;
    Z63_DWC3_LOG(s, "ep %u moved %u of %u byte(s)%s", phys, moved, len,
                 complete ? ", transfer done" : "");
    if (complete) {
        z63_dwc3_depevt(s, phys, Z63_DEPEVT_XFERCOMPLETE, status, 0);
    }
    return Z63_TOK_OK;
}

/* The host reset the port: high speed, address 0, Reset and ConnectDone
 * posted -- the latter is what has the driver configure endpoint 0. */
static void z63_dwc3_bus_reset(Z63Dwc3 *s)
{
    for (unsigned i = 0; i < Z63_DWC3_EPS; i++) {
        s->ep[i].stalled = s->ep[i].asked = false;
    }
    s->regs[Z63_DWC3_DCFG / 4] &= ~(0x7fu << 3);
    s->dsts &= ~7u;
    z63_dwc3_devt(s, Z63_DEVT_RESET, 0);
    z63_dwc3_devt(s, Z63_DEVT_CONNECTDONE, 0);
}

static uint64_t z63_dwc3_read(void *opaque, hwaddr off, unsigned size)
{
    Z63Dwc3 *s = opaque;

    if (size != 4 || (off & 3) || !z63_dwc3_offset_known(off)) {
        z63_stop_read("DWC3", Z63_DWC3_BASE + off, size);
        return 0;
    }
    if (off >= Z63_DWC3_GEVNTADRLO && off <= Z63_DWC3_GEVNTCOUNT) {
        switch (off - Z63_DWC3_GEVNTADRLO) {
        case 0x0: return s->evt[0].adrlo;
        case 0x4: return s->evt[0].adrhi;
        case 0x8: return s->evt[0].size;
        default:  return s->evt[0].count & Z63_DWC3_GEVNTCOUNT_MASK;
        }
    }
    if (off >= Z63_DWC3_DEP_BASE && off < Z63_DWC3_DEP_END) {
        Z63Dwc3Ep *ep = &s->ep[(off - Z63_DWC3_DEP_BASE) / 0x10];

        switch (off & 0xc) {
        case 0x0: return ep->par2;
        case 0x4: return ep->par1;
        case 0x8: return ep->par0;
        default:  return ep->cmd;
        }
    }
    switch (off) {
    case Z63_DWC3_DCTL:
        return s->dctl;
    case Z63_DWC3_DSTS:
        return s->dsts |
               (((qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
                  Z63_DWC3_MICROFRAME_NS) & Z63_DWC3_DSTS_SOFFN_MASK)
                << Z63_DWC3_DSTS_SOFFN_SHIFT);
    case Z63_DWC3_DALEPENA:
        return s->dalepena;
    default:
        return s->regs[off / 4];
    }
}

static void z63_dwc3_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    Z63Dwc3 *s = opaque;
    uint32_t v = value;

    if (size != 4 || (off & 3) || !z63_dwc3_offset_known(off) ||
        off == Z63_DWC3_DSTS) {
        z63_stop_write("DWC3", Z63_DWC3_BASE + off, size, value);
        return;
    }
    if (off >= Z63_DWC3_GEVNTADRLO && off <= Z63_DWC3_GEVNTCOUNT) {
        switch (off - Z63_DWC3_GEVNTADRLO) {
        case 0x0: s->evt[0].adrlo = v; s->evt[0].put = 0; break;
        case 0x4: s->evt[0].adrhi = v; s->evt[0].put = 0; break;
        case 0x8:
            /*
             * The ISR masks and EXSUSBDP unmasks through this register
             * on every batch: only a new size starts the ring over. (A
             * reset here put each batch's events back at the start of
             * the ring while the driver read on from where it was, and
             * enumeration stopped at the first status stage.)
             */
            if ((v ^ s->evt[0].size) & Z63_DWC3_GEVNTSIZ_MASK) {
                s->evt[0].put = 0;
            }
            s->evt[0].size = v;
            break;
        default:
            Z63_DWC3_LOG(s, "driver took %u of %u byte(s)",
                         v & Z63_DWC3_GEVNTCOUNT_MASK, s->evt[0].count);
            s->evt[0].count -= MIN(v & Z63_DWC3_GEVNTCOUNT_MASK,
                                   s->evt[0].count);
            break;
        }
        z63_dwc3_update_irq(s, 0);
        return;
    }
    if (off >= Z63_DWC3_DEP_BASE && off < Z63_DWC3_DEP_END) {
        unsigned n = (off - Z63_DWC3_DEP_BASE) / 0x10;
        Z63Dwc3Ep *ep = &s->ep[n];

        switch (off & 0xc) {
        case 0x0: ep->par2 = v; break;
        case 0x4: ep->par1 = v; break;
        case 0x8: ep->par0 = v; break;
        default:
            if (v & Z63_DWC3_CMDACT) {
                z63_dwc3_depcmd(s, n, v);
            } else {
                ep->cmd = v;
            }
            break;
        }
        return;
    }
    switch (off) {
    case Z63_DWC3_DCTL:
        if (v & Z63_DWC3_DCTL_CSFTRST) {
            for (unsigned i = 0; i < Z63_DWC3_EPS; i++) {
                z63_dwc3_ep_reset(s, i);
            }
            v &= ~Z63_DWC3_DCTL_CSFTRST;
        }
        if ((v ^ s->dctl) & Z63_DWC3_DCTL_RUN_STOP) {
            z63_dwc3_set_running(s, v & Z63_DWC3_DCTL_RUN_STOP);
        }
        s->dctl = v;
        break;
    case Z63_DWC3_DGCMD:
        s->regs[off / 4] = v & ~Z63_DWC3_CMDACT;    /* done at once */
        break;
    case Z63_DWC3_DALEPENA:
        s->dalepena = v;
        break;
    default:
        s->regs[off / 4] = v;
        break;
    }
}

static const MemoryRegionOps z63_dwc3_dev_ops = {
    .read = z63_dwc3_read,
    .write = z63_dwc3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void z63_dwc3_reset(Z63Dwc3 *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->evt, 0, sizeof(s->evt));
    memset(s->ep, 0, sizeof(s->ep));
    s->dctl = s->dalepena = 0;
    s->on_bus = false;
    s->dsts = Z63_DWC3_DSTS_DEVCTRLHLT;
    qemu_set_irq(s->irq, 0);
}

#endif /* HW_ARM_NIKON_Z63_DWC3_H */
